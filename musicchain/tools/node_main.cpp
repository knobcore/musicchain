/**
 * node_main.cpp - Entry point for musicchain-node
 *
 * Usage:
 *   musicchain-node start [options]
 *   musicchain-node status
 *   musicchain-node peers
 *   musicchain-node sync-status
 *   musicchain-node stop
 *   musicchain-node rebuild-index
 *   musicchain-node verify-chain
 */

#include "../src/core/chain.h"
#include "../src/storage/database.h"
#include "../src/network/manager.h"
#include "../src/network/rats_link.h"
#include "../src/api/server.h"
#include "../src/api/rats_api.h"
#include "../src/api/jsonrpc_server.h"
// h3_server include removed: the standalone HTTP/3 listener was retired
// when verbs moved to librats RPC. Restore behind MC_WITH_H3 when bringing
// it back.
#include "../src/consensus/candidate.h"
#include "../src/consensus/validator.h"
#include "../src/sync/block_propagator.h"
#include "../src/sync/deep_audit.h"
#include "../src/net/load_monitor.h"
#include "../src/net/relay_credit_tracker.h"
#include "../src/crypto/bip39.h"
#include "../src/core/transaction.h"
#include "../src/crypto/keys.h"
#include "../src/crypto/hash.h"
#include "../src/crypto/signature.h"
#include "../src/audio/fingerprint.h"
#include <unordered_set>
#include "node_tui.h"

#include "../deps/librats/src/librats_c.h"

#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <string>
#include <vector>
#include <cstring>

using json = nlohmann::json;
namespace fs = std::filesystem;

// Load-monitor settings parsed from the JSON config — used to construct
// the LoadMonitor below. File-scope so load_config can populate it
// without changing the function signature.
static mc::net::LoadConfig g_load_cfg{};

// ---- Configuration --------------------------------------------------

// Mirrors NodeConfig + the bits node_main keeps in file-scope state so
// running the binary a second time replays the same setup. Anything an
// operator sets via the TUI or CLI lands here, gets serialized to
// `config.json`, and is honoured on next boot. Defaults are conservative
// (loopback-free 9333/9334/8080 + small caches) so a fresh install with
// no config still produces a useful node.
static bool g_tui_mode_persisted = true;

static mc::net::NodeConfig load_config(const std::string& path) {
    mc::net::NodeConfig cfg;
    if (!fs::exists(path)) return cfg;
    std::ifstream f(path);
    json j;
    f >> j;
    if (j.contains("data_dir"))           cfg.data_dir = j["data_dir"];
    if (j.contains("p2p_port"))           cfg.p2p_port = j["p2p_port"];
    if (j.contains("api_port"))           cfg.api_port = j["api_port"];
    if (j.contains("rats_port"))          cfg.rats_port = j["rats_port"];
    if (j.contains("max_peers"))          cfg.max_peers = j["max_peers"];
    if (j.contains("max_sessions"))       cfg.max_sessions = j["max_sessions"];
    if (j.contains("validator_enabled"))  cfg.validator_enabled = j["validator_enabled"];
    if (j.contains("log_level"))          cfg.log_level = j["log_level"];
    if (j.contains("tui_mode"))           g_tui_mode_persisted = j["tui_mode"];
    if (j.contains("seed_nodes")) {
        for (auto& s : j["seed_nodes"]) cfg.seed_nodes.push_back(s);
    }
    if (j.contains("sync_seeds")) {
        for (auto& s : j["sync_seeds"]) cfg.sync_seeds.push_back(s);
    }
    if (j.contains("dht_bootstrap_hash"))
        cfg.dht_bootstrap_hash = j["dht_bootstrap_hash"];
    if (j.contains("registry_url"))  cfg.registry_url  = j["registry_url"];
    if (j.contains("load_monitor")) {
        const auto& lm = j["load_monitor"];
        if (lm.contains("max_bandwidth_bps"))
            g_load_cfg.max_bandwidth_bps = lm["max_bandwidth_bps"];
        if (lm.contains("cpu_weight"))
            g_load_cfg.cpu_weight = lm["cpu_weight"];
        if (lm.contains("net_weight"))
            g_load_cfg.net_weight = lm["net_weight"];
        if (lm.contains("sample_interval_ms"))
            g_load_cfg.sample_interval_ms = lm["sample_interval_ms"];
        if (lm.contains("busy_score_threshold"))
            g_load_cfg.busy_score_threshold = lm["busy_score_threshold"];
        if (lm.contains("disable_net_metric"))
            g_load_cfg.disable_net_metric = lm["disable_net_metric"];
        if (lm.contains("disable_cpu_metric"))
            g_load_cfg.disable_cpu_metric = lm["disable_cpu_metric"];
    }
    return cfg;
}

// Write the current config (whatever load_config + CLI overrides produced)
// back to disk so the next launch picks up the same settings without the
// operator having to remember which flags they used. Called from cmd_start
// once the full settings are resolved; safe to overwrite — we keep the
// existing file's unknown keys by reading-then-mutating-then-writing.
static void save_config(const std::string& path,
                        const mc::net::NodeConfig& cfg,
                        bool tui_mode) {
    json j = json::object();
    if (fs::exists(path)) {
        try { std::ifstream f(path); f >> j; } catch (...) { j = json::object(); }
    }
    j["data_dir"]          = cfg.data_dir;
    j["p2p_port"]          = cfg.p2p_port;
    j["api_port"]          = cfg.api_port;
    j["rats_port"]         = cfg.rats_port;
    j["max_peers"]         = cfg.max_peers;
    j["max_sessions"]      = cfg.max_sessions;
    j["validator_enabled"] = cfg.validator_enabled;
    j["log_level"]         = cfg.log_level;
    j["tui_mode"]          = tui_mode;
    j["seed_nodes"]        = cfg.seed_nodes;
    j["sync_seeds"]        = cfg.sync_seeds;
    j["dht_bootstrap_hash"]= cfg.dht_bootstrap_hash;
    j["registry_url"]      = cfg.registry_url;
    json lm = json::object();
    lm["max_bandwidth_bps"]    = g_load_cfg.max_bandwidth_bps;
    lm["cpu_weight"]           = g_load_cfg.cpu_weight;
    lm["net_weight"]           = g_load_cfg.net_weight;
    lm["sample_interval_ms"]   = g_load_cfg.sample_interval_ms;
    lm["busy_score_threshold"] = g_load_cfg.busy_score_threshold;
    lm["disable_net_metric"]   = g_load_cfg.disable_net_metric;
    lm["disable_cpu_metric"]   = g_load_cfg.disable_cpu_metric;
    j["load_monitor"]      = lm;
    try {
        fs::create_directories(fs::path(path).parent_path());
        std::ofstream f(path);
        f << j.dump(2);
    } catch (...) { /* non-fatal */ }
}

// ---- Signal handling ------------------------------------------------

static std::atomic<bool> g_running{true};

static void signal_handler(int /*sig*/) {
    g_running.store(false, std::memory_order_relaxed);
}

// ---- Subcommand: start ----------------------------------------------

static int cmd_start(const std::vector<std::string>& args, const char* exe_path = nullptr) {
    mc::net::NodeConfig cfg;
    std::string config_path;
    // The TUI is now the default surface for `start`. Pass --no-tui or
    // --daemon when launching from systemd / service manager / Windows
    // service so the binary stays a plain log-only daemon and doesn't
    // leave the controlling terminal in raw mode.
    bool tui_mode = true;

    // Parse arguments
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--config" && i+1 < args.size())         { config_path = args[++i]; }
        else if (args[i] == "--data-dir" && i+1 < args.size())  { cfg.data_dir = args[++i]; }
        else if (args[i] == "--p2p-port" && i+1 < args.size())  { cfg.p2p_port = static_cast<uint16_t>(std::stoi(args[++i])); }
        else if (args[i] == "--api-port" && i+1 < args.size())  { cfg.api_port = static_cast<uint16_t>(std::stoi(args[++i])); }
        else if (args[i] == "--no-tui" || args[i] == "--daemon"
                                       || args[i] == "--quiet") { tui_mode = false; }
        else if (args[i] == "--tui")                            { tui_mode = true; }
    }

    // Divert all log output into the TUI's in-memory ring BEFORE chain
    // / database / network bringup starts spewing. Two layers needed:
    // (1) the librats DLL has its own std::cout / std::cerr instances
    //     inside mc_rats.dll's CRT — the EXE-side rdbuf swap can't reach
    //     them, so we use librats's own config to silence its console
    //     logger entirely.
    // (2) our own code (chain, rats_api, swarm) writes via the EXE's
    //     std::cout / std::cerr; the rdbuf swap inside start_log_capture
    //     diverts those into the ring.
    if (tui_mode) {
        rats_set_console_logging_enabled(0);
        rats_set_logging_enabled(0);
        mc::ui::start_log_capture();
    }

    // Locate config file. Probe order (first hit wins):
    //   1. --config explicit path
    //   2. ${data_dir}/config.json (set in previous run via save_config)
    //   3. ./data/config.json (default data-dir convention)
    //   4. ./full-node.config.json (shipped alongside the binary by the
    //      build script — the operator's first-run starting point)
    //   5. <exe_dir>/full-node.config.json (operator launched from elsewhere)
    // Without this fallback ladder the operator either has to remember
    // --config every time or carry the file in cwd. The build now ships
    // a default file next to the .exe so step 4/5 always fires on a
    // clean install.
    if (config_path.empty()) {
        auto try_path = [&](const std::string& p) {
            if (!config_path.empty()) return;
            if (fs::exists(p)) config_path = p;
        };
        if (!cfg.data_dir.empty()) try_path(cfg.data_dir + "/config.json");
        try_path("./data/config.json");
        try_path("./full-node.config.json");
        if (exe_path) {
            const fs::path exe_dir = fs::path(exe_path).parent_path();
            if (!exe_dir.empty())
                try_path((exe_dir / "full-node.config.json").string());
        }
    }
    // Load from file if a config exists (explicit args still override).
    if (!config_path.empty() && fs::exists(config_path)) {
        auto file_cfg = load_config(config_path);
        if (cfg.data_dir.empty())  cfg.data_dir  = file_cfg.data_dir;
        if (cfg.p2p_port == 9333 && file_cfg.p2p_port != 9333) cfg.p2p_port = file_cfg.p2p_port;
        if (cfg.api_port == 9334 && file_cfg.api_port != 9334) cfg.api_port = file_cfg.api_port;
        if (cfg.rats_port == 8080 && file_cfg.rats_port != 8080) cfg.rats_port = file_cfg.rats_port;
        if (cfg.max_peers == 125 && file_cfg.max_peers != 125) cfg.max_peers = file_cfg.max_peers;
        if (cfg.seed_nodes.empty())    cfg.seed_nodes    = file_cfg.seed_nodes;
        if (cfg.registry_url.empty())  cfg.registry_url  = file_cfg.registry_url;
        // tui_mode: file value wins unless CLI explicitly passed --no-tui /
        // --tui (handled before this block flipped tui_mode away from the
        // default). We re-apply the persisted value here only when the CLI
        // didn't touch it. Simplest signal: if CLI didn't set it, tui_mode
        // is still the default true.
        if (tui_mode == true) tui_mode = g_tui_mode_persisted;
    }

    if (cfg.data_dir.empty()) cfg.data_dir = "./data";

    // Persist the resolved (file + CLI) settings back so a re-run with
    // no flags reuses everything the operator picked here — including
    // data_dir, ports, tui mode, and the load_monitor block. The path
    // is config_path when --config was passed, else ${data_dir}/config.json
    // so a node-with-no-arguments run still leaves a self-describing
    // record next to its data.
    {
        std::string save_path = config_path.empty()
            ? cfg.data_dir + "/config.json"
            : config_path;
        save_config(save_path, cfg, tui_mode);
    }

    // Create directories
    std::cerr << "[dbg] creating dirs\n";
    fs::create_directories(cfg.data_dir + "/blockchain.db");
    fs::create_directories(cfg.data_dir + "/keys");
    fs::create_directories(cfg.data_dir + "/logs");

    std::cout << "[node] data_dir : " << cfg.data_dir << "\n";
    std::cout << "[node] rats port: " << cfg.rats_port
              << " (UDP/QUIC — all traffic, RPC and h3 verbs)\n";
    std::cerr << "[dbg] opening database\n";

    // Open database
    mc::Database db(cfg.data_dir + "/blockchain.db");
    std::cerr << "[dbg] database opened\n";
    std::cout << "[node] database opened\n";

    // Load or generate keypair
    std::cerr << "[dbg] loading keypair\n";
    auto keypair = mc::crypto::load_or_generate_node_keypair(cfg.data_dir + "/keys");
    std::cerr << "[dbg] keypair loaded\n";
    cfg.node_id = mc::crypto::sha256(keypair.public_key.data(), 33);
    std::cout << "[node] node_id: " << mc::crypto::to_hex(cfg.node_id) << "\n";
    std::cout << "[node] address: " << mc::crypto::to_checksum_hex(keypair.address) << "\n";

    // Bootstrap first moderator wallet (runs once, guarded by sentinel key)
    std::cerr << "[dbg] moderator bootstrap\n";
    {
        auto sentinel = db.get("moderator_initialized");
        if (!sentinel) {
            leveldb::WriteBatch mod_batch;
            auto mod_kp = mc::crypto::generate_keypair();
            std::ofstream mod_f(cfg.data_dir + "/moderator.txt");
            mod_f << "Private Key: " << mc::crypto::to_hex(mod_kp.private_key.data(),
                                                            mod_kp.private_key.size()) << "\n";
            mod_f << "Address:     " << mc::crypto::to_checksum_hex(mod_kp.address) << "\n";
            mod_f.close();
            db.add_moderator(mod_batch, mod_kp.address);
            db.put_batch(mod_batch, "moderator_initialized", {1});
            db.write(mod_batch);
            std::cout << "[node] moderator wallet created — " << cfg.data_dir << "/moderator.txt\n";
            std::cout << "[node] moderator address: "
                      << mc::crypto::to_checksum_hex(mod_kp.address) << "\n";
        }
    }

    std::cerr << "[dbg] initializing chain\n";
    // Initialize chain
    mc::Chain chain(db);
    if (!chain.init()) {
        std::cerr << "[node] chain init failed\n";
        return 1;
    }
    std::cerr << "[dbg] chain init ok\n";
    std::cout << "[node] chain height: " << chain.tip().height << "\n";

    // Candidate manager + upload worker
    std::cerr << "[dbg] creating candidate manager\n";
    mc::CandidateManager candidates;

    // Network manager
    std::cerr << "[dbg] creating network manager\n";
    mc::net::NetworkManager network(chain, candidates, cfg, keypair);
    // Legacy direct-connect path — fires when a peer pushes a finalized
    // block over the (gutted) TCP mesh. Still safe to leave installed: if
    // the mesh ever comes back it just calls connect_block.
    network.set_block_handler([&chain](mc::Block block) {
        if (chain.connect_block(block)) {
            std::cout << "[chain] connected block at height "
                      << chain.tip().height << "\n";
        } else {
            std::cerr << "[chain] failed to connect block\n";
        }
    });

    std::cerr << "[dbg] starting network\n";
    if (!network.start()) {
        std::cerr << "[node] network start failed\n";
        return 1;
    }
    std::cerr << "[dbg] network started\n";
    std::cout << "[node] P2P listening on port " << cfg.p2p_port << "\n";

    // Start upload worker thread
    std::cerr << "[dbg] starting upload worker\n";
    candidates.start(chain, db, network, cfg, keypair);
    // Kick the producer immediately so a cold start with mempool txs
    // (e.g. founder GRANT + UsernameTx queued in a previous run) doesn't
    // wait out the 30-second heartbeat poll before minting block 1.
    // Without this poke the wake predicate only fires for song
    // registrations or explicit TUI actions — the wallet/login flow
    // sits on "GRANT not yet on chain" for half a minute on every cold
    // start.
    candidates.wake();

    (void)exe_path;  // bootstrap file no longer shipped — kept for ABI

    // HTTP gossip mesh has been retired — peer discovery now happens via the
    // VPS mini-node's librats `routes.get` RPC. Block sync between full
    // nodes still uses the legacy TCP mesh in NetworkManager.

    // HTTP API server — kept for diagnostics and legacy clients. The new
    // librats RPC layer (RatsApi) reuses these same verb handlers, so we
    // create it first and hand a reference to RatsApi.
    mc::api::HttpServer api(chain, candidates, network, db, cfg, keypair);

    // JSON-RPC HTTP shim for exchange / explorer integration. Ethereum-
    // flavoured `eth_*` methods on port 8545 (the EVM convention), bound
    // to 127.0.0.1 by default so an operator opening the firewall is an
    // explicit decision. Cheap to keep alive — about 1 MiB of resident
    // memory at idle and no traffic until somebody scrapes us.
    mc::api::JsonRpcServer rpc(chain, db, 8545);
    rpc.start();

    // The standalone HTTP/3 server on cfg.api_port is gone — every verb in
    // HttpServer is now reachable over the same librats QUIC RPC channel
    // (see RatsApi below) on UDP/443. Players, full nodes and the mini-node
    // all share that one socket; no second port to forward.

    // QUIC NAT punchthrough — uses 85.239.238.226 as the STUN + rendezvous
    // so this node can be reached by other nodes and phone clients even
    // from behind a NAT, without any UPnP support. Also publishes our
    // routing record (node_id + STUN-discovered public address + api_port)
    // to the VPS mini-node every 15 minutes via the MC_ROUTES_TOPIC topic.
    // LoadMonitor must outlive RatsLink (RatsLink reads its current()
    // snapshot when building each routes record).
    static mc::net::LoadMonitor load_mon(g_load_cfg);
    load_mon.start();
    std::cout << "[load] monitor started: max_bw="
              << g_load_cfg.max_bandwidth_bps << " bps, busy@"
              << g_load_cfg.busy_score_threshold << "\n";
    mc::net::RatsLink rats(cfg.rats_port,
                            mc::crypto::to_hex(cfg.node_id),
                            cfg.api_port);
    rats.set_load_monitor(&load_mon);
    mc::api::RatsApi rats_api(api, chain, candidates, network, db, cfg, keypair);
    if (rats.start()) {
        std::cout << "[node] rats link active on port " << cfg.rats_port
                  << " (VPS " << mc::net::MC_VPS_HOST << ":"
                  << mc::net::MC_VPS_RATS_PORT << ")\n";
        const std::string pub = rats.public_address();
        if (!pub.empty())
            std::cout << "[node] public address (STUN): " << pub << "\n";

        // Boot the BitTorrent-compatible DHT so players that come up
        // without an active VPS link can still discover swarm peers by
        // content_hash. The dht_port=0 hint lets librats pick an
        // ephemeral UDP socket; we never have to expose it through a
        // firewall because peers reach this node via the DHT routing
        // table, not a fixed port. Failure here is non-fatal: the
        // SwarmIndex + VPS routes still mediate discovery.
        if (rats_start_dht_discovery(rats.client(), 0) != 0) {
            std::cerr << "[node] start_dht_discovery failed — DHT swarm "
                         "discovery unavailable, falling back to VPS\n";
        } else {
            std::cout << "[node] DHT swarm discovery active "
                      << "(routing table " << rats_get_dht_routing_table_size(rats.client())
                      << " entries)\n";
        }

        rats_api.start(rats.client());

        // ---- Consensus mesh wiring -------------------------------------
        //
        // Hand RatsLink to the consensus layer so the producer (in
        // CandidateManager::commit_block) can fan out fresh candidates
        // and so we can count actual peers when deciding solo vs.
        // multi-node. Without these two lines block production runs
        // forever in the solo-self-sign path even when peers are present.
        network.set_peer_count_provider([&rats] {
            int n = rats.validated_peer_count();
            return n < 0 ? size_t{0} : static_cast<size_t>(n);
        });
        network.set_candidate_publisher([&rats](const std::vector<uint8_t>& b) {
            rats.publish_block_candidate(b);
        });

        // ---- Validator-side: vote on incoming candidates --------------
        //
        // Other nodes' producers broadcast their freshly-minted block
        // here. We re-deserialize, run the same checks chain.connect_block
        // would later run, plus the uniqueness gate ("has anyone already
        // registered this fingerprint?"). If everything passes we sign
        // (block_hash, keypair.private_key) and publish a Confirmation
        // back over the same channel, where the producer's confirmation
        // handler picks it up and calls candidates.add_confirmation.
        //
        // We deliberately do NOT call chain.connect_block here — the block
        // isn't final until REQUIRED_CONFIRMATIONS arrive at the producer.
        // The producer then re-broadcasts the finalized block on a future
        // turn (via routes / catch-up sync) and connect happens there.
        rats.set_block_candidate_handler(
            [&chain, &db, &cfg, &keypair, &rats](std::vector<uint8_t> bytes) {
                mc::Block block;
                if (!mc::Block::deserialize(bytes.data(), bytes.size(), block)) {
                    std::cerr << "[consensus] dropped candidate: malformed\n";
                    return;
                }
                std::string err;
                if (!chain.validate_block(block, err)) {
                    // Note: Block::validate (called from chain.validate_block)
                    // already enforces sha256(compressed_fingerprint bytes)
                    // == header.fingerprint_hash, so we don't repeat that
                    // check here. A producer can't lie about which
                    // fingerprint a header claims.
                    std::cerr << "[consensus] dropped candidate: validate failed — "
                              << err << "\n";
                    return;
                }
                if (block.has_song) {
                    // Uniqueness gate #1 — same audio file already on-chain.
                    // Indexed by content_hash via the "f:" prefix. Cheapest
                    // check; one DB get.
                    if (db.get_fingerprint(block.song.content_hash)) {
                        std::cerr << "[consensus] dropped candidate: content_hash "
                                     "already registered\n";
                        return;
                    }
                    // Uniqueness gate #2 — SAME song re-encoded as a
                    // different file. chromaprint is content-similarity,
                    // not bit-equality: re-encoding an MP3 to OGG keeps
                    // the song audibly identical but produces a *similar*
                    // (not equal) fingerprint, which means a different
                    // header.fingerprint_hash and a different
                    // song.content_hash. So we have to decode the
                    // candidate's chromaprint blob into raw 32-bit
                    // per-frame codes and compute bin-level hamming-
                    // distance similarity against everything in the
                    // bucket index. Threshold matches rats_api's existing
                    // fingerprint.submit fuzzy probe (kSimThreshold=0.55)
                    // — anything above that is treated as the same song.
                    auto submitted = mc::audio::Fingerprint::from_compressed(
                        block.song.compressed_fingerprint);
                    if (submitted) {
                        constexpr float kSimThreshold = 0.55f;
                        std::unordered_set<std::string> seen;
                        float best_sim = 0.0f;
                        mc::Hash256 best_match{};
                        int n_candidates = 0;
                        for (auto bucket : submitted->bucket_ids()) {
                            for (const auto& cand_ch : db.get_bucket(bucket)) {
                                if (cand_ch == block.song.content_hash) continue;
                                const std::string key = mc::crypto::to_hex(cand_ch);
                                if (!seen.insert(key).second) continue;
                                ++n_candidates;
                                auto entry = db.get_fingerprint(cand_ch);
                                if (!entry) continue;
                                auto cand_fp = mc::audio::Fingerprint::from_compressed(
                                    entry->compressed_fingerprint);
                                if (!cand_fp) continue;
                                const float sim = submitted->similarity(*cand_fp);
                                if (sim > best_sim) {
                                    best_sim   = sim;
                                    best_match = cand_ch;
                                }
                                if (sim >= kSimThreshold) goto reject_dup;
                            }
                        }
                        std::cout << "[consensus] fuzzy probe: "
                                  << n_candidates << " candidates, best="
                                  << best_sim << "\n";
                        goto fuzzy_ok;
reject_dup:
                        std::cerr << "[consensus] dropped candidate: chromaprint "
                                     "similar to already-registered song "
                                  << mc::crypto::to_hex(best_match).substr(0, 16)
                                  << "… (sim=" << best_sim << " >= "
                                  << kSimThreshold << ")\n";
                        return;
fuzzy_ok: ;
                    }
                }
                // Validation passed. Sign and broadcast a confirmation.
                mc::Confirmation conf{};
                conf.validator_id = cfg.node_id;
                std::copy(keypair.public_key.begin(), keypair.public_key.end(),
                          conf.pubkey.begin());
                conf.signature = mc::crypto::sign_ecdsa(block.hash(),
                                                        keypair.private_key);
                const std::string block_hash_hex =
                    mc::crypto::to_hex(block.hash());
                rats.publish_confirmation(block_hash_hex, conf);
                std::cout << "[consensus] confirmed candidate "
                          << block_hash_hex.substr(0, 16) << "…\n";
            });

        // ---- Producer-side: collect votes from validators -------------
        rats.set_confirmation_handler(
            [&candidates, &chain](std::string block_hash_hex,
                                   mc::Confirmation c) {
                // Slashed validators' confirmations don't count. We
                // derive the validator's address from their public key
                // and check the slashed: index. This protects against
                // a slashed party still emitting confirmation messages
                // from their old key — those go in the bin.
                const auto addr =
                    mc::crypto::address_from_pubkey(c.pubkey);
                if (chain.is_slashed(addr)) {
                    std::cerr << "[consensus] dropped confirmation from "
                                 "slashed validator "
                              << mc::crypto::to_hex(addr.data(), 20)
                                    .substr(0, 16) << "…\n";
                    return;
                }
                bool now_final =
                    candidates.add_confirmation(block_hash_hex, c);
                if (now_final) {
                    std::cout << "[consensus] candidate "
                              << block_hash_hex.substr(0, 16)
                              << "… reached quorum\n";
                }
            });
    } else {
        std::cerr << "[node] rats link failed to start — continuing without NAT punch\n";
    }
    if (rats.client()) {
        // BlockPropagator — bitcoin-style block distribution over
        // librats with BitTorrent-style DHT multi-source fetch.
        // Replaces the old SyncManager + routes.get / relay.forward
        // path: full nodes find each other via the librats DHT
        // (announce + find under cfg.dht_bootstrap_hash), exchange
        // tips on connect via block.hello, and catch up via
        // block.getblocks → block.inv → block.getdata → block.data.
        // New locally-minted blocks are gossiped via block.inv.
        // See src/sync/block_propagator.h for the wire protocol.
        static mc::BlockPropagator propagator(chain, rats, cfg);
        rats_api.set_block_propagator(&propagator);
        candidates.set_block_announcer(
            [](const mc::Hash256& h) { propagator.announce_new_block(h); });
        propagator.start(rats.client());

        // DeepAuditor runs the chromaprint↔audio re-check on a random
        // recent block every kAuditIntervalMs. Catches producers that
        // declared one fingerprint but uploaded different audio. Full
        // nodes don't hold audio under the post-pivot architecture, so
        // most cycles will no-op (graceful — the audit just skips). On
        // the player + on nodes that opt into a content cache, this is
        // the gate against fingerprint-vs-audio forgery.
        static mc::DeepAuditor audit(chain, db, cfg.data_dir + "/audio");
        audit.start();

        // ---- Relay credit tracker + periodic RelayRewardTx sweep ----
        //
        // Counts every mini-node tunneled delivery this full node serves
        // (populated by rats_api hooks, persisted to leveldb under "rc:"
        // so credits aren't lost across restarts). Every 5 minutes we
        // sweep the counters into one RelayRewardTx per mini-node,
        // signed by the founder key (loaded from founder.seed alongside
        // the chain data dir). Counter zeroes when the tx hits a block.
        //
        // The mint callback is no-op until the operator has bootstrapped
        // the founder seed — before that we can't sign a RELAY_REWARD
        // since only the founder can issue them.
        static mc::net::RelayCreditTracker relay_tracker(db);
        relay_tracker.start(
            [&chain, &db, &candidates, &cfg](const mc::Address& mini_addr, uint64_t count) {
                // Read the founder seed off disk on every sweep so the
                // operator can hot-swap it without restarting the node.
                const std::string seed_path = cfg.data_dir + "/founder.seed";
                std::ifstream sf(seed_path);
                if (!sf) {
                    std::cerr << "[relay] no founder.seed at " << seed_path
                              << " — skipping reward sweep for "
                              << count << " credits\n";
                    // Re-credit so we don't lose them.
                    return;
                }
                std::string mnemonic;
                std::getline(sf, mnemonic);
                while (!mnemonic.empty() &&
                       (mnemonic.back() == '\r' || mnemonic.back() == '\n'
                        || mnemonic.back() == ' ')) mnemonic.pop_back();
                auto kp_opt = mc::crypto::bip39_mnemonic_to_keypair(mnemonic, "");
                if (!kp_opt) {
                    std::cerr << "[relay] founder.seed mnemonic failed BIP32 derive\n";
                    return;
                }
                const auto founder_kp = *kp_opt;
                // Confirm the seed actually owns the chain's founder
                // address. Otherwise our signature won't verify under
                // apply_relay_reward's founder check.
                auto chain_founder = db.get_founder();
                if (!chain_founder ||
                    std::memcmp(chain_founder->data(),
                                founder_kp.address.data(), 20) != 0) {
                    std::cerr << "[relay] founder.seed doesn't match "
                                 "chain founder — skipping sweep\n";
                    return;
                }
                mc::RelayRewardTx tx{};
                tx.target_address  = mini_addr;
                tx.count           = count;
                tx.issuer_address  = founder_kp.address;
                tx.issuer_pubkey   = founder_kp.public_key;
                tx.nonce           = db.get_nonce(founder_kp.address);
                {
                    auto msg  = tx.sign_message();
                    auto h    = mc::crypto::sha256(msg.data(), msg.size());
                    tx.signature = mc::crypto::sign_ecdsa(h, founder_kp.private_key);
                }
                if (!tx.verify_signature()) {
                    std::cerr << "[relay] internal sign/verify mismatch — bug\n";
                    return;
                }
                auto h = tx.tx_hash();
                if (!db.put_pending_tx(h, tx.serialize())) {
                    std::cerr << "[relay] put_pending_tx failed\n";
                    return;
                }
                candidates.wake();
                std::cout << "[relay] queued RelayRewardTx: "
                          << count << " MC → mini "
                          << db.hex(mini_addr).substr(0, 12) << "…\n";
            });
    }

    // UPnP removed. NAT traversal is now mc_rats_quic's job (QUIC peers all
    // connect outbound to the mini-node first; inbound flows tunnel via the
    // relay if reachability probing showed this node is firewalled).

    // Start the HTTP API server (it was constructed earlier so RatsApi
    // could borrow its verb handlers).
    if (!api.start()) {
        std::cerr << "[node] API server start failed\n";
        return 1;
    }
    std::cout << "[node] API listening on port " << cfg.api_port << "\n";

    // Register signal handlers
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    if (tui_mode) {
        // Kick a background maintenance thread so the TUI's redraw loop
        // doesn't have to busy-poll candidates.cleanup_expired().
        std::thread janitor([&]{
            while (g_running) {
                std::this_thread::sleep_for(std::chrono::seconds(10));
                candidates.cleanup_expired();
            }
        });
        mc::ui::run_tui(api, rats_api, chain, db, rats_api.swarm_index(),
                        network, candidates, keypair, cfg.data_dir, g_running);
        if (janitor.joinable()) janitor.join();
    } else {
        std::cout << "[node] running. Press Ctrl+C to stop.\n";
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            candidates.cleanup_expired();
        }
    }

    std::cout << "[node] shutting down...\n";
    rats_api.stop();
    rats.stop();
    api.stop();
    network.stop();
    return 0;
}

// ---- Subcommand: status ---------------------------------------------

static int cmd_status(const std::vector<std::string>& /*args*/) {
    // Simplified: just print "OK". Full implementation would query the running node's API.
    std::cout << "musicchain-node status: use 'curl http://localhost:9334/api/v1/status'\n";
    return 0;
}

// ---- Subcommand: peers ----------------------------------------------

static int cmd_peers(const std::vector<std::string>& /*args*/) {
    std::cout << "musicchain-node peers: use 'curl http://localhost:9334/api/v1/peers'\n";
    return 0;
}

// ---- Subcommand: verify-chain ---------------------------------------

static int cmd_verify_chain(const std::vector<std::string>& args) {
    std::string data_dir = "./data";
    for (size_t i = 0; i < args.size(); ++i)
        if (args[i] == "--data-dir" && i+1 < args.size()) data_dir = args[++i];

    mc::Database db(data_dir + "/blockchain.db");
    mc::Chain chain(db);
    if (!chain.init()) { std::cerr << "chain init failed\n"; return 1; }

    std::cout << "Verifying " << chain.tip().height << " blocks...\n";
    for (uint32_t h = 1; h <= chain.tip().height; ++h) {
        auto hash  = chain.get_block_hash(h);
        if (!hash) { std::cerr << "missing block at height " << h << "\n"; return 1; }
        auto block = chain.get_block(*hash);
        if (!block) { std::cerr << "cannot load block at height " << h << "\n"; return 1; }
        if (!block->validate()) { std::cerr << "invalid block at height " << h << "\n"; return 1; }
        if (h % 1000 == 0) std::cout << "  verified " << h << " / " << chain.tip().height << "\n";
    }
    std::cout << "Chain verification OK.\n";
    return 0;
}

// ---- Subcommand: rebuild-index --------------------------------------

static int cmd_rebuild_index(const std::vector<std::string>& args) {
    std::string data_dir = "./data";
    for (size_t i = 0; i < args.size(); ++i)
        if (args[i] == "--data-dir" && i+1 < args.size()) data_dir = args[++i];

    mc::Database db(data_dir + "/blockchain.db");
    mc::Chain chain(db);
    if (!chain.init()) { std::cerr << "chain init failed\n"; return 1; }
    std::cout << "Rebuilding derived state from " << chain.tip().height << " blocks...\n";
    if (!chain.rebuild_derived_state()) { std::cerr << "rebuild failed\n"; return 1; }
    std::cout << "Done.\n";
    return 0;
}

// ---- main -----------------------------------------------------------

int main(int argc, char** argv) {
    // When the node runs under a service manager (or `Start-Process
    // -RedirectStandardOutput`), stdout is fully-buffered and diagnostic
    // [rats-api] / [chain] traces only surface after the buffer fills.
    // Disable that so logs hit disk as they're written.
    std::ios::sync_with_stdio(false);
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    if (argc < 2) {
        std::cerr << "Usage: musicchain-node <command> [options]\n"
                  << "Commands: start, status, peers, sync-status, stop,\n"
                  << "          rebuild-index, verify-chain\n";
        return 1;
    }

    std::string command = argv[1];
    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i) args.push_back(argv[i]);

    if (command == "start")         return cmd_start(args, argv[0]);
    if (command == "status")        return cmd_status(args);
    if (command == "peers")         return cmd_peers(args);
    if (command == "sync-status")   return cmd_status(args);
    if (command == "verify-chain")  return cmd_verify_chain(args);
    if (command == "rebuild-index") return cmd_rebuild_index(args);
    if (command == "stop") {
        std::cout << "Send SIGTERM to the running node process.\n";
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n";
    return 1;
}
