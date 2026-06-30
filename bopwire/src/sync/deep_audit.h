#pragma once
#include <cstdint>

#include "../core/chain.h"
#include "../storage/database.h"

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <thread>

namespace mc {

// Result of comparing a song's audio bytes against its declared
// fingerprint. ok = matches within tolerance; sim = measured similarity;
// audio_sha = sha256 of the exact bytes audited (the audit transcript,
// gossiped in a forgery report so other nodes can corroborate / re-audit).
struct AuditResult {
    bool    ok;
    float   sim;
    Hash256 audio_sha;
};

// Reusable core: decode the audio stored at `content_hash` in the
// content-addressed store under `audio_dir`, re-fingerprint it, and compare
// to the declared fingerprint recorded for that hash in `db`. Returns
// nullopt if the bytes aren't held locally or can't be decoded. Used by the
// DeepAuditor loop AND by RatsApi's forgery-report re-audit path.
std::optional<AuditResult> audit_content(Database& db,
                                         const std::string& audio_dir,
                                         const Hash256& content_hash);

// DeepAuditor — anti-forgery check that closes the audio↔fingerprint
// gap our sync-time fingerprint validation can't on its own.
//
// The hole it plugs: today every block self-validates that
// sha256(compressed_fingerprint) == header.fingerprint_hash, which
// guarantees the producer didn't lie about which fingerprint they're
// pinning. It does NOT guarantee the *audio file* at content_hash
// actually matches that fingerprint. A producer can publish a song
// where the fingerprint in the block matches what they hashed, but the
// audio they upload to the swarm is completely different.
//
// DeepAuditor catches that mismatch by periodically:
//   1. Picking a recently-finalized block at random.
//   2. Fetching the audio bytes via the local content-addressed store
//      (or via a swarm peer if we don't hold them).
//   3. Decoding to PCM and re-running chromaprint on the result.
//   4. Computing similarity against the in-block compressed_fingerprint.
//   5. If similarity < kSlashThreshold (≈ 0.50) it's a clear forgery —
//      we log it, hold the evidence, and once SlashTx is wired we will
//      emit an EquivocationProof tx that gets the producer slashed.
//
// Frequency: every kAuditIntervalMs we pick one block. Bias toward
// newer blocks (where slashing still has meaning) but every block
// eventually gets audited, so a producer who tried to forge week-old
// content can still be caught later.
//
// Not synchronous: deep audit happens in a background thread because
// audio decode + chromaprint is hundreds of milliseconds per song.
// We never block the producer / RPC threads on it.
class DeepAuditor {
public:
    static constexpr float    kSlashThreshold    = 0.50f;
    static constexpr uint32_t kAuditIntervalMs   = 60 * 1000; // 1/min

    // on_forgery (optional): called when a sampled block's audio does NOT
    // match its declared fingerprint, with (content_hash, block_hash,
    // measured similarity, audio sha256). node_main wires this to
    // RatsApi::publish_forgery_report so the finding gossips across the mesh
    // (#4). When unset, the auditor only invalidates the content locally.
    using OnForgery = std::function<void(const Hash256& content_hash,
                                         const Hash256& block_hash,
                                         float sim,
                                         const Hash256& audio_sha)>;
    DeepAuditor(Chain& chain, Database& db, const std::string& audio_dir,
                OnForgery on_forgery = {});
    ~DeepAuditor();

    void start();
    void stop();

private:
    void loop();
    // Audit a sampled block's audio against its declared fingerprint.
    // nullopt = couldn't audit (audio not held locally — retry next cycle).
    std::optional<AuditResult> audit_block(const Block& block);

    Chain&            chain_;
    Database&         db_;
    std::string       audio_dir_;
    OnForgery         on_forgery_;
    std::thread       worker_;
    std::atomic<bool> running_{false};
};

} // namespace mc
