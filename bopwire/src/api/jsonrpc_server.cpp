#include "jsonrpc_server.h"
#include "../core/block.h"
#include "../core/transaction.h"
#include "../crypto/hash.h"
#include "../crypto/keys.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <iostream>

namespace mc::api {

namespace {

using nlohmann::json;

// ---- Hex helpers ----------------------------------------------------

std::string u64_hex(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx",
                  static_cast<unsigned long long>(v));
    return buf;
}

std::string u32_hex(uint32_t v) {
    return u64_hex(static_cast<uint64_t>(v));
}

std::string bytes_hex(const uint8_t* data, size_t len) {
    static const char lut[] = "0123456789abcdef";
    std::string out;
    out.reserve(2 + len * 2);
    out.append("0x");
    for (size_t i = 0; i < len; ++i) {
        out.push_back(lut[(data[i] >> 4) & 0xF]);
        out.push_back(lut[ data[i]       & 0xF]);
    }
    return out;
}

bool parse_address_param(const std::string& s, Address& out) {
    // Accept lowercase-with-0x and full EIP-55 mixed-case. Mixed-case
    // with a bad checksum is rejected so an exchange's eth_getBalance
    // typo surfaces at the JSON-RPC boundary instead of returning the
    // balance of an unrelated address.
    return mc::crypto::parse_address_checksummed(s, out);
}

bool parse_hash_param(const std::string& s, Hash256& out) {
    std::string h = s;
    if (h.size() == 66 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) {
        h = h.substr(2);
    }
    if (h.size() != 64) return false;
    return mc::crypto::parse_hash256(h, out);
}

bool parse_block_number(const std::string& s, uint32_t tip_height,
                        uint32_t& out) {
    if (s == "latest" || s == "pending" || s == "safe" || s == "finalized") {
        out = tip_height;
        return true;
    }
    if (s == "earliest") { out = 0; return true; }
    // Hex form "0x..."
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        try {
            out = static_cast<uint32_t>(std::stoul(s.substr(2), nullptr, 16));
            return true;
        } catch (...) { return false; }
    }
    try {
        out = static_cast<uint32_t>(std::stoul(s, nullptr, 10));
        return true;
    } catch (...) { return false; }
}

// ---- JSON-RPC envelopes --------------------------------------------

json make_result(const json& id, const json& result) {
    return json{{"jsonrpc", "2.0"}, {"result", result}, {"id", id}};
}

json make_error(const json& id, int code, const std::string& msg) {
    return json{
        {"jsonrpc", "2.0"},
        {"error",   {{"code", code}, {"message", msg}}},
        {"id",      id},
    };
}

// ---- Block + tx renderers ------------------------------------------

json render_tx_summary(const std::vector<uint8_t>& raw, size_t /*idx*/) {
    if (raw.empty()) return json::object();
    // Compute the tx hash the same way the chain does (sha256 of the
    // serialized bytes) so external scanners can dedupe by hash.
    auto h = mc::crypto::sha256(raw.data(), raw.size());
    return json{
        {"hash", bytes_hex(h.data(), 32)},
        {"raw",  bytes_hex(raw.data(), raw.size())},
        {"type", u32_hex(static_cast<uint32_t>(raw[0]))},
    };
}

json render_block(const Block& block, uint32_t height, bool full_txs) {
    auto bh = block.hash();
    json out = {
        {"number",         u32_hex(height)},
        {"hash",           bytes_hex(bh.data(), 32)},
        {"parentHash",     bytes_hex(block.header.prev_hash.data(), 32)},
        {"timestamp",      u64_hex(block.header.timestamp_ms / 1000)},
        {"transactionCount", u64_hex(block.transactions.size())},
    };
    json txs = json::array();
    if (full_txs) {
        for (size_t i = 0; i < block.transactions.size(); ++i) {
            txs.push_back(render_tx_summary(block.transactions[i], i));
        }
    } else {
        for (const auto& raw : block.transactions) {
            if (raw.empty()) continue;
            auto h = mc::crypto::sha256(raw.data(), raw.size());
            txs.push_back(bytes_hex(h.data(), 32));
        }
    }
    out["transactions"] = txs;
    return out;
}

} // namespace

// ---- Pimpl impl + dispatch -----------------------------------------

struct JsonRpcServer::Impl {
    httplib::Server svr;
};

JsonRpcServer::JsonRpcServer(Chain& chain, Database& db,
                             uint16_t port, std::string bind_addr)
    : chain_(chain), db_(db), port_(port), bind_addr_(std::move(bind_addr)),
      impl_(std::make_unique<Impl>()) {}

JsonRpcServer::~JsonRpcServer() { stop(); }

bool JsonRpcServer::start() {
    if (running_.load()) return true;

    impl_->svr.Post("/", [this](const httplib::Request& req,
                                 httplib::Response& res) {
        auto body = dispatch(req.body);
        res.set_content(body, "application/json");
    });
    impl_->svr.Get("/", [this](const httplib::Request& /*req*/,
                                httplib::Response& res) {
        // Friendly GET so an exchange engineer pasting the URL into a
        // browser sees something useful instead of a method-not-allowed.
        json info = {
            {"name", "bopwire"},
            {"jsonrpc", "2.0"},
            {"chainId", u32_hex(MC_CHAIN_ID)},
            {"docs", "POST a JSON-RPC 2.0 envelope to this endpoint"},
        };
        res.set_content(info.dump(2), "application/json");
    });

    running_.store(true);
    thread_ = std::thread([this]{
        std::cout << "[jsonrpc] listening on "
                  << bind_addr_ << ":" << port_ << "\n";
        impl_->svr.listen(bind_addr_.c_str(), port_);
        running_.store(false);
    });
    return true;
}

void JsonRpcServer::stop() {
    if (impl_) impl_->svr.stop();
    if (thread_.joinable()) thread_.join();
    running_.store(false);
}

std::string JsonRpcServer::dispatch(const std::string& body) {
    json req_id = nullptr;
    try {
        auto req = json::parse(body);
        req_id = req.value("id", json(nullptr));
        const std::string method = req.value("method", std::string());
        const json params = req.value("params", json::array());

        if (method == "eth_chainId") {
            return make_result(req_id, u32_hex(MC_CHAIN_ID)).dump();
        }
        if (method == "net_version") {
            return make_result(req_id, std::to_string(MC_CHAIN_ID)).dump();
        }
        if (method == "web3_clientVersion") {
            return make_result(req_id,
                std::string("bopwire/0.2.0/cpp")).dump();
        }
        if (method == "eth_blockNumber") {
            return make_result(req_id, u32_hex(chain_.tip().height)).dump();
        }
        if (method == "eth_gasPrice") {
            // Bopwire has no gas market; reply with a canonical 1 so
            // wallet UIs that multiply by gas-used still get a sane
            // total. Exchanges generally ignore gas for read scans.
            return make_result(req_id, std::string("0x1")).dump();
        }
        if (method == "eth_getBalance") {
            if (!params.is_array() || params.empty() || !params[0].is_string()) {
                return make_error(req_id, -32602, "expected [address, block]").dump();
            }
            Address addr{};
            if (!parse_address_param(params[0].get<std::string>(), addr)) {
                return make_error(req_id, -32602, "bad address").dump();
            }
            uint64_t bal = db_.get_balance(addr);
            return make_result(req_id, u64_hex(bal)).dump();
        }
        if (method == "eth_getTransactionCount") {
            if (!params.is_array() || params.empty() || !params[0].is_string()) {
                return make_error(req_id, -32602, "expected [address, block]").dump();
            }
            Address addr{};
            if (!parse_address_param(params[0].get<std::string>(), addr)) {
                return make_error(req_id, -32602, "bad address").dump();
            }
            uint64_t n = db_.get_nonce(addr);
            return make_result(req_id, u64_hex(n)).dump();
        }
        if (method == "eth_getBlockByNumber") {
            if (!params.is_array() || params.size() < 1 || !params[0].is_string()) {
                return make_error(req_id, -32602, "expected [block_number, full_tx_flag]").dump();
            }
            uint32_t h = 0;
            if (!parse_block_number(params[0].get<std::string>(),
                                    chain_.tip().height, h)) {
                return make_error(req_id, -32602, "bad block number").dump();
            }
            bool full = params.size() >= 2 && params[1].is_boolean() && params[1].get<bool>();
            auto hh = chain_.get_block_hash(h);
            if (!hh) {
                return make_result(req_id, nullptr).dump();
            }
            auto blk = chain_.get_block(*hh);
            if (!blk) {
                return make_result(req_id, nullptr).dump();
            }
            return make_result(req_id, render_block(*blk, h, full)).dump();
        }
        if (method == "eth_getBlockByHash") {
            if (!params.is_array() || params.empty() || !params[0].is_string()) {
                return make_error(req_id, -32602, "expected [block_hash, full_tx_flag]").dump();
            }
            Hash256 bh{};
            if (!parse_hash_param(params[0].get<std::string>(), bh)) {
                return make_error(req_id, -32602, "bad block hash").dump();
            }
            bool full = params.size() >= 2 && params[1].is_boolean() && params[1].get<bool>();
            auto blk = chain_.get_block(bh);
            if (!blk) return make_result(req_id, nullptr).dump();
            auto h = chain_.get_block_height(bh);
            return make_result(req_id,
                render_block(*blk, h.value_or(0), full)).dump();
        }
        if (method == "eth_getTransactionByHash") {
            if (!params.is_array() || params.empty() || !params[0].is_string()) {
                return make_error(req_id, -32602, "expected [tx_hash]").dump();
            }
            Hash256 wanted{};
            if (!parse_hash_param(params[0].get<std::string>(), wanted)) {
                return make_error(req_id, -32602, "bad tx hash").dump();
            }
            // Walk back from the tip until we find the tx. Costly for
            // deep chains; a future tx-index column-family makes this
            // O(1). For now the chain is small enough this is fine.
            const uint32_t tip = chain_.tip().height;
            for (uint32_t h = tip; h > 0; --h) {
                auto bh = chain_.get_block_hash(h);
                if (!bh) continue;
                auto blk = chain_.get_block(*bh);
                if (!blk) continue;
                for (size_t i = 0; i < blk->transactions.size(); ++i) {
                    const auto& raw = blk->transactions[i];
                    if (raw.empty()) continue;
                    auto th = mc::crypto::sha256(raw.data(), raw.size());
                    if (std::memcmp(th.data(), wanted.data(), 32) == 0) {
                        json out = render_tx_summary(raw, i);
                        out["blockNumber"]  = u32_hex(h);
                        out["blockHash"]    = bytes_hex(bh->data(), 32);
                        out["transactionIndex"] = u32_hex(static_cast<uint32_t>(i));
                        return make_result(req_id, out).dump();
                    }
                }
            }
            return make_result(req_id, nullptr).dump();
        }

        if (method == "eth_sendRawTransaction" || method == "eth_call") {
            return make_error(req_id, -32601,
                method + " not supported — bopwire uses its own "
                "tx format; submit signed bytes via the librats RPC "
                "or the upcoming /submit_tx endpoint").dump();
        }

        return make_error(req_id, -32601, "method not supported").dump();
    } catch (const std::exception& e) {
        return make_error(req_id, -32700,
                          std::string("parse error: ") + e.what()).dump();
    }
}

} // namespace mc::api
