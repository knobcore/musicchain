// mc_rats_quic.cpp — librats-API-compatible transport implemented on msquic.
//
// Wire protocol on top of QUIC streams:
//   • Per-connection control stream (id 0, opened by initiator): 20 raw
//     peer-id bytes, no length prefix — receiver reads exactly 20 and we
//     hex-encode for the public API. Sent once at handshake, then the
//     stream is held open as a keep-alive.
//   • Typed-message stream: single send + recv cycle. Frame layout:
//       byte  : 'M'                  (kind tag)
//       u32 LE: type-length
//       u32 LE: data-length
//       bytes : type
//       bytes : data
//     Stream closes after one frame.
//   • Binary stream:
//       byte  : 'B'
//       u64 LE: data-length
//       bytes : data
//     Stream closes after one frame.
//
// TLS: msquic insists on a credential even for the listener. We synthesize a
// self-signed runtime cert (Schannel via SCHANNEL_CRED on Windows) and the
// client accepts any cert (we trust the rendezvous to vouch for identity at
// the application layer).

#define MCR_BUILDING_DLL 1
#include "mc_rats_quic.h"

#include <msquic.h>
#include "cert_util.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// ---- msquic API singleton ------------------------------------------

const QUIC_API_TABLE* g_api = nullptr;
std::mutex            g_api_mu;
int                   g_api_refcount = 0;

bool ensure_api() {
    std::lock_guard<std::mutex> lk(g_api_mu);
    if (g_api) { ++g_api_refcount; return true; }
    const QUIC_API_TABLE* api = nullptr;
    if (QUIC_FAILED(MsQuicOpen2(&api))) {
        std::cerr << "[mcr] MsQuicOpen2 failed\n";
        return false;
    }
    g_api = api;
    g_api_refcount = 1;
    return true;
}

void release_api() {
    std::lock_guard<std::mutex> lk(g_api_mu);
    if (--g_api_refcount > 0) return;
    if (g_api) { MsQuicClose(g_api); g_api = nullptr; }
}

// ---- Helpers --------------------------------------------------------

constexpr const char* kAlpn = "mc-rats/1";
constexpr size_t      kPeerIdBytes = 20;

std::string random_peer_id() {
    std::random_device rd;
    std::mt19937_64    g(rd());
    static const char hex[] = "0123456789abcdef";
    std::string out(kPeerIdBytes * 2, '0');
    for (size_t i = 0; i < kPeerIdBytes; ++i) {
        uint64_t r = g();
        out[i * 2]     = hex[(r >> 4) & 0xF];
        out[i * 2 + 1] = hex[r & 0xF];
    }
    return out;
}

void write_u32_le(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}
uint32_t read_u32_le(const uint8_t* p) {
    return uint32_t(p[0])
         | (uint32_t(p[1]) << 8)
         | (uint32_t(p[2]) << 16)
         | (uint32_t(p[3]) << 24);
}
void write_u64_le(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = (v >> (i * 8)) & 0xFF;
}
uint64_t read_u64_le(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t(p[i]) << (i * 8);
    return v;
}

// Self-signed cert generation moved to transport/cert_util.{h,cpp} so the
// HTTP/3 server (server.cpp) can share the same path.

} // namespace

// =====================================================================
// Internal types
// =====================================================================

struct McrClient; // fwd

struct McrPeer {
    McrClient*  client = nullptr;  // back-pointer so stream cb can reach state
    std::string peer_id;       // 40 hex chars
    HQUIC       connection;    // owned by Client; we hold a borrowed handle
    HQUIC       control_stream = nullptr;
    bool        validated = false;
    std::vector<uint8_t> handshake_buf; // accumulating the 20-byte peer id
    std::vector<uint8_t> send_keep;     // backing storage for in-flight Send
};

struct McrInboundStream {
    HQUIC                connection;
    HQUIC                stream;
    std::vector<uint8_t> buffer;
};

struct McrClient {
    int      listen_port_request = 0;
    uint16_t actual_listen_port  = 0;
    std::string own_peer_id;

    HQUIC registration   = nullptr;
    HQUIC server_config  = nullptr;
    HQUIC client_config  = nullptr;
    HQUIC listener       = nullptr;
    QUIC_BUFFER alpn{};
    std::vector<uint8_t> alpn_storage;
    std::atomic<bool> started{false};

    int max_peers = 4096;

    // Peer table — keyed by HQUIC connection pointer (cast to uintptr_t)
    std::mutex peers_mu;
    std::map<uintptr_t, std::shared_ptr<McrPeer>> peers; // connection -> peer
    std::unordered_map<std::string, uintptr_t>    by_id; // peer_id -> conn

    // Per-stream buffering (incoming send-once frames)
    std::mutex streams_mu;
    std::unordered_map<uintptr_t, std::vector<uint8_t>> rx_buffers; // stream key

    // Callbacks
    rats_connection_cb conn_cb        = nullptr; void* conn_ud = nullptr;
    rats_disconnect_cb disc_cb        = nullptr; void* disc_ud = nullptr;
    rats_binary_cb     binary_cb      = nullptr; void* bin_ud  = nullptr;

    struct TypedHandler {
        rats_message_cb cb;
        void*           ud;
    };
    std::mutex                                 handlers_mu;
    std::unordered_map<std::string, TypedHandler> handlers;
};

// =====================================================================
// Stream callback (data arrival + close)
// =====================================================================

static void deliver_frame(McrClient* c, McrPeer* peer,
                          const uint8_t* data, size_t size) {
    if (size < 1) return;
    const uint8_t kind = data[0];
    if (kind == 'M') {
        if (size < 9) return;
        const uint32_t type_len = read_u32_le(data + 1);
        const uint32_t body_len = read_u32_le(data + 5);
        if (size < 9 + (size_t)type_len + (size_t)body_len) return;
        std::string type((const char*)data + 9, type_len);
        std::string body((const char*)data + 9 + type_len, body_len);

        rats_message_cb cb = nullptr; void* ud = nullptr;
        {
            std::lock_guard<std::mutex> lk(c->handlers_mu);
            auto it = c->handlers.find(type);
            if (it != c->handlers.end()) { cb = it->second.cb; ud = it->second.ud; }
        }
        if (cb) {
            // Match patched librats lifetime semantics: caller frees both.
            char* peer_cp = strdup(peer->peer_id.c_str());
            char* data_cp = strdup(body.c_str());
            cb(ud, peer_cp, data_cp);
        }
    } else if (kind == 'B') {
        if (size < 9) return;
        const uint64_t body_len = read_u64_le(data + 1);
        if (size < 9 + body_len) return;
        if (c->binary_cb) {
            char* peer_cp = strdup(peer->peer_id.c_str());
            void* bin_cp  = std::malloc(body_len);
            if (bin_cp) std::memcpy(bin_cp, data + 9, body_len);
            c->binary_cb(c->bin_ud, peer_cp, bin_cp, (size_t)body_len);
        }
    }
}

static QUIC_STATUS QUIC_API stream_callback(HQUIC stream, void* ctx,
                                            QUIC_STREAM_EVENT* ev) {
    std::fprintf(stderr, "[mcr-scb] stream=%p type=%d\n", (void*)stream, (int)ev->Type); std::fflush(stderr);
    auto* peer = static_cast<McrPeer*>(ctx);
    if (!peer) return QUIC_STATUS_SUCCESS;
    auto* c    = peer->client;
    if (!c) return QUIC_STATUS_SUCCESS;

    switch (ev->Type) {
        case QUIC_STREAM_EVENT_RECEIVE: {
            const auto& recv = ev->RECEIVE;
            // Accumulate into the stream's rx buffer.
            const uintptr_t key = reinterpret_cast<uintptr_t>(stream);
            std::vector<uint8_t>* buf = nullptr;
            {
                std::lock_guard<std::mutex> lk(c->streams_mu);
                buf = &c->rx_buffers[key];
            }
            for (uint32_t i = 0; i < recv.BufferCount; ++i) {
                const auto& b = recv.Buffers[i];
                buf->insert(buf->end(), b.Buffer, b.Buffer + b.Length);
            }
            break;
        }
        case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {
            // Peer finished sending — process buffered frame.
            const uintptr_t key = reinterpret_cast<uintptr_t>(stream);
            std::vector<uint8_t> data;
            {
                std::lock_guard<std::mutex> lk(c->streams_mu);
                auto it = c->rx_buffers.find(key);
                if (it != c->rx_buffers.end()) { data = std::move(it->second); c->rx_buffers.erase(it); }
            }
            std::fprintf(stderr, "[mcr-hs] PEER_SEND_SHUTDOWN size=%zu validated=%d\n",
                         data.size(), (int)peer->validated);
            std::fflush(stderr);
            // The first ≥20-byte frame from a not-yet-validated peer is the
            // peer-id handshake. We can't compare against a single
            // `control_stream` pointer because each side opens its own
            // outbound stream — the peer's identity arrives on whichever
            // stream the peer opened, not on the one we opened.
            if (!peer->validated && data.size() >= kPeerIdBytes) {
                static const char hex[] = "0123456789abcdef";
                std::string id(kPeerIdBytes * 2, '0');
                for (size_t i = 0; i < kPeerIdBytes; ++i) {
                    id[i * 2]     = hex[(data[i] >> 4) & 0xF];
                    id[i * 2 + 1] = hex[data[i] & 0xF];
                }
                peer->peer_id   = std::move(id);
                peer->validated = true;
                {
                    std::lock_guard<std::mutex> lk(c->peers_mu);
                    c->by_id[peer->peer_id] =
                        reinterpret_cast<uintptr_t>(peer->connection);
                }
                std::fprintf(stderr, "[mcr-hs] VALIDATED %s\n", peer->peer_id.c_str());
                std::fflush(stderr);
                if (c->conn_cb) c->conn_cb(c->conn_ud, strdup(peer->peer_id.c_str()));
            } else {
                deliver_frame(c, peer, data.data(), data.size());
            }
            // Reciprocally close our send side so the stream tears down.
            g_api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
            break;
        }
        case QUIC_STREAM_EVENT_SEND_COMPLETE: {
            // We owned the send buffer; free it now.
            if (ev->SEND_COMPLETE.ClientContext) {
                std::free(ev->SEND_COMPLETE.ClientContext);
            }
            break;
        }
        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
            const uintptr_t key = reinterpret_cast<uintptr_t>(stream);
            {
                std::lock_guard<std::mutex> lk(c->streams_mu);
                c->rx_buffers.erase(key);
            }
            g_api->StreamClose(stream);
            break;
        }
        default: break;
    }
    return QUIC_STATUS_SUCCESS;
}

// =====================================================================
// Connection callback
// =====================================================================

static QUIC_STATUS QUIC_API connection_callback(HQUIC conn, void* ctx,
                                                QUIC_CONNECTION_EVENT* ev) {
    if (ev->Type == 0 || ev->Type == 1 || ev->Type == 2 || ev->Type == 3) {
        std::fprintf(stderr, "[mcr-cb] conn ev=%d conn=%p\n", (int)ev->Type, (void*)conn);
        if (ev->Type == 1) {
            std::fprintf(stderr, "[mcr-cb]   transport shutdown status=0x%x ec=%llu\n",
                         (unsigned)ev->SHUTDOWN_INITIATED_BY_TRANSPORT.Status,
                         (unsigned long long)ev->SHUTDOWN_INITIATED_BY_TRANSPORT.ErrorCode);
        }
        std::fflush(stderr);
    }
    auto* c    = static_cast<McrClient*>(ctx);
    auto  key  = reinterpret_cast<uintptr_t>(conn);

    switch (ev->Type) {
        case QUIC_CONNECTION_EVENT_CONNECTED: {
            std::fprintf(stderr, "[mcr-cb] CONNECTED conn=%p\n", (void*)conn); std::fflush(stderr);
            // Open control stream and send our peer id.
            std::shared_ptr<McrPeer> peer;
            {
                std::lock_guard<std::mutex> lk(c->peers_mu);
                auto it = c->peers.find(key);
                if (it == c->peers.end()) {
                    peer = std::make_shared<McrPeer>();
                    peer->client     = c;
                    peer->connection = conn;
                    c->peers[key] = peer;
                } else {
                    peer = it->second;
                    peer->client = c;
                }
            }
            HQUIC stream = nullptr;
            auto so_rc = g_api->StreamOpen(conn, QUIC_STREAM_OPEN_FLAG_NONE,
                                            stream_callback, peer.get(), &stream);
            std::fprintf(stderr, "[mcr-cb] StreamOpen rc=0x%x\n", (unsigned)so_rc); std::fflush(stderr);
            if (QUIC_SUCCEEDED(so_rc)) {
                peer->control_stream = stream;
                auto ss_rc = g_api->StreamStart(stream, QUIC_STREAM_START_FLAG_NONE);
                std::fprintf(stderr, "[mcr-cb] StreamStart rc=0x%x\n", (unsigned)ss_rc); std::fflush(stderr);
                // Hex-decode our peer id into peer->send_keep so msquic has a
                // stable backing buffer until SEND_COMPLETE; we don't pass a
                // ClientContext so there is nothing to double-free either.
                peer->send_keep.resize(kPeerIdBytes);
                for (size_t i = 0; i < kPeerIdBytes; ++i) {
                    auto hx = [](char c){ return c <= '9' ? c - '0' : 10 + (c | 32) - 'a'; };
                    peer->send_keep[i] =
                        (hx(c->own_peer_id[i*2]) << 4) | hx(c->own_peer_id[i*2+1]);
                }
                QUIC_BUFFER qb{ (uint32_t)kPeerIdBytes, peer->send_keep.data() };
                auto sd_rc = g_api->StreamSend(stream, &qb, 1, QUIC_SEND_FLAG_FIN, nullptr);
                std::fprintf(stderr, "[mcr-cb] StreamSend rc=0x%x\n", (unsigned)sd_rc); std::fflush(stderr);
            }
            break;
        }
        case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
            std::fprintf(stderr, "[mcr-cb] PEER_STREAM_STARTED conn=%p\n", (void*)conn); std::fflush(stderr);
            std::shared_ptr<McrPeer> peer;
            {
                std::lock_guard<std::mutex> lk(c->peers_mu);
                auto it = c->peers.find(key);
                if (it != c->peers.end()) peer = it->second;
            }
            if (!peer) {
                peer = std::make_shared<McrPeer>();
                peer->client = c;
                peer->connection = conn;
                std::lock_guard<std::mutex> lk(c->peers_mu);
                c->peers[key] = peer;
            }
            HQUIC stream = ev->PEER_STREAM_STARTED.Stream;
            g_api->SetCallbackHandler(stream, (void*)stream_callback, peer.get());
            // First stream from the peer is the control stream by convention.
            if (peer->control_stream == nullptr) peer->control_stream = stream;
            break;
        }
        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: {
            std::shared_ptr<McrPeer> peer;
            {
                std::lock_guard<std::mutex> lk(c->peers_mu);
                auto it = c->peers.find(key);
                if (it != c->peers.end()) { peer = it->second; c->peers.erase(it); }
                if (peer && !peer->peer_id.empty()) c->by_id.erase(peer->peer_id);
            }
            if (peer && peer->validated && c->disc_cb) {
                c->disc_cb(c->disc_ud, strdup(peer->peer_id.c_str()));
            }
            g_api->ConnectionClose(conn);
            break;
        }
        default: break;
    }
    return QUIC_STATUS_SUCCESS;
}

// =====================================================================
// Listener callback
// =====================================================================

static QUIC_STATUS QUIC_API listener_callback(HQUIC /*listener*/, void* ctx,
                                              QUIC_LISTENER_EVENT* ev) {
    auto* c = static_cast<McrClient*>(ctx);
    if (ev->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION) return QUIC_STATUS_SUCCESS;
    HQUIC conn = ev->NEW_CONNECTION.Connection;
    g_api->SetCallbackHandler(conn, (void*)connection_callback, c);
    g_api->SetContext(conn, c);
    return g_api->ConnectionSetConfiguration(conn, c->server_config);
}

// =====================================================================
// Public API
// =====================================================================

extern "C" {

MCR_API void rats_string_free(const char* str) { if (str) std::free((void*)str); }
MCR_API const char* rats_get_version_string(void) { return "mc-rats-quic-0.1"; }
MCR_API uint32_t    rats_get_abi(void)            { return 1; }

MCR_API rats_client_t rats_create(int listen_port) {
    if (!ensure_api()) return nullptr;
    auto* c = new McrClient();
    c->listen_port_request = listen_port;
    c->own_peer_id = random_peer_id();
    c->alpn_storage.assign((const uint8_t*)kAlpn, (const uint8_t*)kAlpn + std::strlen(kAlpn));
    c->alpn = { (uint32_t)c->alpn_storage.size(), c->alpn_storage.data() };

    QUIC_REGISTRATION_CONFIG reg_cfg{ "mc-rats-quic",
                                       QUIC_EXECUTION_PROFILE_LOW_LATENCY };
    if (QUIC_FAILED(g_api->RegistrationOpen(&reg_cfg, &c->registration))) {
        delete c; release_api(); return nullptr;
    }
    return c;
}

MCR_API void rats_destroy(rats_client_t handle) {
    if (!handle) return;
    rats_stop(handle);
    auto* c = static_cast<McrClient*>(handle);
    if (c->registration) g_api->RegistrationClose(c->registration);
    delete c;
    release_api();
}

MCR_API int rats_start(rats_client_t handle) {
    auto* c = static_cast<McrClient*>(handle);
    if (!c || !c->registration) return 1;
    if (c->started.exchange(true)) return 0;

    // ---- Server configuration with self-signed cert ----------------
    QUIC_SETTINGS settings{};
    settings.IsSet.PeerBidiStreamCount = TRUE;
    settings.PeerBidiStreamCount       = 256;
    settings.IsSet.IdleTimeoutMs       = TRUE;
    settings.IdleTimeoutMs             = 60'000;

    QUIC_STATUS rc;
    rc = g_api->ConfigurationOpen(c->registration,
                                   &c->alpn, 1, &settings, sizeof(settings),
                                   nullptr, &c->server_config);
    if (QUIC_FAILED(rc)) {
        std::cerr << "[mcr] ConfigurationOpen(server) failed: 0x"
                  << std::hex << rc << std::dec << "\n";
        return 2;
    }
    mc::transport::CertFiles cf = mc::transport::make_self_signed_files();
    if (!cf.ok) {
        std::cerr << "[mcr] make_self_signed_files failed\n";
        return 3;
    }
    QUIC_CERTIFICATE_FILE cert_file{};
    cert_file.CertificateFile = cf.cert_path.c_str();
    cert_file.PrivateKeyFile  = cf.key_path.c_str();

    QUIC_CREDENTIAL_CONFIG cred{};
    cred.Type            = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    cred.CertificateFile = &cert_file;

    rc = g_api->ConfigurationLoadCredential(c->server_config, &cred);
    // Files are no longer needed once msquic has ingested them.
    std::remove(cf.cert_path.c_str());
    std::remove(cf.key_path.c_str());
    if (QUIC_FAILED(rc)) {
        std::cerr << "[mcr] ConfigurationLoadCredential(server) failed: 0x"
                  << std::hex << rc << std::dec << "\n";
        return 4;
    }

    // ---- Client configuration (no validation) ----------------------
    rc = g_api->ConfigurationOpen(c->registration,
                                   &c->alpn, 1, &settings, sizeof(settings),
                                   nullptr, &c->client_config);
    if (QUIC_FAILED(rc)) {
        std::cerr << "[mcr] ConfigurationOpen(client) failed: 0x"
                  << std::hex << rc << std::dec << "\n";
        return 5;
    }
    QUIC_CREDENTIAL_CONFIG ccred{};
    ccred.Type  = QUIC_CREDENTIAL_TYPE_NONE;
    ccred.Flags = QUIC_CREDENTIAL_FLAG_CLIENT
                | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
    rc = g_api->ConfigurationLoadCredential(c->client_config, &ccred);
    if (QUIC_FAILED(rc)) {
        std::cerr << "[mcr] ConfigurationLoadCredential(client) failed: 0x"
                  << std::hex << rc << std::dec << "\n";
        return 6;
    }

    // ---- Listener --------------------------------------------------
    rc = g_api->ListenerOpen(c->registration, listener_callback, c, &c->listener);
    if (QUIC_FAILED(rc)) {
        std::cerr << "[mcr] ListenerOpen failed: 0x"
                  << std::hex << rc << std::dec << "\n";
        return 7;
    }
    QUIC_ADDR addr{};
    QuicAddrSetFamily(&addr, QUIC_ADDRESS_FAMILY_UNSPEC);
    QuicAddrSetPort(&addr, (uint16_t)c->listen_port_request);

    rc = g_api->ListenerStart(c->listener, &c->alpn, 1, &addr);
    if (QUIC_FAILED(rc)) {
        std::cerr << "[mcr] ListenerStart(port=" << c->listen_port_request
                  << ") failed: 0x" << std::hex << rc << std::dec << "\n";
        return 8;
    }
    c->actual_listen_port = (uint16_t)c->listen_port_request;
    std::cout << "[mcr] listening on UDP " << c->actual_listen_port
              << " (own peer id " << c->own_peer_id << ")\n";
    return 0;
}

MCR_API void rats_stop(rats_client_t handle) {
    auto* c = static_cast<McrClient*>(handle);
    if (!c) return;
    if (c->listener)      { g_api->ListenerClose(c->listener);      c->listener = nullptr; }
    if (c->client_config) { g_api->ConfigurationClose(c->client_config); c->client_config = nullptr; }
    if (c->server_config) { g_api->ConfigurationClose(c->server_config); c->server_config = nullptr; }
    c->started = false;
}

MCR_API int rats_connect(rats_client_t handle, const char* host, int port) {
    auto* c = static_cast<McrClient*>(handle);
    if (!c || !c->started || !host) return 1;
    HQUIC conn = nullptr;
    if (QUIC_FAILED(g_api->ConnectionOpen(c->registration, connection_callback,
                                           c, &conn))) {
        return 2;
    }
    g_api->SetContext(conn, c);
    if (QUIC_FAILED(g_api->ConnectionStart(conn, c->client_config,
                                            QUIC_ADDRESS_FAMILY_UNSPEC, host,
                                            (uint16_t)port))) {
        g_api->ConnectionClose(conn);
        return 3;
    }
    return 0;
}

MCR_API int  rats_get_listen_port(rats_client_t h) {
    auto* c = static_cast<McrClient*>(h);
    return c ? c->actual_listen_port : 0;
}

MCR_API int rats_get_peer_count(rats_client_t h) {
    auto* c = static_cast<McrClient*>(h);
    if (!c) return 0;
    std::lock_guard<std::mutex> lk(c->peers_mu);
    int n = 0;
    for (auto& kv : c->peers) if (kv.second->validated) ++n;
    return n;
}

MCR_API char* rats_get_our_peer_id(rats_client_t h) {
    auto* c = static_cast<McrClient*>(h);
    if (!c) return nullptr;
    return strdup(c->own_peer_id.c_str());
}

MCR_API char** rats_get_validated_peer_ids(rats_client_t h, int* count) {
    auto* c = static_cast<McrClient*>(h);
    if (count) *count = 0;
    if (!c) return nullptr;
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lk(c->peers_mu);
        for (auto& kv : c->peers) if (kv.second->validated) ids.push_back(kv.second->peer_id);
    }
    char** out = (char**)std::calloc(ids.size() + 1, sizeof(char*));
    for (size_t i = 0; i < ids.size(); ++i) out[i] = strdup(ids[i].c_str());
    if (count) *count = (int)ids.size();
    return out;
}

MCR_API char** rats_get_peer_ids(rats_client_t h, int* count) {
    return rats_get_validated_peer_ids(h, count);
}

MCR_API rats_error_t rats_disconnect_peer_by_id(rats_client_t /*h*/, const char* /*peer_id*/) {
    return RATS_SUCCESS;
}

MCR_API rats_error_t rats_set_max_peers(rats_client_t h, int n) {
    auto* c = static_cast<McrClient*>(h);
    if (c) c->max_peers = n;
    return RATS_SUCCESS;
}
MCR_API int rats_get_max_peers(rats_client_t h) {
    auto* c = static_cast<McrClient*>(h);
    return c ? c->max_peers : 0;
}
MCR_API int rats_is_peer_limit_reached(rats_client_t h) {
    return rats_get_peer_count(h) >= rats_get_max_peers(h);
}

MCR_API rats_error_t rats_on_message(rats_client_t h, const char* type,
                                       rats_message_cb cb, void* ud) {
    auto* c = static_cast<McrClient*>(h);
    if (!c || !type) return RATS_ERROR_INVALID_PARAMETER;
    std::lock_guard<std::mutex> lk(c->handlers_mu);
    c->handlers[type] = McrClient::TypedHandler{ cb, ud };
    return RATS_SUCCESS;
}

static rats_error_t send_one_shot_frame(McrClient* c, const char* peer_id,
                                         const uint8_t* frame, size_t frame_len) {
    uintptr_t key = 0;
    HQUIC     conn = nullptr;
    {
        std::lock_guard<std::mutex> lk(c->peers_mu);
        auto it = c->by_id.find(peer_id);
        if (it == c->by_id.end()) return RATS_ERROR_PEER_NOT_FOUND;
        key = it->second;
        auto pit = c->peers.find(key);
        if (pit == c->peers.end()) return RATS_ERROR_PEER_NOT_FOUND;
        conn = pit->second->connection;
    }
    HQUIC stream = nullptr;
    if (QUIC_FAILED(g_api->StreamOpen(conn, QUIC_STREAM_OPEN_FLAG_NONE,
                                       stream_callback,
                                       nullptr /* no peer ctx for outbound */,
                                       &stream))) {
        return RATS_ERROR_OPERATION_FAILED;
    }
    g_api->StreamStart(stream, QUIC_STREAM_START_FLAG_NONE);
    // Heap-copy so we can free in SEND_COMPLETE.
    auto* buf = (uint8_t*)std::malloc(frame_len);
    if (!buf) { g_api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0); return RATS_ERROR_MEMORY_ALLOCATION; }
    std::memcpy(buf, frame, frame_len);
    QUIC_BUFFER qb{ (uint32_t)frame_len, buf };
    g_api->StreamSend(stream, &qb, 1, QUIC_SEND_FLAG_FIN, buf);
    return RATS_SUCCESS;
}

MCR_API rats_error_t rats_send_message(rats_client_t h, const char* peer_id,
                                         const char* type, const char* data) {
    auto* c = static_cast<McrClient*>(h);
    if (!c || !peer_id || !type || !data) return RATS_ERROR_INVALID_PARAMETER;
    const uint32_t tl = (uint32_t)std::strlen(type);
    const uint32_t dl = (uint32_t)std::strlen(data);
    std::vector<uint8_t> frame(1 + 4 + 4 + tl + dl);
    frame[0] = 'M';
    write_u32_le(frame.data() + 1, tl);
    write_u32_le(frame.data() + 5, dl);
    std::memcpy(frame.data() + 9, type, tl);
    std::memcpy(frame.data() + 9 + tl, data, dl);
    return send_one_shot_frame(c, peer_id, frame.data(), frame.size());
}

MCR_API rats_error_t rats_broadcast_message(rats_client_t h, const char* type, const char* data) {
    auto* c = static_cast<McrClient*>(h);
    if (!c) return RATS_ERROR_INVALID_HANDLE;
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lk(c->peers_mu);
        for (auto& kv : c->peers) if (kv.second->validated) ids.push_back(kv.second->peer_id);
    }
    for (auto& id : ids) rats_send_message(h, id.c_str(), type, data);
    return RATS_SUCCESS;
}

MCR_API void rats_set_binary_callback(rats_client_t h, rats_binary_cb cb, void* ud) {
    auto* c = static_cast<McrClient*>(h);
    if (!c) return;
    c->binary_cb = cb; c->bin_ud = ud;
}

MCR_API rats_error_t rats_send_binary(rats_client_t h, const char* peer_id,
                                        const void* data, size_t size) {
    auto* c = static_cast<McrClient*>(h);
    if (!c || !peer_id || !data) return RATS_ERROR_INVALID_PARAMETER;
    std::vector<uint8_t> frame(1 + 8 + size);
    frame[0] = 'B';
    write_u64_le(frame.data() + 1, (uint64_t)size);
    std::memcpy(frame.data() + 9, data, size);
    return send_one_shot_frame(c, peer_id, frame.data(), frame.size());
}

MCR_API int rats_broadcast_binary(rats_client_t h, const void* data, size_t size) {
    auto* c = static_cast<McrClient*>(h);
    if (!c) return 0;
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lk(c->peers_mu);
        for (auto& kv : c->peers) if (kv.second->validated) ids.push_back(kv.second->peer_id);
    }
    int n = 0;
    for (auto& id : ids) if (rats_send_binary(h, id.c_str(), data, size) == RATS_SUCCESS) ++n;
    return n;
}

MCR_API void rats_set_connection_callback(rats_client_t h, rats_connection_cb cb, void* ud) {
    auto* c = static_cast<McrClient*>(h);
    if (c) { c->conn_cb = cb; c->conn_ud = ud; }
}
MCR_API void rats_set_disconnect_callback(rats_client_t h, rats_disconnect_cb cb, void* ud) {
    auto* c = static_cast<McrClient*>(h);
    if (c) { c->disc_cb = cb; c->disc_ud = ud; }
}
MCR_API void rats_set_string_callback(rats_client_t /*h*/, rats_string_cb /*cb*/, void* /*ud*/) {}
MCR_API void rats_set_json_callback  (rats_client_t /*h*/, rats_json_cb   /*cb*/, void* /*ud*/) {}

// ---- STUN — Phase 2b stubs that just refuse (we don't need real STUN
// until Phase 2f's ICE work). Returns an empty string so RatsLink keeps
// running; the public_address field stays blank in the route broadcast.
MCR_API void rats_add_stun_server(rats_client_t /*h*/, const char* /*host*/, uint16_t /*port*/) {}
MCR_API char* rats_discover_public_address(rats_client_t /*h*/, const char* /*server*/,
                                             uint16_t /*port*/, int /*timeout_ms*/) {
    char* out = (char*)std::malloc(1); if (out) out[0] = 0; return out;
}

// ---- Topic stubs: just route to typed-message handlers. mini-node uses
// these and falls back to direct typed messages today.
MCR_API rats_error_t rats_subscribe_to_topic(rats_client_t /*h*/, const char* /*topic*/) {
    return RATS_SUCCESS;
}
MCR_API rats_error_t rats_publish_to_topic(rats_client_t h, const char* topic, const char* msg) {
    return rats_broadcast_message(h, topic, msg);
}
MCR_API void rats_set_topic_message_callback(rats_client_t h, const char* topic,
                                              rats_topic_message_cb cb, void* ud) {
    // Wrap as a typed-message handler. We lose the explicit topic arg but
    // tools/mini_node.cpp doesn't use it.
    if (!cb) return;
    struct Bridge { rats_topic_message_cb tcb; void* tud; std::string topic; };
    auto* b = new Bridge{ cb, ud, std::string(topic) };
    rats_on_message(h, topic,
        [](void* ud, const char* peer_id, const char* msg) {
            auto* b = static_cast<Bridge*>(ud);
            b->tcb(b->tud, peer_id, b->topic.c_str(), msg);
        }, b);
}

MCR_API void rats_start_automatic_peer_discovery(rats_client_t /*h*/) {}
MCR_API void rats_stop_automatic_peer_discovery (rats_client_t /*h*/) {}
MCR_API void rats_set_console_logging_enabled   (int /*on*/) {}
MCR_API void rats_set_log_level                 (const char* /*lvl*/) {}

} // extern "C"
