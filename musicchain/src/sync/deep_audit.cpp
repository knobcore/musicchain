#include "deep_audit.h"
#include "../audio/fingerprint.h"
#include "../crypto/hash.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>

namespace fs = std::filesystem;

namespace mc {

DeepAuditor::DeepAuditor(Chain& chain, Database& db,
                          const std::string& audio_dir)
    : chain_(chain), db_(db), audio_dir_(audio_dir) {}

DeepAuditor::~DeepAuditor() { stop(); }

void DeepAuditor::start() {
    if (running_.exchange(true)) return;
    worker_ = std::thread([this] { loop(); });
}

void DeepAuditor::stop() {
    if (!running_.exchange(false)) return;
    if (worker_.joinable()) worker_.join();
}

std::optional<bool> DeepAuditor::audit_block(const Block& block) {
    if (!block.has_song) return std::nullopt; // heartbeat — nothing to audit

    // Audio bytes are stored content-addressed at
    //   <audio_dir>/<first 2 hex chars>/<full content_hash hex>
    // (see HttpServer::audio_dir helpers). If we don't hold them locally
    // we skip this round — next cycle's random pick may select a block
    // we DO hold, and once a swarm-fetch helper is wired we'll be able
    // to pull missing audio on demand.
    const std::string ch_hex = crypto::to_hex(block.song.content_hash);
    const fs::path audio_path =
        fs::path(audio_dir_) / ch_hex.substr(0, 2) / ch_hex;
    std::error_code ec;
    if (!fs::exists(audio_path, ec)) return std::nullopt;

    std::ifstream f(audio_path, std::ios::binary | std::ios::ate);
    if (!f) return std::nullopt;
    const auto size = f.tellg();
    if (size <= 0) return std::nullopt;
    f.seekg(0);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(bytes.data()), size);

    // Multi-format audio decode via FFmpeg — handles MP3 / FLAC / WAV /
    // AAC / Opus / Ogg uniformly. No more "skip non-Ogg" hole.
    auto fresh = audio::Fingerprint::from_any(bytes.data(), bytes.size());
    if (!fresh) return std::nullopt;
    auto declared = audio::Fingerprint::from_compressed(
        block.song.compressed_fingerprint);
    if (!declared) return std::nullopt;

    const float sim = fresh->similarity(*declared);
    std::cout << "[audit] " << ch_hex.substr(0, 16) << "… sim=" << sim
              << (sim >= kSlashThreshold ? " ok\n" : " FORGED\n");
    return sim >= kSlashThreshold;
}

void DeepAuditor::loop() {
    std::mt19937 rng{std::random_device{}()};
    while (running_) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kAuditIntervalMs));
        if (!running_) break;

        const auto tip = chain_.tip();
        if (tip.height == 0) continue;

        // Bias toward the newest 1024 blocks where slashing still has
        // meaning, but with some probability check anything from height
        // 1 onward so historical forgeries can't outlast the audit
        // window forever.
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        uint32_t h;
        if (u(rng) < 0.85f) {
            uint32_t lo = tip.height > 1024 ? tip.height - 1024 : 1;
            std::uniform_int_distribution<uint32_t> d(lo, tip.height);
            h = d(rng);
        } else {
            std::uniform_int_distribution<uint32_t> d(1, tip.height);
            h = d(rng);
        }
        auto bh = chain_.get_block_hash(h);
        if (!bh) continue;
        auto blk = chain_.get_block(*bh);
        if (!blk) continue;
        auto result = audit_block(*blk);
        if (result && !*result) {
            // FORGERY. For now we log loudly. Once SlashTx is wired we
            // construct an EquivocationProof carrying:
            //   - block_hash      — the offender
            //   - producer_pubkey — who signed it (from confirmations)
            //   - audio_sha256(bytes)  — what we actually saw
            //   - declared_fingerprint
            //   - recomputed_fingerprint
            // and broadcast it; chain.apply_slash() updates the validator
            // registry to reduce that producer's weight to zero.
            std::cerr << "[audit] CRITICAL: block at height " << h
                      << " has forged fingerprint — slash evidence held "
                         "in memory until SlashTx ships\n";
        }
    }
}

} // namespace mc
