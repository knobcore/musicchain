#include "library_store.h"

#include "../storage/database.h"

#include "roaring.hh"           // roaring::Roaring (CRoaring C++ wrapper)

#include <leveldb/write_batch.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mc::store {

namespace {

// leveldb keyspace (separate from the chain's). 2-char prefixes:
//   Lc<hash:32>    -> id:u32        intern: content_hash -> local song id
//   Lp<wallet:20>  -> ord:u32       wallet -> local ordinal (for reverse roaring)
//   Lw<wallet:20>  -> ver:u64 | roaring(portable)   the library
constexpr char kPfxIntern[]   = "Lc";
constexpr char kPfxWalletOrd[] = "Lp";
constexpr char kPfxLibrary[]  = "Lw";

std::string raw_key(const char* pfx, const uint8_t* data, size_t n) {
    std::string k(pfx);
    k.append(reinterpret_cast<const char*>(data), n);
    return k;
}
std::string hkey(const Hash256& h) {
    return std::string(reinterpret_cast<const char*>(h.data()), h.size());
}
std::string akey(const Address& a) {
    return std::string(reinterpret_cast<const char*>(a.data()), a.size());
}
void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.resize(4);
    std::memcpy(v.data(), &x, 4);          // host LE; both consensus targets are x64 LE
}
uint32_t get_u32(const std::string& v) {
    uint32_t x = 0;
    if (v.size() >= 4) std::memcpy(&x, v.data(), 4);
    return x;
}

} // namespace

struct LibraryStore::Impl {
    Database* db = nullptr;
    mutable std::mutex mu;

    // intern: content_hash <-> local song id (id == index into id_to_hash)
    std::unordered_map<std::string, uint32_t> hash_to_id;
    std::vector<Hash256>                       id_to_hash;

    // wallet <-> local ordinal (reverse roaring keys are ordinals, not addrs)
    std::unordered_map<std::string, uint32_t>  wallet_to_ord;
    std::vector<Address>                       ord_to_wallet;

    // forward: wallet -> set of song ids + version
    std::unordered_map<std::string, roaring::Roaring> libs;
    std::unordered_map<std::string, uint64_t>          versions;

    // reverse (derived, rebuilt on attach): song id -> set of wallet ordinals
    std::unordered_map<uint32_t, roaring::Roaring>     holders;

    // --- intern helpers (assume mu held) ---
    uint32_t intern_hash(const Hash256& h, leveldb::WriteBatch& b) {
        const std::string k = hkey(h);
        auto it = hash_to_id.find(k);
        if (it != hash_to_id.end()) return it->second;
        const uint32_t id = static_cast<uint32_t>(id_to_hash.size());
        hash_to_id.emplace(k, id);
        id_to_hash.push_back(h);
        std::vector<uint8_t> v; put_u32(v, id);
        db->put_batch(b, raw_key(kPfxIntern, h.data(), h.size()), v);
        return id;
    }
    // read-only lookup (no assign)
    bool lookup_id(const Hash256& h, uint32_t& out) const {
        auto it = hash_to_id.find(hkey(h));
        if (it == hash_to_id.end()) return false;
        out = it->second;
        return true;
    }
    uint32_t intern_wallet(const Address& w, leveldb::WriteBatch& b) {
        const std::string k = akey(w);
        auto it = wallet_to_ord.find(k);
        if (it != wallet_to_ord.end()) return it->second;
        const uint32_t ord = static_cast<uint32_t>(ord_to_wallet.size());
        wallet_to_ord.emplace(k, ord);
        ord_to_wallet.push_back(w);
        std::vector<uint8_t> v; put_u32(v, ord);
        db->put_batch(b, raw_key(kPfxWalletOrd, w.data(), w.size()), v);
        return ord;
    }

    // serialize {version, roaring} into the Lw value (assume mu held)
    std::vector<uint8_t> encode_library(uint64_t version,
                                        const roaring::Roaring& r) const {
        std::vector<uint8_t> out(8);
        std::memcpy(out.data(), &version, 8);                 // host LE
        const size_t rsz = r.getSizeInBytes(true /*portable*/);
        out.resize(8 + rsz);
        r.write(reinterpret_cast<char*>(out.data()) + 8, true);
        return out;
    }
    void persist_library(leveldb::WriteBatch& b, const Address& w,
                         uint64_t version, const roaring::Roaring& r) const {
        const auto v = encode_library(version, r);
        db->put_batch(b, raw_key(kPfxLibrary, w.data(), w.size()), v);
    }
};

LibraryStore::LibraryStore() : p_(std::make_unique<Impl>()) {}
LibraryStore::~LibraryStore() = default;

void LibraryStore::attach(Database& db) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->db = &db;

    // 1. intern table: Lc<hash> -> id
    db.for_each_with_prefix(kPfxIntern, [&](const std::string& key,
                                            const std::string& val) {
        if (key.size() != 2 + 32) return true;
        Hash256 h{}; std::memcpy(h.data(), key.data() + 2, 32);
        const uint32_t id = get_u32(val);
        p_->hash_to_id.emplace(std::string(key.data() + 2, 32), id);
        if (id >= p_->id_to_hash.size()) p_->id_to_hash.resize(id + 1);
        p_->id_to_hash[id] = h;
        return true;
    });
    // 2. wallet ordinals: Lp<wallet> -> ord
    db.for_each_with_prefix(kPfxWalletOrd, [&](const std::string& key,
                                               const std::string& val) {
        if (key.size() != 2 + 20) return true;
        Address w{}; std::memcpy(w.data(), key.data() + 2, 20);
        const uint32_t ord = get_u32(val);
        p_->wallet_to_ord.emplace(std::string(key.data() + 2, 20), ord);
        if (ord >= p_->ord_to_wallet.size()) p_->ord_to_wallet.resize(ord + 1);
        p_->ord_to_wallet[ord] = w;
        return true;
    });
    // 3. libraries: Lw<wallet> -> {version, roaring}; rebuild reverse index.
    db.for_each_with_prefix(kPfxLibrary, [&](const std::string& key,
                                             const std::string& val) {
        if (key.size() != 2 + 20 || val.size() < 8) return true;
        const std::string wkey(key.data() + 2, 20);
        uint64_t version = 0; std::memcpy(&version, val.data(), 8);
        roaring::Roaring r =
            roaring::Roaring::readSafe(val.data() + 8, val.size() - 8);
        // wallet ordinal for the reverse index
        auto ow = p_->wallet_to_ord.find(wkey);
        const bool have_ord = ow != p_->wallet_to_ord.end();
        for (uint32_t id : r) {
            if (have_ord) p_->holders[id].add(ow->second);
        }
        p_->versions[wkey] = version;
        p_->libs.emplace(wkey, std::move(r));
        return true;
    });
}

bool LibraryStore::set_library(const Address& wallet,
                               const std::vector<Hash256>& hashes,
                               uint64_t version) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->db) return false;
    const std::string wkey = akey(wallet);
    // Version-gated SNAPSHOT replace: only a strictly-newer full set applies
    // (gossip re-delivery / out-of-order arrival drop here). Replacing rather
    // than adding means removals propagate too.
    auto vit = p_->versions.find(wkey);
    if (vit != p_->versions.end() && version <= vit->second) return false;

    leveldb::WriteBatch batch;
    const uint32_t ord = p_->intern_wallet(wallet, batch);

    // Build the new id set.
    roaring::Roaring next;
    for (const auto& h : hashes) next.add(p_->intern_hash(h, batch));

    // Reverse-index diff: drop ords for removed ids, add for new ones.
    auto it = p_->libs.find(wkey);
    if (it != p_->libs.end()) {
        for (uint32_t id : it->second)
            if (!next.contains(id)) p_->holders[id].remove(ord);
    }
    for (uint32_t id : next) p_->holders[id].add(ord);

    p_->versions[wkey] = version;
    p_->persist_library(batch, wallet, version, next);
    p_->libs[wkey] = std::move(next);
    p_->db->write(batch);
    return true;
}

bool LibraryStore::apply_delta(const Address& wallet,
                               const std::vector<Hash256>& add,
                               const std::vector<Hash256>& remove,
                               uint64_t version) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->db) return false;
    const std::string wkey = akey(wallet);
    // Idempotent / monotonic: only newer versions apply (gossip re-delivery,
    // out-of-order arrival, and self-echo are all dropped here).
    auto vit = p_->versions.find(wkey);
    if (vit != p_->versions.end() && version <= vit->second) return false;

    leveldb::WriteBatch batch;
    const uint32_t ord = p_->intern_wallet(wallet, batch);
    roaring::Roaring& r = p_->libs[wkey];   // default-constructs if absent

    for (const auto& h : remove) {
        uint32_t id;
        if (p_->lookup_id(h, id) && r.contains(id)) {
            r.remove(id);
            auto hit = p_->holders.find(id);
            if (hit != p_->holders.end()) hit->second.remove(ord);
        }
    }
    for (const auto& h : add) {
        const uint32_t id = p_->intern_hash(h, batch);
        r.add(id);
        p_->holders[id].add(ord);
    }

    p_->versions[wkey] = version;
    p_->persist_library(batch, wallet, version, r);
    p_->db->write(batch);
    return true;
}

std::vector<Hash256> LibraryStore::library(const Address& wallet) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::vector<Hash256> out;
    auto it = p_->libs.find(akey(wallet));
    if (it == p_->libs.end()) return out;
    out.reserve(it->second.cardinality());
    for (uint32_t id : it->second)
        if (id < p_->id_to_hash.size()) out.push_back(p_->id_to_hash[id]);
    return out;
}

size_t LibraryStore::library_size(const Address& wallet) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    auto it = p_->libs.find(akey(wallet));
    return it == p_->libs.end() ? 0 : static_cast<size_t>(it->second.cardinality());
}

uint64_t LibraryStore::library_version(const Address& wallet) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    auto it = p_->versions.find(akey(wallet));
    return it == p_->versions.end() ? 0 : it->second;
}

std::vector<Address> LibraryStore::holders(const Hash256& ch) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::vector<Address> out;
    uint32_t id;
    if (!p_->lookup_id(ch, id)) return out;
    auto it = p_->holders.find(id);
    if (it == p_->holders.end()) return out;
    out.reserve(it->second.cardinality());
    for (uint32_t ord : it->second)
        if (ord < p_->ord_to_wallet.size()) out.push_back(p_->ord_to_wallet[ord]);
    return out;
}

size_t LibraryStore::holder_count(const Hash256& ch) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    uint32_t id;
    if (!p_->lookup_id(ch, id)) return 0;
    auto it = p_->holders.find(id);
    return it == p_->holders.end() ? 0 : static_cast<size_t>(it->second.cardinality());
}

namespace {
// Lowercase-hex of n bytes (no 0x). Local so library_store.cpp keeps its
// minimal dependency surface (it deliberately doesn't pull in crypto/hash.h).
std::string to_hex_local(const uint8_t* d, size_t n) {
    static const char* k = "0123456789abcdef";
    std::string s; s.resize(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s[2 * i]     = k[(d[i] >> 4) & 0xF];
        s[2 * i + 1] = k[d[i] & 0xF];
    }
    return s;
}
// Decode a 40-char wallet hex into the 20-byte raw key used by libs/wallet
// maps. Returns false on any malformed input (skips that wallet).
bool wallet_hex_to_raw(const std::string& hex, std::string& out) {
    if (hex.size() != 40) return false;
    out.resize(20);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < 20; ++i) {
        int hi = nib(hex[2 * i]), lo = nib(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<char>((hi << 4) | lo);
    }
    return true;
}
} // namespace

void LibraryStore::online_snapshot(
        const std::unordered_set<std::string>& live_wallets,
        std::unordered_set<std::string>& online_hashes,
        std::unordered_map<std::string, size_t>& holder_counts) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    // Single pass over ONLY the live wallets' forward libraries — not every
    // song crossed with every holder. For each live wallet we walk its Roaring
    // id set once, map id -> content_hash hex, and bump the per-hash live
    // holder count (presence in the map also serves as the online-hash set).
    std::string wraw;
    for (const auto& whex : live_wallets) {
        if (!wallet_hex_to_raw(whex, wraw)) continue;
        auto lit = p_->libs.find(wraw);
        if (lit == p_->libs.end()) continue;
        for (uint32_t id : lit->second) {
            if (id >= p_->id_to_hash.size()) continue;
            const Hash256& h = p_->id_to_hash[id];
            std::string hhex = to_hex_local(h.data(), h.size());
            ++holder_counts[hhex];
            online_hashes.insert(std::move(hhex));
        }
    }
}

size_t LibraryStore::wallet_count() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->libs.size();
}

size_t LibraryStore::catalog_size() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->id_to_hash.size();
}

// ---- playlists ---------------------------------------------------------
// leveldb-backed, on-demand (no in-memory cache). Key: "Pp"<wallet:20><id:16>.
// Value: ver:u64 | deleted:u8 | name_len:u16 | name | count:u32 | songs(32·n).
// Ordered (NOT a Roaring set); hashes stored inline so playlists don't depend
// on the intern table. Version-gated for gossip convergence (incl. tombstones).
namespace {
void w2(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x & 0xFF)); v.push_back(uint8_t((x >> 8) & 0xFF));
}
void w4(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t((x >> (8 * i)) & 0xFF));
}
void w8(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(uint8_t((x >> (8 * i)) & 0xFF));
}
uint16_t r2(const std::string& s, size_t o) {
    return (o + 2 <= s.size())
        ? uint16_t(uint8_t(s[o]) | (uint16_t(uint8_t(s[o + 1])) << 8)) : 0;
}
uint32_t r4(const std::string& s, size_t o) {
    uint32_t x = 0;
    if (o + 4 <= s.size())
        for (int i = 0; i < 4; ++i) x |= uint32_t(uint8_t(s[o + i])) << (8 * i);
    return x;
}
uint64_t r8(const std::string& s, size_t o) {
    uint64_t x = 0;
    if (o + 8 <= s.size())
        for (int i = 0; i < 8; ++i) x |= uint64_t(uint8_t(s[o + i])) << (8 * i);
    return x;
}
std::string pl_key(const Address& w, const std::array<uint8_t, 16>& id) {
    std::string k = "Pp";
    k.append(reinterpret_cast<const char*>(w.data()), w.size());
    k.append(reinterpret_cast<const char*>(id.data()), id.size());
    return k;
}
} // namespace

bool LibraryStore::set_playlist(const Address& wallet,
                                const std::array<uint8_t, 16>& id,
                                const std::string& name,
                                const std::vector<Hash256>& songs,
                                uint64_t version) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->db) return false;
    const std::string key = pl_key(wallet, id);
    if (auto cur = p_->db->get(key)) {
        const std::string s(reinterpret_cast<const char*>(cur->data()), cur->size());
        if (version <= r8(s, 0)) return false;   // monotonic version gate
    }
    const uint16_t nlen =
        static_cast<uint16_t>(std::min<size_t>(name.size(), 0xFFFF));
    std::vector<uint8_t> v;
    w8(v, version);
    v.push_back(0);                              // deleted = false
    w2(v, nlen);
    v.insert(v.end(), name.begin(), name.begin() + nlen);
    w4(v, static_cast<uint32_t>(songs.size()));
    for (const auto& h : songs) v.insert(v.end(), h.begin(), h.end());
    return p_->db->put(key, v);
}

bool LibraryStore::delete_playlist(const Address& wallet,
                                   const std::array<uint8_t, 16>& id,
                                   uint64_t version) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->db) return false;
    const std::string key = pl_key(wallet, id);
    if (auto cur = p_->db->get(key)) {
        const std::string s(reinterpret_cast<const char*>(cur->data()), cur->size());
        if (version <= r8(s, 0)) return false;
    }
    std::vector<uint8_t> v;
    w8(v, version);
    v.push_back(1);                              // deleted = true (tombstone)
    w2(v, 0);
    w4(v, 0);
    return p_->db->put(key, v);
}

static bool decode_playlist(const std::string& s, LibraryStore::Playlist& pl) {
    if (s.size() < 9) return false;
    pl.version = r8(s, 0);
    pl.deleted = s[8] != 0;
    if (pl.deleted) return false;                // hidden from queries
    size_t off = 9;
    const uint16_t nlen = r2(s, off); off += 2;
    if (off + nlen > s.size()) return false;
    pl.name.assign(s.data() + off, nlen); off += nlen;
    const uint32_t count = r4(s, off); off += 4;
    for (uint32_t i = 0; i < count; ++i) {
        if (off + 32 > s.size()) break;
        Hash256 h{}; std::memcpy(h.data(), s.data() + off, 32); off += 32;
        pl.songs.push_back(h);
    }
    return true;
}

std::optional<LibraryStore::Playlist> LibraryStore::get_playlist(
        const Address& wallet, const std::array<uint8_t, 16>& id) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->db) return std::nullopt;
    auto cur = p_->db->get(pl_key(wallet, id));
    if (!cur) return std::nullopt;
    const std::string s(reinterpret_cast<const char*>(cur->data()), cur->size());
    Playlist pl; pl.id = id;
    if (!decode_playlist(s, pl)) return std::nullopt;
    return pl;
}

std::vector<LibraryStore::Playlist> LibraryStore::list_playlists(
        const Address& wallet) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::vector<Playlist> out;
    if (!p_->db) return out;
    std::string prefix = "Pp";
    prefix.append(reinterpret_cast<const char*>(wallet.data()), wallet.size());
    p_->db->for_each_with_prefix(prefix, [&](const std::string& key,
                                             const std::string& s) {
        if (key.size() != 2 + 20 + 16) return true;
        Playlist pl;
        std::memcpy(pl.id.data(), key.data() + 2 + 20, 16);
        if (decode_playlist(s, pl)) out.push_back(std::move(pl));
        return true;
    });
    return out;
}

// ---- anti-entropy: stored signed payloads ------------------------------
void LibraryStore::store_library_payload(const Address& w, uint64_t version,
                                         const std::string& payload) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->db) return;
    std::string key = "Ls";
    key.append(reinterpret_cast<const char*>(w.data()), w.size());
    std::vector<uint8_t> v; v.reserve(8 + payload.size());
    w8(v, version);
    v.insert(v.end(), payload.begin(), payload.end());
    p_->db->put(key, v);
}

void LibraryStore::store_playlist_payload(const Address& w,
                                          const std::array<uint8_t, 16>& id,
                                          uint64_t version,
                                          const std::string& payload) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->db) return;
    std::string key = "Ps";
    key.append(reinterpret_cast<const char*>(w.data()), w.size());
    key.append(reinterpret_cast<const char*>(id.data()), id.size());
    std::vector<uint8_t> v; v.reserve(8 + payload.size());
    w8(v, version);
    v.insert(v.end(), payload.begin(), payload.end());
    p_->db->put(key, v);
}

void LibraryStore::put_manifest(const Hash256& ch,
                                const std::string& manifest_json) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->db) return;
    std::string key = "Lm";
    key.append(reinterpret_cast<const char*>(ch.data()), ch.size());
    std::vector<uint8_t> v(manifest_json.begin(), manifest_json.end());
    p_->db->put(key, v);
}

std::optional<std::string> LibraryStore::get_manifest(const Hash256& ch) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->db) return std::nullopt;
    std::string key = "Lm";
    key.append(reinterpret_cast<const char*>(ch.data()), ch.size());
    auto v = p_->db->get(key);
    if (!v) return std::nullopt;
    return std::string(v->begin(), v->end());
}

void LibraryStore::for_each_library_payload(
        const std::function<void(const Address&, uint64_t,
                                 const std::string&)>& cb) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->db) return;
    p_->db->for_each_with_prefix("Ls", [&](const std::string& key,
                                           const std::string& val) {
        if (key.size() != 2 + 20 || val.size() < 8) return true;
        Address w{}; std::memcpy(w.data(), key.data() + 2, 20);
        cb(w, r8(val, 0), val.substr(8));
        return true;
    });
}

void LibraryStore::for_each_playlist_payload(
        const std::function<void(const Address&, const std::array<uint8_t, 16>&,
                                 uint64_t, const std::string&)>& cb) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->db) return;
    p_->db->for_each_with_prefix("Ps", [&](const std::string& key,
                                           const std::string& val) {
        if (key.size() != 2 + 20 + 16 || val.size() < 8) return true;
        Address w{}; std::memcpy(w.data(), key.data() + 2, 20);
        std::array<uint8_t, 16> id{};
        std::memcpy(id.data(), key.data() + 2 + 20, 16);
        cb(w, id, r8(val, 0), val.substr(8));
        return true;
    });
}

} // namespace mc::store
