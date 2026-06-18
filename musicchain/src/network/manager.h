#pragma once
#include <cstdint>
#include "../core/chain.h"
#include "../consensus/candidate.h"
#include "../crypto/keys.h"
#include <memory>
#include <vector>
#include <mutex>
#include <functional>
#include <unordered_map>
#include <array>
#include <string>

namespace mc::net {

struct NodeConfig {
    std::string data_dir;
    // librats peer-to-peer talks TCP on `rats_port` (default 8080,
    // librats's project default). HTTP/3 listener (msh3) runs separately
    // on UDP/443 from src/transport/h3_server.cpp. `p2p_port` /
    // `api_port` are legacy fields retained for config-file compat with
    // older deploys; no listeners actually open on them.
    uint16_t    p2p_port     = 9333;
    uint16_t    api_port     = 9334;
    uint16_t    rats_port    = 8080;
    std::vector<std::string> seed_nodes;
    // Explicit "host:port" full-node sync seeds for BlockPropagator's
    // initial dial. Optional — when empty, BlockPropagator falls back to
    // librats DHT discovery (announce + find under the configured
    // dht_bootstrap_hash). Useful for a brand-new VPS that hasn't yet
    // populated its DHT routing table, or for an air-gapped test where
    // you want a deterministic peer.
    std::vector<std::string> sync_seeds;
    // sha1-hex DHT key every full node announces itself under. Anyone
    // searching this key on the BitTorrent-style DHT gets back the
    // set of musicchain full nodes currently online. Defaults to
    // sha1("musicchain-fullnode-mainnet") computed at boot; override
    // in config to spin up a private network.
    std::string dht_bootstrap_hash;
    uint32_t    max_peers    = 125;
    uint32_t    max_sessions = 10000;
    bool        validator_enabled = true;
    std::string log_level    = "info";
    Hash256     node_id      = {};
    std::string registry_url = {};
};

// Kept around so any future inbound-route persistence has a place to live —
// today populated only by the mini-node's route forwarder, never used by
// NetworkManager itself.
struct DhtEntry {
    std::array<uint8_t, 16> ipv6       = {};
    uint16_t                p2p_port   = 0;
    uint16_t                api_port   = 0;
    Hash256                 node_id    = {};
    uint64_t                last_seen_ms = 0;

    std::string ipv6_str() const;
    std::string api_url() const;
};

/// `NetworkManager` is now a thin shim retained so existing call sites compile
/// and link. The real transport is `mc_rats_quic`. All legacy methods are
/// no-ops or trivially-empty getters until callers migrate over to direct
/// rats calls.
class NetworkManager {
public:
    NetworkManager(Chain& chain, CandidateManager& candidates,
                   const NodeConfig& config,
                   const crypto::KeyPair& keypair = {});
    ~NetworkManager();

    bool start();
    void stop();

    // Block-handler hook — no longer fires because no inbound mesh exists.
    using BlockHandler = std::function<void(mc::Block)>;
    void set_block_handler(BlockHandler h) { on_block_ = std::move(h); }

    // Outbound broadcast is now a no-op; mc_rats_quic is the broadcast path.
    // We keep the signature so consensus/candidate code doesn't have to be
    // rewritten in this phase.
    template <typename T> void broadcast(const T&) {}

    // ---- peer_count plumbing ----------------------------------------
    //
    // CandidateManager::commit_block branches on peer_count(): == 0 picks
    // the solo self-sign fast path, > 0 broadcasts the candidate and
    // waits for confirmations. Until node_main hands us a real provider
    // we return 0 (correct for boot, before RatsLink is up).
    using PeerCountProvider = std::function<size_t()>;
    void   set_peer_count_provider(PeerCountProvider p) { peers_ = std::move(p); }
    size_t peer_count() const { return peers_ ? peers_() : 0; }

    // ---- candidate publisher ----------------------------------------
    //
    // Hook so CandidateManager can fan out a freshly-minted candidate
    // block to the validator peer set. node_main wires this to
    // RatsLink::publish_block_candidate. Until then it's a no-op — the
    // self-sign path doesn't need it.
    using CandidatePublisher =
        std::function<void(const std::vector<uint8_t>& /*block_bytes*/)>;
    void set_candidate_publisher(CandidatePublisher p) { publish_ = std::move(p); }
    void publish_candidate(const std::vector<uint8_t>& bytes) const {
        if (publish_) publish_(bytes);
    }

    // Diagnostic helpers — all return empty since the TCP mesh is gone.
    // Callers should migrate to `rats_get_peer_count()` etc.
    std::vector<std::string> connected_peers() const { return {}; }
    std::vector<DhtEntry>    get_dht_peers()    const { return {}; }
    std::string              own_ipv6_str()     const { return {}; }
    uint32_t                 chain_height()     const;
    bool verify_block_checksum(uint32_t, const Hash256&) { return true; }
    void connect_peer(const std::string&, uint16_t) {}
    void inject_peer(const std::string&, uint16_t, const Hash256&) {}

private:
    Chain&             chain_;
    CandidateManager&  candidates_;
    NodeConfig         config_;
    crypto::KeyPair    keypair_;
    BlockHandler       on_block_;
    PeerCountProvider  peers_;
    CandidatePublisher publish_;
};

} // namespace mc::net
