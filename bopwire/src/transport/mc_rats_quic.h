// mc_rats_quic — the librats C API, reimplemented over msquic.
//
// Drop-in replacement for librats_c.h that we link instead of librats. Every
// call site in our codebase (Dart FFI bindings, src/api/rats_api.cpp,
// src/network/rats_link.cpp, tools/mini_node.cpp) continues to work.
//
// Wire-level changes:
//   • Peer connections are QUIC instead of TCP.
//   • Each rats_send_message opens a single bidirectional QUIC stream and
//     writes [type-string \0 body-string \0]. The receiver closes the stream
//     after delivering it to the callback.
//   • rats_send_binary uses a stream with a single byte tag (0x42) followed
//     by [u64 length] + body.
//
// What is intentionally simpler than librats:
//   • No GossipSub mesh (mini-node still does pubsub manually).
//   • No DHT discovery.
//   • No BitTorrent layer.
//   • No persistent peer cache.
//   • STUN/ICE/TURN APIs delegate to librats's existing implementation for
//     now; only the peer-to-peer message transport is replaced.

#pragma once

#include <stddef.h>
#include <stdint.h>

// The same header serves two link models:
//   - Static lib `mc_rats_quic` linked into bopwire-node / mini-node.
//     In that case MC_RATS_SHARED is undefined and MCR_API is empty.
//   - Shared lib `mc_rats.dll` / `libmc_rats.so` loaded by the Dart FFI side.
//     The DLL build defines MC_RATS_SHARED + MCR_BUILDING_DLL; consumers
//     just define MC_RATS_SHARED so the prototypes carry dllimport.
#if defined(MC_RATS_SHARED)
  #if defined(_WIN32)
    #if defined(MCR_BUILDING_DLL)
      #define MCR_API __declspec(dllexport)
    #else
      #define MCR_API __declspec(dllimport)
    #endif
  #else
    #define MCR_API __attribute__((visibility("default")))
  #endif
#else
  #define MCR_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void* rats_client_t;

typedef enum {
    RATS_SUCCESS               =  0,
    RATS_ERROR_INVALID_HANDLE  = -1,
    RATS_ERROR_INVALID_PARAMETER = -2,
    RATS_ERROR_NOT_RUNNING     = -3,
    RATS_ERROR_OPERATION_FAILED = -4,
    RATS_ERROR_PEER_NOT_FOUND  = -5,
    RATS_ERROR_MEMORY_ALLOCATION = -6,
    RATS_ERROR_JSON_PARSE      = -7
} rats_error_t;

// Callback signatures must match librats_c.h exactly.
typedef void (*rats_connection_cb)(void* user_data, const char* peer_id);
typedef void (*rats_disconnect_cb)(void* user_data, const char* peer_id);
typedef void (*rats_message_cb)(void* user_data, const char* peer_id,
                                 const char* message_data);
typedef void (*rats_binary_cb)(void* user_data, const char* peer_id,
                                const void* data, size_t size);
typedef void (*rats_string_cb)(void* user_data, const char* peer_id,
                                const char* message);
typedef void (*rats_json_cb)(void* user_data, const char* peer_id,
                              const char* json_str);
typedef void (*rats_topic_message_cb)(void* user_data, const char* peer_id,
                                       const char* topic, const char* message);

// Memory helpers
MCR_API void rats_string_free(const char* str);

// Version / ABI (stubbed)
MCR_API const char* rats_get_version_string(void);
MCR_API uint32_t    rats_get_abi(void);

// Client lifecycle
MCR_API rats_client_t rats_create(int listen_port);
MCR_API void          rats_destroy(rats_client_t client);
MCR_API int           rats_start(rats_client_t client);
MCR_API void          rats_stop(rats_client_t client);

// Connect to a remote peer (QUIC handshake; blocks until accepted or refused).
MCR_API int  rats_connect(rats_client_t client, const char* host, int port);
MCR_API int  rats_get_listen_port(rats_client_t client);

// Peer info / iteration
MCR_API int   rats_get_peer_count(rats_client_t client);
MCR_API char* rats_get_our_peer_id(rats_client_t client);
MCR_API char**rats_get_validated_peer_ids(rats_client_t client, int* count);
MCR_API char**rats_get_peer_ids(rats_client_t client, int* count);
MCR_API rats_error_t rats_disconnect_peer_by_id(rats_client_t client,
                                                  const char* peer_id);

// Peer configuration
MCR_API rats_error_t rats_set_max_peers(rats_client_t client, int max_peers);
MCR_API int          rats_get_max_peers(rats_client_t client);
MCR_API int          rats_is_peer_limit_reached(rats_client_t client);

// Typed messages — the core RPC channel.
MCR_API rats_error_t rats_on_message(rats_client_t client,
                                       const char* message_type,
                                       rats_message_cb callback,
                                       void* user_data);
MCR_API rats_error_t rats_send_message(rats_client_t client,
                                         const char* peer_id,
                                         const char* message_type,
                                         const char* data);
MCR_API rats_error_t rats_broadcast_message(rats_client_t client,
                                              const char* message_type,
                                              const char* data);

// Binary data — used by stream.open and chunked upload.
MCR_API void rats_set_binary_callback(rats_client_t client,
                                       rats_binary_cb cb, void* user_data);
MCR_API rats_error_t rats_send_binary(rats_client_t client,
                                        const char* peer_id,
                                        const void* data, size_t size);
MCR_API int  rats_broadcast_binary(rats_client_t client,
                                     const void* data, size_t size);

// Connection / disconnection lifecycle callbacks.
MCR_API void rats_set_connection_callback(rats_client_t client,
                                            rats_connection_cb cb, void* ud);
MCR_API void rats_set_disconnect_callback(rats_client_t client,
                                            rats_disconnect_cb cb, void* ud);
MCR_API void rats_set_string_callback(rats_client_t client,
                                        rats_string_cb cb, void* ud);
MCR_API void rats_set_json_callback(rats_client_t client,
                                      rats_json_cb cb, void* ud);

// STUN — delegates to librats's stun_client for now (port 9337 case keeps
// rats_link.cpp working without changes).
MCR_API void  rats_add_stun_server(rats_client_t client,
                                     const char* host, uint16_t port);
MCR_API char* rats_discover_public_address(rats_client_t client,
                                             const char* stun_server,
                                             uint16_t port, int timeout_ms);

// Topic / GossipSub stubs — typed message handlers stand in for topics so
// the mini-node code continues to compile. Real pubsub semantics aren't
// implemented in Phase 2b; tools/mini_node.cpp falls back to direct typed
// messages, which we already proved works.
MCR_API rats_error_t rats_subscribe_to_topic(rats_client_t client,
                                               const char* topic);
MCR_API rats_error_t rats_publish_to_topic(rats_client_t client,
                                             const char* topic,
                                             const char* message);
MCR_API void rats_set_topic_message_callback(rats_client_t client,
                                              const char* topic,
                                              rats_topic_message_cb cb,
                                              void* ud);

// Misc helpers / logging — stubbed to no-op.
MCR_API void rats_start_automatic_peer_discovery(rats_client_t client);
MCR_API void rats_stop_automatic_peer_discovery(rats_client_t client);
MCR_API void rats_set_console_logging_enabled(int enabled);
MCR_API void rats_set_log_level(const char* level_str);

#ifdef __cplusplus
} // extern "C"
#endif
