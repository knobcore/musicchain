#include "deep_audit.h"
#include "../audio/fingerprint.h"
#include "../crypto/hash.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <utility>

namespace fs = std::filesystem;

namespace mc {

DeepAuditor::DeepAuditor(Chain& chain, Database& db,
                          const std::string& audio_dir,
                          OnForgery on_forgery)
    : chain_(chain), db_(db), audio_dir_(audio_dir),
      on_forgery_(std::move(on_forgery)) {}

DeepAuditor::~DeepAuditor() { stop(); }

namespace {
// Read the content-addressed audio bytes for `ch` under `audio_dir`, or
// nullopt if not held locally / empty. Layout: <audio_dir>/<ch[0:2]>/<ch>.
std::optional<std::vector<uint8_t>> read_audio(const std::string& audio_dir,
                                               const std::string& ch_hex) {
    const fs::path p = fs::path(audio_dir) / ch_hex.substr(0, 2) / ch_hex;
    std::error_code ec;
    if (!fs::exists(p, ec)) return std::nullopt;
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return std::nullopt;
    const auto size = f.tellg();
    if (size <= 0) return std::nullopt;
    f.seekg(0);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}

// Decode `bytes`, re-fingerprint, and compare to `declared_compressed_fp`.
std::optional<AuditResult> compare_audio_to_fp(
        const std::vector<uint8_t>& bytes,
        const std::string& declared_compressed_fp) {
    auto fresh = audio::Fingerprint::from_any(bytes.data(), bytes.size());
    if (!fresh) return std::nullopt;
    auto declared = audio::Fingerprint::from_compressed(declared_compressed_fp);
    if (!declared) return std::nullopt;
    AuditResult r;
    r.sim       = fresh->similarity(*declared);
    r.ok        = r.sim >= DeepAuditor::kSlashThreshold;
    r.audio_sha = crypto::sha256(bytes.data(), bytes.size());
    return r;
}
} // namespace

// Reusable core used by both the auditor loop and RatsApi's re-audit path.
std::optional<AuditResult> audit_content(Database& db,
                                         const std::string& audio_dir,
                                         const Hash256& content_hash) {
    const std::string ch_hex = crypto::to_hex(content_hash);
    auto bytes = read_audio(audio_dir, ch_hex);
    if (!bytes) return std::nullopt;
    auto entry = db.get_fingerprint(content_hash);   // the declared fingerprint
    if (!entry) return std::nullopt;
    return compare_audio_to_fp(*bytes, entry->compressed_fingerprint);
}

void DeepAuditor::start() {
    if (running_.exchange(true)) return;
    worker_ = std::thread([this] { loop(); });
}

void DeepAuditor::stop() {
    if (!running_.exchange(false)) return;
    if (worker_.joinable()) worker_.join();
}

std::optional<AuditResult> DeepAuditor::audit_block(const Block& block) {
    if (!block.has_song) return std::nullopt; // heartbeat — nothing to audit

    // Audio bytes are stored content-addressed at
    //   <audio_dir>/<first 2 hex chars>/<full content_hash hex>
    // If we don't hold them locally we skip this round — next cycle's
    // random pick may select a block we DO hold.
    const std::string ch_hex = crypto::to_hex(block.song.content_hash);
    auto bytes = read_audio(audio_dir_, ch_hex);
    if (!bytes) return std::nullopt;
    // FFmpeg multi-format decode + chromaprint vs the block's declared fp.
    auto r = compare_audio_to_fp(*bytes, block.song.compressed_fingerprint);
    if (r) {
        std::cout << "[audit] " << ch_hex.substr(0, 16) << "… sim=" << r->sim
                  << (r->ok ? " ok\n" : " FORGED\n");
    }
    return r;
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
        if (result && !result->ok) {
            // FORGERY: the audio at content_hash does not match the pinned
            // fingerprint. Tooth #1 (local, deterministic): invalidate the
            // content so THIS node stops serving / surfacing / minting
            // against it.
            leveldb::WriteBatch batch;
            db_.mark_song_deleted(batch, blk->song.content_hash);
            db_.write(batch);
            std::cerr << "[audit] CRITICAL: block at height " << h
                      << " has forged fingerprint (sim=" << result->sim
                      << ") — content "
                      << crypto::to_hex(blk->song.content_hash).substr(0, 16)
                      << "… invalidated locally\n";
            // Tooth #2 (#4): gossip a signed forgery report so the rest of
            // the mesh independently corroborates (K reports) / re-audits
            // and drops it too. node_main wires this to
            // RatsApi::publish_forgery_report.
            if (on_forgery_)
                on_forgery_(blk->song.content_hash, *bh,
                            result->sim, result->audio_sha);
        }
    }
}

} // namespace mc
