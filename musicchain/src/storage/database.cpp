#include "database.h"
#include "../crypto/hash.h"
#include "../audio/fingerprint.h"
#include <leveldb/cache.h>
#include <leveldb/filter_policy.h>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <stdexcept>

namespace mc {

// ---- SongState ------------------------------------------------------

std::vector<uint8_t> SongState::serialize() const {
    std::vector<uint8_t> buf(40);
    for (int i = 0; i < 8; ++i) buf[i]    = (play_count >> (8*i)) & 0xFF;
    std::memcpy(buf.data() + 8, discoverer_address.data(), 20);
    for (int i = 0; i < 4; ++i) buf[28+i] = (first_play_block >> (8*i)) & 0xFF;
    for (int i = 0; i < 8; ++i) buf[32+i] = (first_play_timestamp >> (8*i)) & 0xFF;
    return buf;
}

bool SongState::deserialize(const uint8_t* data, size_t len, SongState& out) {
    if (len < 40) return false;
    out.play_count = 0;
    for (int i = 0; i < 8; ++i) out.play_count |= (static_cast<uint64_t>(data[i]) << (8*i));
    std::memcpy(out.discoverer_address.data(), data + 8, 20);
    out.first_play_block = 0;
    for (int i = 0; i < 4; ++i) out.first_play_block |= (static_cast<uint32_t>(data[28+i]) << (8*i));
    out.first_play_timestamp = 0;
    for (int i = 0; i < 8; ++i) out.first_play_timestamp |= (static_cast<uint64_t>(data[32+i]) << (8*i));
    return true;
}

// ---- Database -------------------------------------------------------

Database::Database(const std::string& path) : path_(path) {
    leveldb::Options opts;
    opts.create_if_missing    = true;
    opts.write_buffer_size    = 64 * 1024 * 1024; // 64 MiB
    opts.block_cache          = leveldb::NewLRUCache(256 * 1024 * 1024); // 256 MiB
    opts.filter_policy        = leveldb::NewBloomFilterPolicy(10);
    opts.compression          = leveldb::kSnappyCompression;

    leveldb::Status s = leveldb::DB::Open(opts, path, &db_);
    if (!s.ok()) throw std::runtime_error("LevelDB open failed: " + s.ToString());
}

Database::~Database() {
    delete db_;
}

std::optional<std::vector<uint8_t>> Database::get(const std::string& key) const {
    std::string val;
    auto s = db_->Get(leveldb::ReadOptions(), key, &val);
    if (s.IsNotFound()) return std::nullopt;
    if (!s.ok()) return std::nullopt;
    return std::vector<uint8_t>(val.begin(), val.end());
}

bool Database::put(const std::string& key, const std::vector<uint8_t>& value) {
    auto s = db_->Put(leveldb::WriteOptions(),
                      key,
                      leveldb::Slice(reinterpret_cast<const char*>(value.data()), value.size()));
    return s.ok();
}

bool Database::del(const std::string& key) {
    auto s = db_->Delete(leveldb::WriteOptions(), key);
    return s.ok();
}

bool Database::write(leveldb::WriteBatch& batch) {
    leveldb::WriteOptions opts;
    opts.sync = false; // batched writes are atomic without sync
    auto s = db_->Write(opts, &batch);
    return s.ok();
}

void Database::put_batch(leveldb::WriteBatch& b, const std::string& key,
                         const std::vector<uint8_t>& value) {
    b.Put(key, leveldb::Slice(reinterpret_cast<const char*>(value.data()), value.size()));
}

void Database::del_batch(leveldb::WriteBatch& b, const std::string& key) {
    b.Delete(key);
}

void Database::put_batch_u32(leveldb::WriteBatch& b, const std::string& key, uint32_t v) {
    std::vector<uint8_t> buf(4);
    for (int i = 0; i < 4; ++i) buf[i] = (v >> (8*i)) & 0xFF;
    put_batch(b, key, buf);
}

void Database::put_batch_u64(leveldb::WriteBatch& b, const std::string& key, uint64_t v) {
    std::vector<uint8_t> buf(8);
    for (int i = 0; i < 8; ++i) buf[i] = (v >> (8*i)) & 0xFF;
    put_batch(b, key, buf);
}

std::optional<uint32_t> Database::get_u32(const std::string& key) const {
    auto v = get(key);
    if (!v || v->size() < 4) return std::nullopt;
    uint32_t r = 0;
    for (int i = 0; i < 4; ++i) r |= (static_cast<uint32_t>((*v)[i]) << (8*i));
    return r;
}

std::optional<uint64_t> Database::get_u64(const std::string& key) const {
    auto v = get(key);
    if (!v || v->size() < 8) return std::nullopt;
    uint64_t r = 0;
    for (int i = 0; i < 8; ++i) r |= (static_cast<uint64_t>((*v)[i]) << (8*i));
    return r;
}

// ---- Song state -----------------------------------------------------

SongState Database::get_song_state(const Hash256& content_hash) const {
    auto v = get("s:" + hex(content_hash));
    if (!v) return {};
    SongState state;
    SongState::deserialize(v->data(), v->size(), state);
    return state;
}

void Database::set_song_state(leveldb::WriteBatch& b, const Hash256& content_hash,
                               const SongState& state) {
    put_batch(b, "s:" + hex(content_hash), state.serialize());
}

uint64_t Database::get_play_count(const Hash256& content_hash) const {
    return get_song_state(content_hash).play_count;
}

void Database::update_song_state(leveldb::WriteBatch& b, const PlayProof& proof,
                                  uint64_t play_count_before) {
    SongState state = get_song_state(proof.content_hash);
    state.play_count++;
    if (play_count_before == 0) {
        state.discoverer_address  = proof.player_address;
        state.first_play_block    = 0; // will be set by chain
        state.first_play_timestamp = proof.play_start_timestamp;
    }
    set_song_state(b, proof.content_hash, state);
}

// ---- Balance ledger -------------------------------------------------

uint64_t Database::get_balance(const Address& address) const {
    auto v = get_u64("a:" + hex(address));
    return v.value_or(0);
}

void Database::set_balance(leveldb::WriteBatch& b, const Address& address, uint64_t balance) {
    put_batch_u64(b, "a:" + hex(address), balance);
}

uint64_t Database::get_total_supply() const {
    return get_u64("c:total_supply").value_or(0);
}

void Database::set_total_supply(leveldb::WriteBatch& b, uint64_t total) {
    put_batch_u64(b, "c:total_supply", total);
}

// ---- Sessions -------------------------------------------------------

bool Database::is_session_used(const Hash256& session_id) const {
    auto v = get("u:" + hex(session_id));
    return v.has_value();
}

void Database::mark_session_used(leveldb::WriteBatch& b, const Hash256& session_id) {
    put_batch(b, "u:" + hex(session_id), {});
}

// ---- Fingerprint index ----------------------------------------------

void Database::put_fingerprint(leveldb::WriteBatch& b, const SongSection& song) {
    // Store full fingerprint entry: f:{content_hash}
    auto fp = audio::Fingerprint::from_compressed(song.compressed_fingerprint);
    if (!fp) return;

    // Store entry: compressed_fingerprint length(2) + fingerprint + block_hash(32)
    std::vector<uint8_t> entry;
    auto comp = song.compressed_fingerprint;
    uint16_t flen = static_cast<uint16_t>(comp.size());
    entry.push_back(flen & 0xFF);
    entry.push_back((flen >> 8) & 0xFF);
    entry.insert(entry.end(), comp.begin(), comp.end());
    entry.insert(entry.end(), song.content_hash.begin(), song.content_hash.end());
    put_batch(b, "f:" + hex(song.content_hash), entry);

    // Reverse index: SHA256(compressed_fingerprint) -> content_hash.
    // Players that fingerprint local audio submit only the digest to ask
    // whether the chain knows that song; this index makes the answer O(1).
    //
    // Bug fix #20: this used to share the "h:" prefix with the block
    // height-by-hash table (chain.cpp:47). Same prefix + same-length
    // hex keys = two schemas living in the same keyspace. Moved to
    // "fph:" so they're physically distinct.
    const Hash256 fp_hash = crypto::sha256(
        reinterpret_cast<const uint8_t*>(comp.data()), comp.size());
    std::vector<uint8_t> fp_to_ch(song.content_hash.begin(),
                                  song.content_hash.end());
    put_batch(b, "fph:" + hex(fp_hash), fp_to_ch);

    // Update bucket inverted index
    for (auto bucket : fp->bucket_ids()) {
        add_to_bucket(b, bucket, song.content_hash);
    }
}

void Database::del_fingerprint(leveldb::WriteBatch& b, const Hash256& content_hash) {
    auto entry = get_fingerprint(content_hash);
    if (!entry) return;

    auto fp = audio::Fingerprint::from_compressed(entry->compressed_fingerprint);
    if (fp) {
        for (auto bucket : fp->bucket_ids())
            remove_from_bucket(b, bucket, content_hash);
    }
    del_batch(b, "f:" + hex(content_hash));

    // Drop the reverse index entry too.
    const Hash256 fp_hash = crypto::sha256(
        reinterpret_cast<const uint8_t*>(entry->compressed_fingerprint.data()),
        entry->compressed_fingerprint.size());
    del_batch(b, "fph:" + hex(fp_hash));
}

std::optional<Hash256> Database::get_content_hash_for_fingerprint(
    const Hash256& fingerprint_hash) const {
    auto v = get("fph:" + hex(fingerprint_hash));
    if (!v || v->size() != 32) return std::nullopt;
    Hash256 ch;
    std::memcpy(ch.data(), v->data(), 32);
    return ch;
}

std::optional<Database::FingerprintEntry> Database::get_fingerprint(const Hash256& content_hash) const {
    auto v = get("f:" + hex(content_hash));
    if (!v || v->size() < 34) return std::nullopt;
    const uint8_t* p = v->data();
    uint16_t flen = static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    if (v->size() < size_t(2 + flen + 32)) return std::nullopt;
    FingerprintEntry fe;
    fe.compressed_fingerprint = std::string(reinterpret_cast<const char*>(p + 2), flen);
    std::memcpy(fe.block_hash.data(), p + 2 + flen, 32);
    return fe;
}

std::string Database::bucket_key(uint16_t bucket_id) const {
    std::ostringstream oss;
    oss << "i:" << std::hex << std::setw(4) << std::setfill('0') << bucket_id;
    return oss.str();
}

std::vector<Hash256> Database::get_bucket(uint16_t bucket_id) const {
    auto v = get(bucket_key(bucket_id));
    if (!v) return {};
    size_t count = v->size() / 32;
    std::vector<Hash256> result(count);
    for (size_t i = 0; i < count; ++i)
        std::memcpy(result[i].data(), v->data() + i * 32, 32);
    return result;
}

void Database::add_to_bucket(leveldb::WriteBatch& b, uint16_t bucket_id,
                              const Hash256& content_hash) {
    auto existing = get_bucket(bucket_id);
    // Check not already present
    for (const auto& h : existing)
        if (h == content_hash) return;
    existing.push_back(content_hash);
    std::vector<uint8_t> data(existing.size() * 32);
    for (size_t i = 0; i < existing.size(); ++i)
        std::memcpy(data.data() + i * 32, existing[i].data(), 32);
    put_batch(b, bucket_key(bucket_id), data);
}

void Database::remove_from_bucket(leveldb::WriteBatch& b, uint16_t bucket_id,
                                   const Hash256& content_hash) {
    auto existing = get_bucket(bucket_id);
    existing.erase(
        std::remove(existing.begin(), existing.end(), content_hash),
        existing.end());
    std::vector<uint8_t> data(existing.size() * 32);
    for (size_t i = 0; i < existing.size(); ++i)
        std::memcpy(data.data() + i * 32, existing[i].data(), 32);
    if (data.empty())
        del_batch(b, bucket_key(bucket_id));
    else
        put_batch(b, bucket_key(bucket_id), data);
}

// ---- Mempool --------------------------------------------------------

bool Database::put_pending_tx(const Hash256& tx_hash, const std::vector<uint8_t>& tx_data) {
    return put("p:" + hex(tx_hash), tx_data);
}

bool Database::del_pending_tx(const Hash256& tx_hash) {
    return del("p:" + hex(tx_hash));
}

std::vector<std::pair<Hash256, std::vector<uint8_t>>> Database::get_all_pending_txs() const {
    std::vector<std::pair<Hash256, std::vector<uint8_t>>> result;
    leveldb::ReadOptions opts;
    auto* it = db_->NewIterator(opts);
    for (it->Seek("p:"); it->Valid() && it->key().starts_with("p:"); it->Next()) {
        auto key_str = it->key().ToString();
        auto hash_hex = key_str.substr(2);
        Hash256 h;
        if (crypto::parse_hash256(hash_hex, h)) {
            std::string val_str = it->value().ToString();
            result.emplace_back(h, std::vector<uint8_t>(val_str.begin(), val_str.end()));
        }
    }
    delete it;
    return result;
}

// ---- Song metadata index --------------------------------------------

static std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::vector<Hash256> parse_hash_list(const std::vector<uint8_t>& data) {
    size_t count = data.size() / 32;
    std::vector<Hash256> result(count);
    for (size_t i = 0; i < count; ++i)
        std::memcpy(result[i].data(), data.data() + i * 32, 32);
    return result;
}

static std::vector<uint8_t> pack_hash_list(const std::vector<Hash256>& hashes) {
    std::vector<uint8_t> data(hashes.size() * 32);
    for (size_t i = 0; i < hashes.size(); ++i)
        std::memcpy(data.data() + i * 32, hashes[i].data(), 32);
    return data;
}

void Database::put_song_meta(leveldb::WriteBatch& b, const Hash256& ch, const SongSection& song) {
    // Tail two fields (year, track_number) tacked onto the existing
    // \0-separated record. Older records lacking them still parse
    // correctly because get_song_meta falls back to default-zero.
    std::string entry = song.title  + '\0' + song.artist + '\0' + song.genre + '\0'
                      + song.album  + '\0' + std::to_string(song.duration_ms)
                      + '\0' + std::to_string(song.year)
                      + '\0' + std::to_string(song.track_number);
    put_batch(b, "sm:" + hex(ch),
              std::vector<uint8_t>(entry.begin(), entry.end()));
}

std::optional<Database::SongMeta> Database::get_song_meta(const Hash256& ch) const {
    auto v = get("sm:" + hex(ch));
    if (!v) return std::nullopt;
    std::string s(v->begin(), v->end());
    SongMeta meta;
    meta.content_hash = ch;
    size_t p0 = s.find('\0');
    if (p0 == std::string::npos) { meta.title = s; return meta; }
    meta.title = s.substr(0, p0);
    size_t p1 = s.find('\0', p0 + 1);
    if (p1 == std::string::npos) { meta.artist = s.substr(p0 + 1); return meta; }
    meta.artist = s.substr(p0 + 1, p1 - p0 - 1);
    size_t p2 = s.find('\0', p1 + 1);
    if (p2 == std::string::npos) { meta.genre = s.substr(p1 + 1); return meta; }
    meta.genre = s.substr(p1 + 1, p2 - p1 - 1);
    size_t p3 = s.find('\0', p2 + 1);
    if (p3 == std::string::npos) { meta.album = s.substr(p2 + 1); return meta; }
    meta.album = s.substr(p2 + 1, p3 - p2 - 1);
    size_t p4 = s.find('\0', p3 + 1);
    if (p4 == std::string::npos) {
        try { meta.duration_ms = static_cast<uint32_t>(std::stoul(s.substr(p3 + 1))); } catch (...) {}
        return meta;
    }
    try { meta.duration_ms = static_cast<uint32_t>(std::stoul(s.substr(p3 + 1, p4 - p3 - 1))); } catch (...) {}
    size_t p5 = s.find('\0', p4 + 1);
    if (p5 == std::string::npos) {
        try { meta.year = static_cast<uint16_t>(std::stoul(s.substr(p4 + 1))); } catch (...) {}
        return meta;
    }
    try { meta.year         = static_cast<uint16_t>(std::stoul(s.substr(p4 + 1, p5 - p4 - 1))); } catch (...) {}
    try { meta.track_number = static_cast<uint16_t>(std::stoul(s.substr(p5 + 1))); } catch (...) {}
    return meta;
}

void Database::set_content_height(leveldb::WriteBatch& b, const Hash256& ch, uint32_t height) {
    put_batch_u32(b, "bh:" + hex(ch), height);
}

std::optional<uint32_t> Database::get_content_height(const Hash256& ch) const {
    return get_u32("bh:" + hex(ch));
}

std::vector<Hash256> Database::get_all_song_hashes() const {
    std::vector<Hash256> result;
    leveldb::ReadOptions opts;
    auto* it = db_->NewIterator(opts);
    for (it->Seek("sm:"); it->Valid() && it->key().starts_with("sm:"); it->Next()) {
        auto key_str = it->key().ToString();
        Hash256 h;
        if (crypto::parse_hash256(key_str.substr(3), h))
            result.push_back(h);
    }
    delete it;
    return result;
}

void Database::add_to_artist_index(leveldb::WriteBatch& b, const std::string& artist,
                                    const Hash256& ch) {
    auto key = "ia:" + lower(artist);
    auto existing_raw = get(key);
    auto existing = existing_raw ? parse_hash_list(*existing_raw) : std::vector<Hash256>{};
    for (const auto& h : existing) if (h == ch) return;
    existing.push_back(ch);
    put_batch(b, key, pack_hash_list(existing));
}

void Database::add_to_genre_index(leveldb::WriteBatch& b, const std::string& genre,
                                   const Hash256& ch) {
    auto key = "ig:" + lower(genre);
    auto existing_raw = get(key);
    auto existing = existing_raw ? parse_hash_list(*existing_raw) : std::vector<Hash256>{};
    for (const auto& h : existing) if (h == ch) return;
    existing.push_back(ch);
    put_batch(b, key, pack_hash_list(existing));
}

std::vector<Hash256> Database::get_songs_by_artist(const std::string& artist) const {
    auto v = get("ia:" + lower(artist));
    return v ? parse_hash_list(*v) : std::vector<Hash256>{};
}

std::vector<Hash256> Database::get_songs_by_genre(const std::string& genre) const {
    auto v = get("ig:" + lower(genre));
    return v ? parse_hash_list(*v) : std::vector<Hash256>{};
}

// ---- Moderator / deleted songs --------------------------------------

bool Database::is_moderator(const Address& addr) const {
    return get("m:" + hex(addr)).has_value();
}

void Database::add_moderator(leveldb::WriteBatch& b, const Address& addr) {
    put_batch(b, "m:" + hex(addr), {});
}

bool Database::is_song_deleted(const Hash256& ch) const {
    return get("d:" + hex(ch)).has_value();
}

void Database::mark_song_deleted(leveldb::WriteBatch& b, const Hash256& ch) {
    put_batch(b, "d:" + hex(ch), {});
}

void Database::unmark_song_deleted(leveldb::WriteBatch& b, const Hash256& ch) {
    b.Delete("d:" + hex(ch));
}

// ---- On-chain moderator records -------------------------------------

std::optional<Address> Database::get_founder() const {
    auto v = get("founder:");
    if (!v || v->size() != 20) return std::nullopt;
    Address a;
    std::memcpy(a.data(), v->data(), 20);
    return a;
}

void Database::set_founder(leveldb::WriteBatch& b, const Address& addr) {
    put_batch(b, "founder:",
              std::vector<uint8_t>(addr.begin(), addr.end()));
}

uint8_t Database::get_mod_level(const Address& addr) const {
    auto v = get("mlvl:" + hex(addr));
    if (!v || v->empty()) return 0;
    return (*v)[0];
}

void Database::set_mod_level(leveldb::WriteBatch& b,
                             const Address& addr, uint8_t level) {
    if (level == 0) {
        b.Delete("mlvl:" + hex(addr));
        // Mirror into legacy `m:` table.
        b.Delete("m:" + hex(addr));
        return;
    }
    put_batch(b, "mlvl:" + hex(addr), std::vector<uint8_t>{level});
    // Any non-zero level implies moderator membership for the legacy
    // `is_moderator()` check.
    put_batch(b, "m:" + hex(addr), {});
}

std::optional<PubKey33> Database::get_mod_pubkey(const Address& addr) const {
    auto v = get("mpub:" + hex(addr));
    if (!v || v->size() != 33) return std::nullopt;
    PubKey33 pk{};
    std::memcpy(pk.data(), v->data(), 33);
    return pk;
}

void Database::set_mod_pubkey(leveldb::WriteBatch& b,
                              const Address& addr,
                              const PubKey33& pubkey) {
    put_batch(b, "mpub:" + hex(addr),
              std::vector<uint8_t>(pubkey.begin(), pubkey.end()));
}

std::optional<uint32_t> Database::get_mod_active_block(const Address& addr) const {
    return get_u32("mact:" + hex(addr));
}

void Database::set_mod_active_block(leveldb::WriteBatch& b,
                                    const Address& addr, uint32_t height) {
    put_batch_u32(b, "mact:" + hex(addr), height);
}

std::vector<Address> Database::list_active_moderators() const {
    std::vector<Address> out;
    for_each_with_prefix("mlvl:", [&](const std::string& k, const std::string& v){
        if (v.empty() || v[0] == 0) return true; // skip NONE entries defensively
        // Key format: "mlvl:<40 hex chars>". Decode addr.
        if (k.size() != 5 + 40) return true;
        Address a{};
        for (size_t i = 0; i < 20; ++i) {
            auto hi = k[5 + 2*i];
            auto lo = k[5 + 2*i + 1];
            auto h2 = [](char c)->int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                return -1;
            };
            int H = h2(hi), L = h2(lo);
            if (H < 0 || L < 0) return true;
            a[i] = static_cast<uint8_t>((H << 4) | L);
        }
        out.push_back(a);
        return true;
    });
    return out;
}

// ---- Record labels --------------------------------------------------

namespace {
std::string label_key_lower(const std::string& name) {
    std::string out = name;
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
} // namespace

void Database::set_label(leveldb::WriteBatch& b,
                         const std::string& name,
                         const LabelDef& def) {
    // Wire format:
    //   name_len (1) | display_name (name_len) |
    //   splits_count (1) | for each: wallet(20) + basis_points(2 LE)
    std::vector<uint8_t> buf;
    const uint8_t nl = static_cast<uint8_t>(std::min<size_t>(def.display_name.size(), 64));
    buf.push_back(nl);
    buf.insert(buf.end(), def.display_name.begin(), def.display_name.begin() + nl);
    const uint8_t sc = static_cast<uint8_t>(std::min<size_t>(def.splits.size(), 16));
    buf.push_back(sc);
    for (uint8_t i = 0; i < sc; ++i) {
        buf.insert(buf.end(),
                   def.splits[i].wallet.begin(),
                   def.splits[i].wallet.end());
        buf.push_back(static_cast<uint8_t>(def.splits[i].basis_points & 0xFF));
        buf.push_back(static_cast<uint8_t>((def.splits[i].basis_points >> 8) & 0xFF));
    }
    put_batch(b, "label:" + label_key_lower(name), buf);
}

std::optional<Database::LabelDef> Database::get_label(const std::string& name) const {
    auto raw = get("label:" + label_key_lower(name));
    if (!raw || raw->empty()) return std::nullopt;
    const uint8_t* p   = raw->data();
    const uint8_t* end = raw->data() + raw->size();
    if (p >= end) return std::nullopt;
    uint8_t nl = *p++;
    if (static_cast<size_t>(end - p) < nl) return std::nullopt;
    LabelDef out;
    out.display_name.assign(reinterpret_cast<const char*>(p), nl);
    p += nl;
    if (p >= end) return std::nullopt;
    uint8_t sc = *p++;
    out.splits.resize(sc);
    for (uint8_t i = 0; i < sc; ++i) {
        if (static_cast<size_t>(end - p) < 22) return std::nullopt;
        std::memcpy(out.splits[i].wallet.data(), p, 20);
        p += 20;
        out.splits[i].basis_points = static_cast<uint16_t>(p[0]) |
                                     (static_cast<uint16_t>(p[1]) << 8);
        p += 2;
    }
    return out;
}

std::vector<std::string> Database::list_labels() const {
    std::vector<std::string> out;
    for_each_with_prefix("label:", [&](const std::string& k, const std::string&){
        // Skip art_label: which doesn't actually start with "label:"
        // but does share the prefix in alphabetical proximity. The
        // strict prefix match above already screens it out.
        if (k.size() > 6) out.push_back(k.substr(6));
        return true;
    });
    return out;
}

void Database::assign_artist_label(leveldb::WriteBatch& b,
                                   const Address& artist,
                                   const std::string& label_name) {
    if (label_name.empty()) {
        b.Delete("art_label:" + hex(artist));
        return;
    }
    auto canonical = label_key_lower(label_name);
    put_batch(b, "art_label:" + hex(artist),
              std::vector<uint8_t>(canonical.begin(), canonical.end()));
}

std::optional<std::string> Database::get_artist_label(const Address& artist) const {
    auto v = get("art_label:" + hex(artist));
    if (!v) return std::nullopt;
    return std::string(v->begin(), v->end());
}

// ---- Username registry ----------------------------------------------

namespace {
std::string un_key_lower(const std::string& name) {
    std::string out = name;
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
} // namespace

bool Database::username_taken(const std::string& name) const {
    return get("un:" + un_key_lower(name)).has_value();
}

void Database::set_username(leveldb::WriteBatch& b,
                            const std::string& name,
                            const Address& addr) {
    auto canonical = un_key_lower(name);
    put_batch(b, "un:" + canonical,
              std::vector<uint8_t>(addr.begin(), addr.end()));
    put_batch(b, "addrun:" + hex(addr),
              std::vector<uint8_t>(canonical.begin(), canonical.end()));
}

std::optional<Address> Database::lookup_username(const std::string& name) const {
    auto v = get("un:" + un_key_lower(name));
    if (!v || v->size() != 20) return std::nullopt;
    Address a;
    std::memcpy(a.data(), v->data(), 20);
    return a;
}

std::optional<std::string> Database::get_addr_username(const Address& addr) const {
    auto v = get("addrun:" + hex(addr));
    if (!v) return std::nullopt;
    return std::string(v->begin(), v->end());
}

// ---- Multi-mod proposals (Phase 3) ----------------------------------

bool Database::has_proposal(const Hash256& h) const {
    return get("prop:" + hex(h)).has_value();
}

void Database::put_proposal(leveldb::WriteBatch& b,
                            const Hash256& h,
                            const std::vector<uint8_t>& raw) {
    put_batch(b, "prop:" + hex(h), raw);
}

std::optional<std::vector<uint8_t>> Database::get_proposal(const Hash256& h) const {
    return get("prop:" + hex(h));
}

uint8_t Database::get_proposal_status(const Hash256& h) const {
    auto v = get("propstatus:" + hex(h));
    if (!v || v->empty()) return PROP_PENDING;
    return (*v)[0];
}

void Database::set_proposal_status(leveldb::WriteBatch& b,
                                   const Hash256& h, uint8_t status) {
    put_batch(b, "propstatus:" + hex(h), std::vector<uint8_t>{status});
}

bool Database::has_proposal_vote(const Hash256& prop_hash,
                                 const Address& voter) const {
    return get("propvote:" + hex(prop_hash) + ":" + hex(voter)).has_value();
}

void Database::add_proposal_vote(leveldb::WriteBatch& b,
                                 const Hash256& prop_hash,
                                 const Address& voter) {
    put_batch(b, "propvote:" + hex(prop_hash) + ":" + hex(voter), {});
}

size_t Database::count_proposal_votes(const Hash256& prop_hash) const {
    size_t n = 0;
    for_each_with_prefix("propvote:" + hex(prop_hash) + ":",
        [&](const std::string&, const std::string&){
            ++n; return true;
        });
    return n;
}

std::vector<Hash256> Database::list_pending_proposals() const {
    // We treat absence of the status key as PENDING; iterate over the
    // `prop:` table and filter out any that have flipped to EXECUTED.
    std::vector<Hash256> out;
    for_each_with_prefix("prop:", [&](const std::string& k, const std::string&){
        // Skip the propstatus + propvote tables which also start with
        // "prop". Filter strictly on the "prop:" prefix length + the
        // 64-hex tail.
        if (k.size() != 5 + 64) return true;
        Hash256 h{};
        for (size_t i = 0; i < 32; ++i) {
            auto h2 = [](char c)->int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                return -1;
            };
            int H = h2(k[5 + 2*i]);
            int L = h2(k[5 + 2*i + 1]);
            if (H < 0 || L < 0) return true;
            h[i] = static_cast<uint8_t>((H << 4) | L);
        }
        if (get_proposal_status(h) == PROP_PENDING) out.push_back(h);
        return true;
    });
    return out;
}

// ---- Category hide lists --------------------------------------------

namespace {
std::string lc(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

bool Database::is_hidden_artist(const std::string& artist) const {
    return get("ha:" + lc(artist)).has_value();
}
bool Database::is_hidden_album(const std::string& album) const {
    return get("hb:" + lc(album)).has_value();
}
bool Database::is_hidden_title(const std::string& title) const {
    return get("ht:" + lc(title)).has_value();
}
void Database::add_hidden_artist(leveldb::WriteBatch& b, const std::string& artist) {
    put_batch(b, "ha:" + lc(artist), std::vector<uint8_t>(artist.begin(), artist.end()));
}
void Database::add_hidden_album(leveldb::WriteBatch& b, const std::string& album) {
    put_batch(b, "hb:" + lc(album),  std::vector<uint8_t>(album.begin(),  album.end()));
}
void Database::add_hidden_title(leveldb::WriteBatch& b, const std::string& title) {
    put_batch(b, "ht:" + lc(title),  std::vector<uint8_t>(title.begin(),  title.end()));
}
void Database::remove_hidden_artist(leveldb::WriteBatch& b, const std::string& artist) {
    b.Delete("ha:" + lc(artist));
}
void Database::remove_hidden_album(leveldb::WriteBatch& b, const std::string& album) {
    b.Delete("hb:" + lc(album));
}
void Database::remove_hidden_title(leveldb::WriteBatch& b, const std::string& title) {
    b.Delete("ht:" + lc(title));
}
std::vector<std::string> Database::list_hidden_artists() const {
    std::vector<std::string> out;
    for_each_with_prefix("ha:", [&](const std::string&, const std::string& v){
        out.emplace_back(v); return true;
    });
    return out;
}
std::vector<std::string> Database::list_hidden_albums() const {
    std::vector<std::string> out;
    for_each_with_prefix("hb:", [&](const std::string&, const std::string& v){
        out.emplace_back(v); return true;
    });
    return out;
}
std::vector<std::string> Database::list_hidden_titles() const {
    std::vector<std::string> out;
    for_each_with_prefix("ht:", [&](const std::string&, const std::string& v){
        out.emplace_back(v); return true;
    });
    return out;
}

// ---- Moderation gossip log ------------------------------------------

namespace {
std::string be16_ts(uint64_t ts) {
    // Big-endian 16-hex-char encoding so leveldb's lexicographic ordering
    // matches numerical timestamp order. Without this, "9" sorts after
    // "10" and iter_mod_log_since skips entries.
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(ts));
    return std::string(buf, 16);
}
std::string ml_key(uint64_t ts, const std::string& sig_hex) {
    std::string sig_prefix = sig_hex.size() >= 16
        ? sig_hex.substr(0, 16)
        : sig_hex;
    return "ml:" + be16_ts(ts) + ":" + sig_prefix;
}
// Signature dedup index: separate prefix that maps sig_prefix → "" so we
// can check "have I seen this sig?" without scanning the whole log.
std::string ms_key(const std::string& sig_hex) {
    std::string sig_prefix = sig_hex.size() >= 16
        ? sig_hex.substr(0, 16)
        : sig_hex;
    return "ms:" + sig_prefix;
}
} // namespace

void Database::append_mod_log_entry(leveldb::WriteBatch& b,
                                    uint64_t            ts_ms,
                                    const std::string&  sig_hex,
                                    const std::string&  payload_json) {
    put_batch(b, ml_key(ts_ms, sig_hex),
              std::vector<uint8_t>(payload_json.begin(), payload_json.end()));
    put_batch(b, ms_key(sig_hex), {});
}

bool Database::mod_log_has_sig(const std::string& sig_hex) const {
    return get(ms_key(sig_hex)).has_value();
}

uint64_t Database::latest_mod_log_ts() const {
    uint64_t latest = 0;
    for_each_with_prefix("ml:", [&](const std::string& k, const std::string&){
        // Key shape: ml:<16hex-ts>:<sig16>
        if (k.size() < 3 + 16) return true;
        uint64_t ts = 0;
        for (size_t i = 3; i < 3 + 16; ++i) {
            char c = k[i];
            int v = 0;
            if      (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else return true;
            ts = (ts << 4) | static_cast<uint64_t>(v);
        }
        if (ts > latest) latest = ts;
        return true;
    });
    return latest;
}

void Database::iter_mod_log_since(
    uint64_t since_ts_ms,
    const std::function<bool(uint64_t,
                             const std::string&,
                             const std::string&)>& cb) const {
    for_each_with_prefix("ml:", [&](const std::string& k, const std::string& v){
        if (k.size() < 3 + 16 + 1) return true;
        uint64_t ts = 0;
        for (size_t i = 3; i < 3 + 16; ++i) {
            char c = k[i];
            int hv = 0;
            if      (c >= '0' && c <= '9') hv = c - '0';
            else if (c >= 'a' && c <= 'f') hv = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') hv = c - 'A' + 10;
            else return true;
            ts = (ts << 4) | static_cast<uint64_t>(hv);
        }
        if (ts < since_ts_ms) return true;
        const std::string sig_prefix = k.substr(3 + 16 + 1);
        return cb(ts, sig_prefix, v);
    });
}

// ---- Transfer nonce -------------------------------------------------

uint64_t Database::get_nonce(const Address& addr) const {
    return get_u64("nv:" + hex(addr)).value_or(0);
}

void Database::set_nonce(leveldb::WriteBatch& b, const Address& addr, uint64_t nonce) {
    put_batch_u64(b, "nv:" + hex(addr), nonce);
}

// ---- Utility --------------------------------------------------------

std::string Database::hex(const Hash256& h) const {
    return crypto::to_hex(h);
}

std::string Database::hex(const Address& a) const {
    return crypto::to_hex(a.data(), 20);
}

void Database::clear_derived_state() {
    // Delete all keys with prefixes: a:, s:, f:, i:, u:
    leveldb::WriteBatch batch;
    const std::vector<std::string> prefixes{"a:", "s:", "f:", "i:", "u:"};
    for (const auto& prefix : prefixes) {
        auto* it = db_->NewIterator(leveldb::ReadOptions());
        for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next())
            batch.Delete(it->key());
        delete it;
    }
    db_->Write(leveldb::WriteOptions(), &batch);
}

uint64_t Database::approximate_size() const {
    leveldb::Range r("", "\xFF\xFF\xFF\xFF");
    uint64_t size = 0;
    db_->GetApproximateSizes(&r, 1, &size);
    return size;
}

void Database::for_each_with_prefix(
    const std::string& prefix,
    const std::function<bool(const std::string&,
                             const std::string&)>& cb) const {
    if (!db_) return;
    auto* it = db_->NewIterator(leveldb::ReadOptions());
    for (it->Seek(prefix);
         it->Valid() && it->key().starts_with(prefix);
         it->Next()) {
        if (!cb(it->key().ToString(), it->value().ToString())) break;
    }
    delete it;
}

} // namespace mc
