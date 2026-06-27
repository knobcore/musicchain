#include "swarm.h"
#include "../crypto/hash.h"
#include "../storage/database.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>

namespace mc::store {

namespace {

// Wire-prefix for swarm entries. Key shape:
//   "sw:" + canonical_hash_hex (64) + ":" + peer_id (40)
// Value (text, '|' separated):
//   <local_content_hash_hex(64)>|<bitrate(uint32)>|<audio_format(0|1)>|<last_seen_ms>
//
// Old-format records that wrote an empty value (peer_id was the only
// data) still load with defaults. New writes always include the full
// 4-field payload.
constexpr const char* kSwarmPrefix = "sw:";

inline std::string make_key(const std::string& canonical_hex,
                            const std::string& peer_id) {
    std::string k;
    k.reserve(3 + canonical_hex.size() + 1 + peer_id.size());
    k.append(kSwarmPrefix);
    k.append(canonical_hex);
    k.push_back(':');
    k.append(peer_id);
    return k;
}

std::string encode_value(const SwarmMember& m) {
    std::ostringstream out;
    out << crypto::to_hex(m.content_hash) << '|'
        << m.bitrate << '|'
        << static_cast<int>(m.audio_format) << '|'
        << m.last_seen_ms;
    return out.str();
}

bool decode_value(const std::string& s, SwarmMember& m_inout) {
    // Empty value = old single-string format; leave defaults but mark
    // as just-seen so the next prune cycle doesn't immediately drop it.
    if (s.empty()) return true;
    size_t a = s.find('|');
    if (a == std::string::npos) return false;
    Hash256 ch{};
    if (!crypto::parse_hash256(s.substr(0, a), ch)) return false;
    m_inout.content_hash = ch;
    size_t b = s.find('|', a + 1);
    if (b == std::string::npos) return false;
    try {
        m_inout.bitrate = static_cast<uint32_t>(std::stoul(s.substr(a + 1, b - a - 1)));
    } catch (...) { return false; }
    size_t c = s.find('|', b + 1);
    try {
        int fmt = std::stoi(s.substr(b + 1, c == std::string::npos ? std::string::npos : c - b - 1));
        // Trust whatever byte we wrote; any unknown future value falls
        // back to OGG on the consumer side so the swarm join still works
        // even if the SwarmIndex format here predates a new format byte.
        if (fmt >= 0 && fmt <= 0xFF) {
            m_inout.audio_format = static_cast<AudioFormat>(fmt);
        } else {
            m_inout.audio_format = AudioFormat::OGG;
        }
    } catch (...) { return false; }
    if (c == std::string::npos) return true;
    try {
        m_inout.last_seen_ms = std::stoull(s.substr(c + 1));
    } catch (...) {/* leave 0 */}
    return true;
}

} // namespace

uint64_t SwarmIndex::now_ms_() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

bool SwarmIndex::is_fresh_(const SwarmMember& m) const {
    if (m.last_seen_ms == 0) return true;  // legacy entries are kept
    const uint64_t now = now_ms_();
    return (now <= m.last_seen_ms) || (now - m.last_seen_ms <= kStaleAfterMs);
}

// Strict: a peer's entries are visible to members() / song_count only
// when that peer is currently online via librats. No TTL fallback —
// the Discover page is explicitly "online files only" per the product
// requirement, and the timely connect/disconnect signal pipeline
// (direct callbacks for direct peers, mini-node forwarded
// swarm.peer_online / swarm.peer_offline events for relayed peers)
// is what keeps the index honest.
bool SwarmIndex::peer_available_(const std::string& peer_id,
                                  const SwarmMember& /*m*/) const {
    if (peer_id.empty()) return false;
    return online_peers_.count(peer_id) > 0;
}

void SwarmIndex::index_member_(const std::string& canonical_hex,
                                const std::string& peer_id) {
    peer_to_hashes_[peer_id].insert(canonical_hex);
}

void SwarmIndex::unindex_member_(const std::string& canonical_hex,
                                  const std::string& peer_id) {
    auto it = peer_to_hashes_.find(peer_id);
    if (it == peer_to_hashes_.end()) return;
    it->second.erase(canonical_hex);
    if (it->second.empty()) peer_to_hashes_.erase(it);
}

std::string SwarmIndex::peer_digest_unlocked_(const std::string& peer_id) const {
    auto it = peer_to_hashes_.find(peer_id);
    if (it == peer_to_hashes_.end() || it->second.empty()) return {};
    std::vector<std::string> sorted;
    sorted.reserve(it->second.size());
    for (const auto& h : it->second) sorted.push_back(h);
    std::sort(sorted.begin(), sorted.end());
    // Hash the raw 32-byte canonical bytes (decoded from hex) so the
    // digest is independent of upper/lower-case representation. Player
    // computes the same digest with sorted lowercase hex → bytes → SHA256.
    std::vector<uint8_t> concat;
    concat.reserve(sorted.size() * 32);
    for (const auto& h : sorted) {
        Hash256 raw{};
        if (!crypto::parse_hash256(h, raw)) continue;
        concat.insert(concat.end(), raw.begin(), raw.end());
    }
    if (concat.empty()) return {};
    return crypto::to_hex(crypto::sha256(concat.data(), concat.size()));
}

void SwarmIndex::attach(Database& db) {
    std::lock_guard<std::mutex> lk(mu_);
    if (db_ != nullptr) return;
    db_ = &db;

    int loaded = 0;
    db.for_each_with_prefix(kSwarmPrefix, [&](const std::string& key,
                                              const std::string& val) {
        // key = "sw:" + canonical_hex(64) + ":" + peer_id(40) = 108 bytes
        if (key.size() < 3 + 64 + 1 + 1) return true;
        if (key[3 + 64] != ':') return true;
        const std::string canonical_hex = key.substr(3, 64);
        const std::string peer_id       = key.substr(3 + 64 + 1);
        SwarmMember m;
        m.peer_id = peer_id;
        decode_value(val, m); // tolerates empty/legacy values
        map_[canonical_hex].push_back(std::move(m));
        index_member_(canonical_hex, peer_id);
        ++loaded;
        return true;
    });
    std::cout << "[swarm] loaded " << loaded
              << " announced peers from leveldb\n";
}

void SwarmIndex::announce(const Hash256& canonical_hash,
                          const SwarmMember& member) {
    if (member.peer_id.empty()) return;
    SwarmMember stamped = member;
    if (stamped.last_seen_ms == 0) stamped.last_seen_ms = now_ms_();
    const std::string key = crypto::to_hex(canonical_hash);
    std::lock_guard<std::mutex> lk(mu_);
    auto& vec = map_[key];
    // Replace existing entry for this peer so a re-scan with updated
    // bitrate/format wins.
    for (auto& existing : vec) {
        if (existing.peer_id == stamped.peer_id) {
            existing = stamped;
            if (db_) {
                const std::string ev = encode_value(stamped);
                db_->put(make_key(key, stamped.peer_id),
                         std::vector<uint8_t>(ev.begin(), ev.end()));
            }
            return;
        }
    }
    vec.push_back(stamped);
    index_member_(key, stamped.peer_id);
    if (db_) {
        const std::string ev = encode_value(stamped);
        db_->put(make_key(key, stamped.peer_id),
                 std::vector<uint8_t>(ev.begin(), ev.end()));
    }
}

void SwarmIndex::touch_peer(const std::string& peer_id) {
    if (peer_id.empty()) return;
    const uint64_t now = now_ms_();
    std::lock_guard<std::mutex> lk(mu_);
    auto pit = peer_to_hashes_.find(peer_id);
    if (pit == peer_to_hashes_.end()) return;
    for (const auto& hash_hex : pit->second) {
        auto mit = map_.find(hash_hex);
        if (mit == map_.end()) continue;
        for (auto& m : mit->second) {
            if (m.peer_id != peer_id) continue;
            m.last_seen_ms = now;
            if (db_) {
                const std::string ev = encode_value(m);
                db_->put(make_key(hash_hex, peer_id),
                         std::vector<uint8_t>(ev.begin(), ev.end()));
            }
        }
    }
}

size_t SwarmIndex::prune_stale() {
    const uint64_t cutoff = now_ms_() > kStaleAfterMs
                                ? now_ms_() - kStaleAfterMs
                                : 0;
    size_t pruned = 0;
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = map_.begin(); it != map_.end();) {
        auto& vec = it->second;
        for (auto vit = vec.begin(); vit != vec.end();) {
            // Connection-state authoritative: only TTL-prune offline
            // peers. An online peer's last_seen_ms can be stale (we
            // stopped touching on every heartbeat) and that's fine —
            // their connection is the live signal.
            const bool offline = online_peers_.count(vit->peer_id) == 0;
            const bool stale   = offline
                              && (vit->last_seen_ms != 0)
                              && (vit->last_seen_ms < cutoff);
            if (stale) {
                if (db_) db_->del(make_key(it->first, vit->peer_id));
                unindex_member_(it->first, vit->peer_id);
                vit = vec.erase(vit);
                ++pruned;
            } else {
                ++vit;
            }
        }
        if (vec.empty()) it = map_.erase(it);
        else             ++it;
    }
    if (pruned > 0) {
        std::cout << "[swarm] pruned " << pruned
                  << " stale peers (>20 min idle, offline)\n";
    }
    return pruned;
}

void SwarmIndex::drop(const Hash256& canonical_hash,
                      const std::string& peer_id) {
    const std::string key = crypto::to_hex(canonical_hash);
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(key);
    if (it == map_.end()) return;
    auto& vec = it->second;
    vec.erase(std::remove_if(vec.begin(), vec.end(),
                             [&](const SwarmMember& m){ return m.peer_id == peer_id; }),
              vec.end());
    if (db_) db_->del(make_key(key, peer_id));
    unindex_member_(key, peer_id);
    if (vec.empty()) map_.erase(it);
}

void SwarmIndex::mark_peer_online(const std::string& peer_id) {
    if (peer_id.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    online_peers_.insert(peer_id);
}

void SwarmIndex::mark_peer_offline(const std::string& peer_id) {
    if (peer_id.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    online_peers_.erase(peer_id);
}

bool SwarmIndex::is_online(const std::string& peer_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    return online_peers_.count(peer_id) > 0;
}

void SwarmIndex::evict_peer(const std::string& peer_id) {
    if (peer_id.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    online_peers_.erase(peer_id);
    auto pit = peer_to_hashes_.find(peer_id);
    if (pit == peer_to_hashes_.end()) return;
    std::vector<std::string> hashes(pit->second.begin(), pit->second.end());
    size_t removed = 0;
    for (const auto& hash_hex : hashes) {
        auto it = map_.find(hash_hex);
        if (it == map_.end()) continue;
        auto& vec = it->second;
        const size_t before = vec.size();
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                                 [&](const SwarmMember& m){
                                     return m.peer_id == peer_id;
                                 }),
                  vec.end());
        removed += (before - vec.size());
        if (db_) db_->del(make_key(hash_hex, peer_id));
        if (vec.empty()) map_.erase(it);
    }
    peer_to_hashes_.erase(peer_id);
    if (removed > 0) {
        std::cout << "[swarm] evicted peer " << peer_id.substr(0, 12)
                  << " (" << removed << " entries dropped)\n";
    }
}

size_t SwarmIndex::drop_peer(const std::string& peer_id) {
    if (peer_id.empty()) return 0;
    std::lock_guard<std::mutex> lk(mu_);
    auto pit = peer_to_hashes_.find(peer_id);
    if (pit == peer_to_hashes_.end()) return 0;
    std::vector<std::string> hashes(pit->second.begin(), pit->second.end());
    size_t n = 0;
    for (const auto& hash_hex : hashes) {
        auto it = map_.find(hash_hex);
        if (it == map_.end()) continue;
        auto& vec = it->second;
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                                 [&](const SwarmMember& m){
                                     return m.peer_id == peer_id;
                                 }),
                  vec.end());
        if (db_) db_->del(make_key(hash_hex, peer_id));
        ++n;
        if (vec.empty()) map_.erase(it);
    }
    peer_to_hashes_.erase(peer_id);
    return n;
}

std::string SwarmIndex::replace_peer(
    const std::string& peer_id,
    const std::vector<std::pair<Hash256, SwarmMember>>& members) {
    if (peer_id.empty()) return {};
    std::lock_guard<std::mutex> lk(mu_);
    // Tear down the existing membership in one shot. Reuses the same
    // path as drop_peer so the reverse index + DB stay in sync.
    auto pit = peer_to_hashes_.find(peer_id);
    if (pit != peer_to_hashes_.end()) {
        std::vector<std::string> hashes(pit->second.begin(),
                                        pit->second.end());
        for (const auto& hash_hex : hashes) {
            auto it = map_.find(hash_hex);
            if (it == map_.end()) continue;
            auto& vec = it->second;
            vec.erase(std::remove_if(vec.begin(), vec.end(),
                                     [&](const SwarmMember& m){
                                         return m.peer_id == peer_id;
                                     }),
                      vec.end());
            if (db_) db_->del(make_key(hash_hex, peer_id));
            if (vec.empty()) map_.erase(it);
        }
        peer_to_hashes_.erase(pit);
    }
    // Apply the new set.
    const uint64_t now = now_ms_();
    for (const auto& [canonical, mem_in] : members) {
        SwarmMember stamped = mem_in;
        stamped.peer_id = peer_id;
        if (stamped.last_seen_ms == 0) stamped.last_seen_ms = now;
        const std::string key = crypto::to_hex(canonical);
        map_[key].push_back(stamped);
        index_member_(key, peer_id);
        if (db_) {
            const std::string ev = encode_value(stamped);
            db_->put(make_key(key, peer_id),
                     std::vector<uint8_t>(ev.begin(), ev.end()));
        }
    }
    return peer_digest_unlocked_(peer_id);
}

std::string SwarmIndex::peer_digest(const std::string& peer_id) const {
    if (peer_id.empty()) return {};
    std::lock_guard<std::mutex> lk(mu_);
    return peer_digest_unlocked_(peer_id);
}

size_t SwarmIndex::peer_size(const std::string& peer_id) const {
    if (peer_id.empty()) return 0;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = peer_to_hashes_.find(peer_id);
    return it == peer_to_hashes_.end() ? 0 : it->second.size();
}

std::vector<SwarmMember> SwarmIndex::members(const Hash256& canonical_hash) const {
    const std::string key = crypto::to_hex(canonical_hash);
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(key);
    if (it == map_.end()) return {};
    std::vector<SwarmMember> out;
    out.reserve(it->second.size());
    for (const auto& m : it->second) {
        if (peer_available_(m.peer_id, m)) out.push_back(m);
    }
    return out;
}

size_t SwarmIndex::song_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    for (const auto& [_, vec] : map_) {
        for (const auto& m : vec) {
            if (peer_available_(m.peer_id, m)) { ++n; break; }
        }
    }
    return n;
}

} // namespace mc::store
