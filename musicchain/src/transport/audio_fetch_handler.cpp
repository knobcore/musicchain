// audio_fetch_handler.cpp — gateway-level `audio.fetch` verb handler.
//
// See audio_fetch_handler.h for the full contract. This file implements
// the worker that drives one audio.fetch exchange end-to-end:
//
//   open WS frame  →  stream.open to peer  →  await reply  →
//   "ok" frame to browser  →  N binary chunks  →  "complete" frame.
//
// All socket IO is owned by the gateway. The handler reaches the
// gateway through two std::function callbacks (send_text / send_bin)
// passed at construction. The handler reaches librats through
// rats_send_message; the inbound side of librats (binary chunks +
// musicchain.reply envelopes) reaches the handler through two static
// dispatch hooks (dispatch_audio_fetch_reply / dispatch_audio_fetch_chunk)
// the mini-node's existing callbacks invoke.
//
// One worker thread per WS connection, mirroring WsAudioBridge's
// "one accept thread + one worker per upgraded conn" model. The worker
// owns the AudioFetchHandle::Impl which holds the per-stream state.
// AudioFetchHandle::cancel() flips an abort flag and notifies the
// worker; the destructor joins so no callback ever fires into freed
// memory.
//

#include "audio_fetch_handler.h"

// Both headers come from deps/librats/src/, re-exported by the
// `rats` (mc_rats_quic alias) target via its BUILD_INTERFACE — works
// whether this file is compiled into the musicchain library or
// directly into the musicchain-mini-node target.
#include "librats_c.h"
#include "json.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mc::transport {

namespace {

// Mini-node side type tags for `musicchain.request` / `musicchain.reply`.
// Mirrors the kRequestType / kReplyType constants in mini_node.cpp.
constexpr const char* kRequestType = "musicchain.request";

// How long the handler waits for the peer's `stream.open` reply before
// giving up. Matches WsAudioBridge.
constexpr int kOpenTimeoutMs = 8000;

// How long the handler waits for `swarm.members` from the home node
// when the browser omitted peer_id. Home-node is local-ish to the
// mini-node; 4 s is plenty.
constexpr int kSwarmLookupTimeoutMs = 4000;

// Stall guard while streaming. If no chunk arrives for this long the
// handler bails (and tells the peer to stop pushing).
constexpr uint64_t kChunkStallMs = 60'000;

// ---- per-stream state -------------------------------------------------

struct StreamState {
    // Producer (librats binary callback thread / on_relay_reply thread) →
    // consumer (this handler's worker thread).
    std::mutex              mu;
    std::condition_variable cv;

    // The peer's stream.open reply lands here.
    bool                    have_reply = false;
    std::string             reply_json;

    // The home-node's swarm.members reply lands here when peer_id was
    // omitted by the browser. Same channel as reply_json but tracked
    // separately so a slow peer doesn't masquerade as the home node.
    bool                    have_swarm = false;
    std::string             swarm_json;

    // Inbound audio chunks waiting to be flushed via send_bin. Each
    // entry is the raw audio payload (header stripped).
    struct Chunk { std::vector<uint8_t> bytes; bool eof; };
    std::vector<Chunk>      chunks;

    // Filled in once the peer answers stream.open OK.
    uint32_t                stream_id      = 0;
    uint64_t                total_bytes    = 0;
    uint64_t                bytes_seen     = 0;
    bool                    have_stream_id = false;

    // Cancel flag — set by AudioFetchHandle::cancel() OR by the worker
    // itself when it hits a terminal error.
    std::atomic<bool>       abort{false};
};

// ---- registries (global, mutex-guarded) -------------------------------
//
// Same shape as WsAudioBridge's static registries. dispatch_*_reply and
// dispatch_*_chunk are global functions; they look up the StreamState
// via these maps. AudioFetchHandle is responsible for inserting +
// removing its own entries so the dispatch hooks never see a dangling
// pointer.

std::mutex                                       g_pending_mu;
std::unordered_map<std::string, StreamState*>    g_pending_opens;   // req_id  → state
std::unordered_map<std::string, StreamState*>    g_pending_swarms;  // req_id  → state

std::mutex                                       g_stream_mu;
std::unordered_map<uint32_t,    StreamState*>    g_streams;         // stream_id → state

void register_pending_open(const std::string& req_id, StreamState* s) {
    std::lock_guard<std::mutex> lk(g_pending_mu);
    g_pending_opens[req_id] = s;
}
void clear_pending_open(const std::string& req_id) {
    std::lock_guard<std::mutex> lk(g_pending_mu);
    g_pending_opens.erase(req_id);
}
void register_pending_swarm(const std::string& req_id, StreamState* s) {
    std::lock_guard<std::mutex> lk(g_pending_mu);
    g_pending_swarms[req_id] = s;
}
void clear_pending_swarm(const std::string& req_id) {
    std::lock_guard<std::mutex> lk(g_pending_mu);
    g_pending_swarms.erase(req_id);
}
void register_stream(uint32_t sid, StreamState* s) {
    std::lock_guard<std::mutex> lk(g_stream_mu);
    g_streams[sid] = s;
}
void clear_stream(uint32_t sid) {
    std::lock_guard<std::mutex> lk(g_stream_mu);
    g_streams.erase(sid);
}

// ---- utilities --------------------------------------------------------

bool is_hex_string(const std::string& s, size_t expected_len) {
    if (s.size() != expected_len) return false;
    for (char c : s) {
        const bool ok = (c >= '0' && c <= '9')
                     || (c >= 'a' && c <= 'f')
                     || (c >= 'A' && c <= 'F');
        if (!ok) return false;
    }
    return true;
}

uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string new_req_id() {
    // Distinguishable from WsAudioBridge's "wsaud-" prefix so logs make
    // it clear which surface minted the request.
    static std::atomic<uint64_t> counter{1};
    char buf[40];
    std::snprintf(buf, sizeof(buf), "afhdl-%010llx",
                  (unsigned long long)counter.fetch_add(1));
    return buf;
}

// Pluck the first 40-hex peer id out of a `stream.open` reply body.
// The home node emits `{peers: [{peer_id, bitrate, audio_format,
// content_hash}, ...], source: "swarm"|"no_swarm"}` per rats_api.cpp's
// stream.open handler. We tolerate the older `members` spelling and
// raw-string entries too, just in case a different node implementation
// ever shows up on the same mesh.
std::string first_swarm_peer(const nlohmann::json& body) {
    if (!body.is_object()) return {};
    const char* keys[] = { "peers", "members" };
    for (const char* k : keys) {
        if (!body.contains(k)) continue;
        const auto& arr = body[k];
        if (!arr.is_array()) continue;
        for (const auto& item : arr) {
            std::string candidate;
            if (item.is_string()) candidate = item.get<std::string>();
            else if (item.is_object() && item.contains("peer_id")
                     && item["peer_id"].is_string()) {
                candidate = item["peer_id"].get<std::string>();
            }
            if (is_hex_string(candidate, 40)) return candidate;
        }
    }
    return {};
}

// Send `stream.close { stream_id }` to the peer so it can stop pushing
// chunks when the WS connection drops or the gateway shuts down.
// Best-effort; the peer also has its own per-stream timeout.
void send_stream_close(rats_client_t rats,
                       const std::string& peer_id,
                       uint32_t stream_id) {
    if (!rats || peer_id.empty()) return;
    nlohmann::json env = {
        {"req_id", new_req_id()},
        {"type",   "stream.close"},
        {"body",   {{"stream_id", stream_id}}},
    };
    rats_send_message(rats, peer_id.c_str(),
                      kRequestType, env.dump().c_str());
}

} // namespace

// ---- AudioFetchHandle::Impl ------------------------------------------
//
// Per-connection scratch space. The worker thread owns the state; the
// public AudioFetchHandle is a thin wrapper that calls cancel() and
// joins the worker on destruction.

struct AudioFetchHandle::Impl {
    // Caller-visible inputs (captured by the worker).
    rats_client_t  rats        = nullptr;
    std::string    browser_req_id;
    std::string    content_hash;
    std::string    peer_id;          // empty == ask home node first
    SendTextFn     send_text;
    SendBinaryFn   send_bin;

    // The req_ids this handle minted, so cancel() can drop them from
    // the global pending-open / pending-swarm registries.
    std::string    open_req_id;
    std::string    swarm_req_id;

    // Per-stream state shared with the dispatch hooks. Heap-allocated
    // so its address can be parked in the global registries without
    // moving when the Impl is moved (it isn't, but the dispatch hooks
    // are a public API we want to keep cheap).
    std::unique_ptr<StreamState> state = std::make_unique<StreamState>();

    // The worker thread.
    std::thread    worker;

    // Set to true once the worker function returns.
    std::atomic<bool> done{false};

    void send_error_envelope(const std::string& status,
                             const std::string& err);
    void run_worker();
    void unregister_all();
};

void AudioFetchHandle::Impl::send_error_envelope(const std::string& status,
                                                 const std::string& err) {
    if (!send_text) return;
    nlohmann::json env = {
        {"req_id", browser_req_id},
        {"status", status},
        {"error",  err},
    };
    try { send_text(env.dump()); } catch (...) {}
}

void AudioFetchHandle::Impl::unregister_all() {
    if (!open_req_id.empty())  clear_pending_open(open_req_id);
    if (!swarm_req_id.empty()) clear_pending_swarm(swarm_req_id);
    if (state && state->have_stream_id) clear_stream(state->stream_id);
}

void AudioFetchHandle::Impl::run_worker() {
    // Mark done on every exit path so finished() reports the truth.
    struct DoneGuard {
        std::atomic<bool>& flag;
        ~DoneGuard() { flag.store(true); }
    } done_guard{done};

    if (!rats) {
        send_error_envelope("no_rats",
            "mini-node has no active librats client");
        return;
    }

    // 1. Validate inputs (cheap; gateway may have done it but the
    //    handler is the source of truth).
    if (!is_hex_string(content_hash, 64)) {
        send_error_envelope("bad_request",
            "content_hash must be 64-hex");
        return;
    }
    if (!peer_id.empty() && !is_hex_string(peer_id, 40)) {
        send_error_envelope("bad_request",
            "peer_id must be 40-hex when present");
        return;
    }

    // 2. If peer_id was omitted, ask the home node for swarm members
    //    and pick the first one whose id is well-shaped. The home node
    //    is on the librats mesh too; we ask it via a regular
    //    `musicchain.request` of type swarm.members.
    if (peer_id.empty()) {
        // Find the home node. The mini-node maintains a list of peers
        // through the regular librats callbacks; for the audio.fetch
        // use case we accept any connected peer as the swarm directory
        // — the home node will answer, others will reply with not_found
        // or be silent and time out. Practical for v1; the gateway can
        // tighten this once it tracks the home-node peer_id explicitly.
        int peer_count = 0;
        char** peer_ids = rats_get_validated_peer_ids(rats, &peer_count);
        if (!peer_ids || peer_count == 0) {
            if (peer_ids) std::free(peer_ids);
            send_error_envelope("no_peer",
                "peer_id omitted and no connected peers to ask "
                "for swarm members");
            return;
        }
        // First peer is fine — the mini-node's connection list is
        // typically dominated by the home node anyway. The librats
        // convention (see broadcast_swarm_peer_* in mini_node.cpp) is
        // to rats_string_free each entry then std::free the array.
        std::string directory = peer_ids[0] ? std::string(peer_ids[0])
                                            : std::string();
        for (int i = 0; i < peer_count; ++i) {
            if (peer_ids[i]) rats_string_free(peer_ids[i]);
        }
        std::free(peer_ids);
        if (!is_hex_string(directory, 40)) {
            send_error_envelope("no_peer",
                "directory peer id is malformed");
            return;
        }

        swarm_req_id = new_req_id();
        register_pending_swarm(swarm_req_id, state.get());

        // Use the canonical `stream.open` verb — the home node's
        // rats_api answers that with a `{peers: [{peer_id, bitrate,
        // audio_format, ...}], source: "swarm"|"no_swarm"}` body. The
        // earlier code targeted a `swarm.members` verb that never
        // existed on the home node, so this lookup always failed with
        // unknown_type and the audio.fetch returned "no usable
        // peer_id" to the browser.
        nlohmann::json swarm_req = {
            {"req_id", swarm_req_id},
            {"type",   "stream.open"},
            {"body",   {{"content_hash", content_hash}}},
        };
        auto rc = rats_send_message(rats, directory.c_str(),
                                    kRequestType, swarm_req.dump().c_str());
        if (rc != RATS_SUCCESS) {
            clear_pending_swarm(swarm_req_id);
            swarm_req_id.clear();
            send_error_envelope("peer_send_failed",
                "stream.open rats_send_message rc="
                + std::to_string(static_cast<int>(rc)));
            return;
        }

        const uint64_t deadline = now_ms() + kSwarmLookupTimeoutMs;
        bool got_swarm = false;
        while (!state->abort.load(std::memory_order_relaxed)) {
            std::unique_lock<std::mutex> lk(state->mu);
            if (state->cv.wait_for(
                    lk, std::chrono::milliseconds(100),
                    [&]{ return state->have_swarm
                              || state->abort.load(); })) {
                got_swarm = state->have_swarm;
                break;
            }
            if (now_ms() > deadline) break;
        }
        clear_pending_swarm(swarm_req_id);
        swarm_req_id.clear();

        if (state->abort.load()) return;
        if (!got_swarm) {
            send_error_envelope("swarm_lookup_timeout",
                "home node did not answer stream.open within "
                + std::to_string(kSwarmLookupTimeoutMs) + "ms");
            return;
        }

        // Parse and pick the first reachable peer.
        nlohmann::json swarm_reply;
        try {
            swarm_reply = nlohmann::json::parse(state->swarm_json);
        } catch (...) {
            send_error_envelope("peer_error",
                "stream.open reply was not JSON");
            return;
        }
        const auto& body = swarm_reply.value(
            "body", nlohmann::json::object());
        std::string picked = first_swarm_peer(body);
        if (picked.empty()) {
            send_error_envelope("no_peer",
                "stream.open reply had no usable peer_id");
            return;
        }
        peer_id = std::move(picked);
    }

    // 3. Send stream.open to the chosen peer and register the req_id so
    //    the reply lands in state->reply_json via dispatch_audio_fetch_reply.
    open_req_id = new_req_id();
    register_pending_open(open_req_id, state.get());
    {
        nlohmann::json env = {
            {"req_id", open_req_id},
            {"type",   "stream.open"},
            {"body",   {{"content_hash", content_hash}}},
        };
        auto rc = rats_send_message(rats, peer_id.c_str(),
                                    kRequestType, env.dump().c_str());
        if (rc != RATS_SUCCESS) {
            clear_pending_open(open_req_id);
            open_req_id.clear();
            send_error_envelope("peer_send_failed",
                "stream.open rats_send_message rc="
                + std::to_string(static_cast<int>(rc)));
            return;
        }
    }

    // 4. Wait for the peer's reply with a deadline.
    const uint64_t open_deadline = now_ms() + kOpenTimeoutMs;
    bool got_reply = false;
    while (!state->abort.load(std::memory_order_relaxed)) {
        std::unique_lock<std::mutex> lk(state->mu);
        if (state->cv.wait_for(
                lk, std::chrono::milliseconds(100),
                [&]{ return state->have_reply
                          || state->abort.load(); })) {
            got_reply = state->have_reply;
            break;
        }
        if (now_ms() > open_deadline) break;
    }
    clear_pending_open(open_req_id);
    open_req_id.clear();

    if (state->abort.load()) return;
    if (!got_reply) {
        send_error_envelope("open_timeout",
            "peer did not answer stream.open within "
            + std::to_string(kOpenTimeoutMs) + "ms");
        return;
    }

    // Parse the peer's reply.
    nlohmann::json reply;
    try {
        reply = nlohmann::json::parse(state->reply_json);
    } catch (...) {
        send_error_envelope("peer_error",
            "stream.open reply was not JSON");
        return;
    }

    const std::string peer_status = reply.value("status", std::string("ok"));
    const auto& peer_body         = reply.value(
        "body", nlohmann::json::object());
    if (peer_status != "ok"
        || peer_body.value("matched", true) == false) {
        const std::string status =
            peer_body.value("matched", true) == false
                ? std::string("not_matched") : peer_status;
        send_error_envelope(status,
            reply.value("error",
                peer_body.value("error",
                    std::string("peer rejected stream.open"))));
        return;
    }
    if (!peer_body.contains("stream_id")
        || !peer_body.contains("total_bytes")) {
        send_error_envelope("peer_error",
            "stream.open reply missing stream_id/total_bytes");
        return;
    }

    state->stream_id      = peer_body["stream_id"].get<uint32_t>();
    state->total_bytes    = peer_body["total_bytes"].get<uint64_t>();
    state->have_stream_id = true;
    register_stream(state->stream_id, state.get());

    // 5. Send the "ok" envelope so the browser UI can show progress
    //    against total_bytes.
    {
        nlohmann::json env = {
            {"req_id", browser_req_id},
            {"status", "ok"},
            {"body", {
                {"stream_id",   state->stream_id},
                {"total_bytes", state->total_bytes},
            }},
        };
        try { send_text(env.dump()); } catch (...) {}
    }

    // 6. Pump audio chunks until either we've delivered total_bytes,
    //    the WS connection cancels, or stall guard fires.
    uint64_t last_chunk_at = now_ms();
    bool     eof_seen      = false;

    while (!state->abort.load(std::memory_order_relaxed)) {
        std::vector<StreamState::Chunk> drained;
        {
            std::unique_lock<std::mutex> lk(state->mu);
            state->cv.wait_for(lk, std::chrono::milliseconds(50),
                               [&]{ return !state->chunks.empty()
                                        || state->abort.load(); });
            if (!state->chunks.empty()) {
                drained.swap(state->chunks);
                last_chunk_at = now_ms();
            }
        }
        for (auto& c : drained) {
            state->bytes_seen += c.bytes.size();
            if (!c.bytes.empty() && send_bin) {
                try { send_bin(c.bytes.data(), c.bytes.size()); }
                catch (...) {
                    // The gateway threw; treat as connection gone.
                    state->abort.store(true);
                    break;
                }
            }
            if (c.eof) eof_seen = true;
        }
        if (state->abort.load()) break;

        if (eof_seen) break;
        if (state->bytes_seen >= state->total_bytes
            && state->total_bytes > 0) {
            // Some peer implementations skip the eof flag on the last
            // chunk; treat byte parity as done.
            break;
        }
        if (now_ms() - last_chunk_at > kChunkStallMs) {
            std::cerr << "[audio-fetch] stream " << state->stream_id
                      << " stalled (no chunks for " << kChunkStallMs
                      << " ms) — dropping\n";
            state->abort.store(true);
            break;
        }
    }

    // 7. Tear-down. Unregister the stream so no further chunks land on
    //    our state pointer, then send the final envelope.
    if (state->have_stream_id) clear_stream(state->stream_id);

    if (state->abort.load()) {
        // Cancelled / stalled. Tell the peer to stop pushing chunks.
        if (state->have_stream_id) {
            send_stream_close(rats, peer_id, state->stream_id);
        }
        return;
    }

    nlohmann::json env = {
        {"req_id", browser_req_id},
        {"status", "complete"},
        {"body",   {{"sent", state->bytes_seen}}},
    };
    try { send_text(env.dump()); } catch (...) {}
}

// ---- AudioFetchHandle public surface ---------------------------------

AudioFetchHandle::AudioFetchHandle()
    : impl_(std::make_unique<Impl>()) {}

AudioFetchHandle::~AudioFetchHandle() {
    cancel();
    if (impl_ && impl_->worker.joinable()) {
        impl_->worker.join();
    }
}

void AudioFetchHandle::cancel() {
    if (!impl_) return;
    // Flip abort first so the worker observes it on the next loop tick
    // BEFORE we drop registry entries the dispatch hooks might race on.
    if (impl_->state) {
        impl_->state->abort.store(true);
        std::lock_guard<std::mutex> lk(impl_->state->mu);
        impl_->state->cv.notify_all();
    }
    impl_->unregister_all();
}

bool AudioFetchHandle::finished() const {
    return impl_ ? impl_->done.load(std::memory_order_relaxed) : true;
}

// ---- Factory ---------------------------------------------------------

std::unique_ptr<AudioFetchHandle> start_audio_fetch(
        rats_client_t           rats,
        const std::string&      req_id,
        const std::string&      content_hash,
        const std::string&      peer_id,
        SendTextFn              send_text,
        SendBinaryFn            send_bin) {
    auto handle = std::unique_ptr<AudioFetchHandle>(new AudioFetchHandle());
    auto& impl = *handle->impl_;
    impl.rats           = rats;
    impl.browser_req_id = req_id;
    impl.content_hash   = content_hash;
    impl.peer_id        = peer_id;
    impl.send_text      = std::move(send_text);
    impl.send_bin       = std::move(send_bin);

    // Capture a raw Impl* — the handle owns the Impl and joins the
    // worker in its destructor, so the pointer is valid for the
    // worker's full lifetime.
    AudioFetchHandle::Impl* raw = &impl;
    impl.worker = std::thread([raw]() {
        try { raw->run_worker(); }
        catch (const std::exception& e) {
            std::cerr << "[audio-fetch] worker threw: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "[audio-fetch] worker threw (non-std)\n";
        }
    });
    return handle;
}

// ---- Static dispatch hooks (called from the mini-node) ---------------

bool dispatch_audio_fetch_reply(const std::string& req_id,
                                const std::string& envelope_json) {
    if (req_id.empty()) return false;

    // Try the stream.open pending map first; on a hit, fill reply_json
    // and notify. If it isn't a stream.open req_id, try the
    // swarm.members map.
    StreamState* hit_open  = nullptr;
    StreamState* hit_swarm = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        if (auto it = g_pending_opens.find(req_id);
            it != g_pending_opens.end()) {
            hit_open = it->second;
            g_pending_opens.erase(it);
        } else if (auto it2 = g_pending_swarms.find(req_id);
                   it2 != g_pending_swarms.end()) {
            hit_swarm = it2->second;
            g_pending_swarms.erase(it2);
        }
    }
    if (hit_open) {
        {
            std::lock_guard<std::mutex> lk(hit_open->mu);
            hit_open->have_reply = true;
            hit_open->reply_json = envelope_json;
        }
        hit_open->cv.notify_all();
        return true;
    }
    if (hit_swarm) {
        {
            std::lock_guard<std::mutex> lk(hit_swarm->mu);
            hit_swarm->have_swarm = true;
            hit_swarm->swarm_json = envelope_json;
        }
        hit_swarm->cv.notify_all();
        return true;
    }
    return false;
}

bool dispatch_audio_fetch_chunk(const uint8_t* data, size_t size) {
    if (!data || size < 9) return false;
    // bytes 0..3 = stream_id LE, 4..7 = seq LE, 8 = eof, 9..N = payload.
    // Matches _AudioReceiver in
    // musicchain_player/lib/src/services/rats_client.dart.
    const uint32_t sid = static_cast<uint32_t>(data[0])
                       | (static_cast<uint32_t>(data[1]) << 8)
                       | (static_cast<uint32_t>(data[2]) << 16)
                       | (static_cast<uint32_t>(data[3]) << 24);
    const bool eof = data[8] != 0;
    StreamState* s = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_stream_mu);
        auto it = g_streams.find(sid);
        if (it == g_streams.end()) return false;
        s = it->second;
    }
    if (!s) return true;
    {
        std::lock_guard<std::mutex> lk(s->mu);
        s->chunks.push_back(StreamState::Chunk{});
        auto& back = s->chunks.back();
        back.bytes.assign(data + 9, data + size);
        back.eof = eof;
    }
    s->cv.notify_all();
    return true;
}

} // namespace mc::transport
