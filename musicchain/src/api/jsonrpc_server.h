#pragma once
#include "../core/chain.h"
#include "../storage/database.h"
#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace mc::api {

// JSON-RPC HTTP server speaking an Ethereum-flavoured `eth_*` API. The
// goal is "exchange integration teams can point their existing EVM
// scanner at us and read balances + transactions without writing a
// custom client." A handful of standard methods is enough; we
// deliberately do NOT try to be EVM-compatible at the execution layer
// (no smart contracts, no eth_call dispatch, no logs / receipts).
//
// Listens on a single TCP port (8545 by default — the Ethereum
// convention). HTTP/1.1 only, POST requests with JSON-RPC 2.0
// envelopes. Bound to localhost by default; exchanges that want to
// scrape it open the firewall themselves.
//
// Methods supported (read-only):
//   eth_chainId             → MC_CHAIN_ID as hex
//   net_version             → MC_CHAIN_ID as decimal string
//   web3_clientVersion      → "musicchain/<version>"
//   eth_blockNumber         → current tip height as hex
//   eth_gasPrice            → "0x1" (we have no gas market)
//   eth_getBalance          → db.get_balance(addr) as hex
//   eth_getTransactionCount → db.get_nonce(addr) as hex
//   eth_getBlockByNumber    → block summary
//   eth_getBlockByHash      → block summary
//   eth_getTransactionByHash→ tx summary
//
// Write methods (eth_sendRawTransaction et al.) stub out with a
// standardised error explaining the chain accepts native musicchain
// transactions over its existing librats RPC. Adding an Ethereum-RLP
// translation layer is a future Chunk.

class JsonRpcServer {
public:
    JsonRpcServer(Chain& chain, Database& db, uint16_t port,
                  std::string bind_addr = "127.0.0.1");
    ~JsonRpcServer();

    bool start();
    void stop();

    bool is_running() const { return running_.load(); }
    uint16_t port() const { return port_; }

private:
    Chain&            chain_;
    Database&         db_;
    uint16_t          port_;
    std::string       bind_addr_;
    std::atomic<bool> running_{false};

    // Server object lives in the .cpp so the header doesn't pull in
    // <httplib.h> (which is huge).
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::thread           thread_;

    std::string dispatch(const std::string& body);
};

} // namespace mc::api
