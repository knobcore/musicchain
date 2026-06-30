#pragma once
//
// Moderation action envelope: a single hide/unhide decision the moderator
// emits, signed with their wallet key, gossiped to every connected node,
// and persisted in each node's mod log so a fresh node can catch up via
// mod.sync_since on its first connection.
//
// Wire shape (JSON, stable):
//
//   { "v":1, "action":"hide_artist", "value":"...",
//     "mod_pub":"<66 hex compressed pubkey>",
//     "ts_ms": 1718412345678,
//     "sig":   "<128 hex compact-ECDSA over canon()>" }
//
// canon() = action + 0x1F + value + 0x1F + ts_ms + 0x1F + mod_pub_hex,
// SHA-256 of that string is the message hash that gets signed.
//

#include "../core/block.h"
#include "../crypto/hash.h"
#include "../crypto/keys.h"
#include "../crypto/signature.h"
#include "../storage/database.h"

#include <leveldb/write_batch.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdint>
#include <string>

namespace mc::moderation {

struct Envelope {
    int         v       = 1;
    std::string action;       // "hide_artist" | "unhide_artist" | ...
    std::string value;
    std::string mod_pub_hex;  // 66 hex (compressed pubkey)
    uint64_t    ts_ms   = 0;
    std::string sig_hex;      // 128 hex
};

inline std::string canon(const std::string& action,
                         const std::string& value,
                         uint64_t           ts_ms,
                         const std::string& mod_pub_hex) {
    std::string s;
    s.reserve(action.size() + value.size() + mod_pub_hex.size() + 40);
    s += action;
    s.push_back('\x1f');
    s += value;
    s.push_back('\x1f');
    s += std::to_string(ts_ms);
    s.push_back('\x1f');
    s += mod_pub_hex;
    return s;
}

inline uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count());
}

inline Envelope sign(const std::string& action,
                     const std::string& value,
                     const mc::crypto::KeyPair& moderator_kp) {
    Envelope e;
    e.action      = action;
    e.value       = value;
    e.mod_pub_hex = mc::crypto::to_hex(moderator_kp.public_key.data(), 33);
    e.ts_ms       = now_ms();
    const std::string c = canon(e.action, e.value, e.ts_ms, e.mod_pub_hex);
    const Hash256 h = mc::crypto::sha256(
        reinterpret_cast<const uint8_t*>(c.data()), c.size());
    const auto sig = mc::crypto::sign_ecdsa(h, moderator_kp.private_key);
    e.sig_hex = mc::crypto::to_hex(sig.data(), 64);
    return e;
}

inline nlohmann::json to_json(const Envelope& e) {
    return {
        {"v",       e.v},
        {"action",  e.action},
        {"value",   e.value},
        {"mod_pub", e.mod_pub_hex},
        {"ts_ms",   e.ts_ms},
        {"sig",     e.sig_hex},
    };
}

inline bool from_json(const nlohmann::json& j, Envelope& out) {
    if (!j.is_object()) return false;
    out.v           = j.value("v",       0);
    out.action      = j.value("action",  std::string());
    out.value       = j.value("value",   std::string());
    out.mod_pub_hex = j.value("mod_pub", std::string());
    out.ts_ms       = j.value("ts_ms",   0ULL);
    out.sig_hex     = j.value("sig",     std::string());
    return out.v == 1
        && !out.action.empty()
        && out.mod_pub_hex.size() == 66
        && out.sig_hex.size()     == 128
        && out.ts_ms              != 0;
}

// Verify the signature AND that the signing pubkey's address is currently
// in the node's moderator set. Returns false on any failure.
inline bool verify(const Envelope& e, const Database& db) {
    if (e.v != 1) return false;
    auto pub_bytes = mc::crypto::from_hex(e.mod_pub_hex);
    if (pub_bytes.size() != 33) return false;
    PubKey33 pub{};
    std::copy(pub_bytes.begin(), pub_bytes.end(), pub.begin());
    const Address addr = mc::crypto::address_from_pubkey(pub);
    if (!db.is_moderator(addr)) return false;
    const std::string c = canon(e.action, e.value, e.ts_ms, e.mod_pub_hex);
    const Hash256 h = mc::crypto::sha256(
        reinterpret_cast<const uint8_t*>(c.data()), c.size());
    auto sig_bytes = mc::crypto::from_hex(e.sig_hex);
    if (sig_bytes.size() != 64) return false;
    Sig64 sig{};
    std::copy(sig_bytes.begin(), sig_bytes.end(), sig.begin());
    return mc::crypto::verify_ecdsa(h, sig, pub);
}

// Like verify() but WITHOUT the is_moderator gate. Used by the forgery-
// report path (#4): a forgery_report is a *machine attestation* ("I, node
// X, decoded the audio at this content_hash and measured a fingerprint
// mismatch"), signed by the auditing node's own key — which is not a
// moderator key. Trust comes from K-independent-reports + local re-audit
// (see RatsApi::handle_forgery_report), NOT a moderator allowlist. Verifies
// only that the signature is valid for the carried pubkey.
inline bool verify_signature_only(const Envelope& e) {
    if (e.v != 1) return false;
    auto pub_bytes = mc::crypto::from_hex(e.mod_pub_hex);
    if (pub_bytes.size() != 33) return false;
    PubKey33 pub{};
    std::copy(pub_bytes.begin(), pub_bytes.end(), pub.begin());
    const std::string c = canon(e.action, e.value, e.ts_ms, e.mod_pub_hex);
    const Hash256 h = mc::crypto::sha256(
        reinterpret_cast<const uint8_t*>(c.data()), c.size());
    auto sig_bytes = mc::crypto::from_hex(e.sig_hex);
    if (sig_bytes.size() != 64) return false;
    Sig64 sig{};
    std::copy(sig_bytes.begin(), sig_bytes.end(), sig.begin());
    return mc::crypto::verify_ecdsa(h, sig, pub);
}

// Mutate db so it reflects the envelope's action. The caller must already
// have verified the signature; replay protection (dedup by sig) is
// expected to be enforced upstream via mod_log_has_sig.
//
// Action vocab:
//   hide_artist / unhide_artist  → ha:
//   hide_album  / unhide_album   → hb:
//   hide_title  / unhide_title   → ht:
//   hide_hash   / unhide_hash    → d:   (value is 64-hex content hash)
//
// Returns false on a malformed action / value (e.g. bad hash); the caller
// should NOT persist such envelopes to the mod log.
inline bool apply(const Envelope& e, Database& db, leveldb::WriteBatch& b) {
    if (e.action == "hide_artist")    { db.add_hidden_artist (b, e.value); return true; }
    if (e.action == "unhide_artist")  { db.remove_hidden_artist(b, e.value); return true; }
    if (e.action == "hide_album")     { db.add_hidden_album  (b, e.value); return true; }
    if (e.action == "unhide_album")   { db.remove_hidden_album(b, e.value); return true; }
    if (e.action == "hide_title")     { db.add_hidden_title  (b, e.value); return true; }
    if (e.action == "unhide_title")   { db.remove_hidden_title(b, e.value); return true; }
    if (e.action == "hide_hash" || e.action == "unhide_hash") {
        Hash256 ch{};
        if (!mc::crypto::parse_hash256(e.value, ch)) return false;
        if (e.action == "hide_hash") db.mark_song_deleted(b, ch);
        else                          db.unmark_song_deleted(b, ch);
        return true;
    }
    return false;
}

} // namespace mc::moderation
