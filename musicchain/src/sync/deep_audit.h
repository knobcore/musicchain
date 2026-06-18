#pragma once

#include "../core/chain.h"
#include "../storage/database.h"

#include <atomic>
#include <thread>

namespace mc {

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

    DeepAuditor(Chain& chain, Database& db, const std::string& audio_dir);
    ~DeepAuditor();

    void start();
    void stop();

private:
    void loop();
    // Returns true if the block's audio matches its declared fingerprint
    // within tolerance. False = forgery detected. nullopt = couldn't
    // audit (audio not available locally — try again next cycle).
    std::optional<bool> audit_block(const Block& block);

    Chain&            chain_;
    Database&         db_;
    std::string       audio_dir_;
    std::thread       worker_;
    std::atomic<bool> running_{false};
};

} // namespace mc
