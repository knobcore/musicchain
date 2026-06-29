// Dart FFI bindings for the librats C API (subset).
//
// Only the verbs the player actually needs are declared here; the full API
// surface in librats_c.h is much larger. Keep this file in sync with that
// header when adding new calls.

import 'dart:ffi';
import 'package:ffi/ffi.dart';

typedef RatsClient = Pointer<Void>;

// -- Native function signatures ----------------------------------------

typedef _NativeCreate     = RatsClient Function(Int32);
typedef _DartCreate       = RatsClient Function(int);

// wallet-as-id: create a client with a forced 40-hex peer id (the wallet
// address). Optional symbol — older shipped libs may not export it.
typedef _NativeCreateWithId = RatsClient Function(Int32, Pointer<Utf8>);
typedef _DartCreateWithId   = RatsClient Function(int, Pointer<Utf8>);

typedef _NativeVoidPtr    = Void Function(RatsClient);
typedef _DartVoidPtr      = void Function(RatsClient);

typedef _NativeIntPtr     = Int32 Function(RatsClient);
typedef _DartIntPtr       = int Function(RatsClient);

typedef _NativeConnect    = Int32 Function(RatsClient, Pointer<Utf8>, Int32);
typedef _DartConnect      = int Function(RatsClient, Pointer<Utf8>, int);

typedef _NativeStringRet  = Pointer<Utf8> Function(RatsClient);
typedef _DartStringRet    = Pointer<Utf8> Function(RatsClient);

typedef _NativeStringFree = Void Function(Pointer<Utf8>);
typedef _DartStringFree   = void Function(Pointer<Utf8>);

typedef _NativeAddStun    = Void Function(RatsClient, Pointer<Utf8>, Uint16);
typedef _DartAddStun      = void Function(RatsClient, Pointer<Utf8>, int);

typedef _NativeSetMaxPeers      = Int32 Function(RatsClient, Int32);
typedef _DartSetMaxPeers        = int Function(RatsClient, int);

typedef _NativeDiscoverPublic   =
    Pointer<Utf8> Function(RatsClient, Pointer<Utf8>, Uint16, Int32);
typedef _DartDiscoverPublic     =
    Pointer<Utf8> Function(RatsClient, Pointer<Utf8>, int, int);

typedef _NativeOnMessage =
    Int32 Function(RatsClient, Pointer<Utf8>, Pointer<NativeFunction<_NativeMessageCb>>,
                  Pointer<Void>);
typedef _DartOnMessage   =
    int Function(RatsClient, Pointer<Utf8>, Pointer<NativeFunction<_NativeMessageCb>>,
                Pointer<Void>);

typedef _NativeSendMessage =
    Int32 Function(RatsClient, Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>);
typedef _DartSendMessage   =
    int Function(RatsClient, Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>);

typedef _NativeSendBinary =
    Int32 Function(RatsClient, Pointer<Utf8>, Pointer<Void>, IntPtr);
typedef _DartSendBinary   =
    int Function(RatsClient, Pointer<Utf8>, Pointer<Void>, int);

// rats_get_validated_peer_ids — returns char** + writes count via int*
typedef _NativeGetPeerIds =
    Pointer<Pointer<Utf8>> Function(RatsClient, Pointer<Int32>);
typedef _DartGetPeerIds   =
    Pointer<Pointer<Utf8>> Function(RatsClient, Pointer<Int32>);

// rats_start_dht_discovery(client, dht_port) — returns rats_error_t (int)
typedef _NativeStartDht =
    Int32 Function(RatsClient, Int32);
typedef _DartStartDht   =
    int Function(RatsClient, int);

// rats_dht_set_bootstrap(client, host, port) — point the DHT at our PRIVATE
// bootstrap (the VPS) instead of the public mainline BitTorrent routers. The DHT
// still speaks KRPC (so the wire looks like BitTorrent), but it only ever talks
// to our own infrastructure. Returns rats_error_t (int).
typedef _NativeDhtSetBootstrap =
    Int32 Function(RatsClient, Pointer<Utf8>, Int32);
typedef _DartDhtSetBootstrap   =
    int Function(RatsClient, Pointer<Utf8>, int);

typedef _NativeIsDht =
    Int32 Function(RatsClient);
typedef _DartIsDht   =
    int Function(RatsClient);

// rats_announce_for_hash(client, hash, port, callback, user_data)
typedef _NativeAnnounce =
    Int32 Function(RatsClient, Pointer<Utf8>, Int32,
                    Pointer<NativeFunction<NativePeersFoundCb>>, Pointer<Void>);
typedef _DartAnnounce =
    int Function(RatsClient, Pointer<Utf8>, int,
                  Pointer<NativeFunction<NativePeersFoundCb>>, Pointer<Void>);

// Returns size_t — Dart uses Size (unsigned, pointer-width) to match the
// native return type exactly. IntPtr (signed) would silently flip to a
// negative routing-table size if the upper bit ever got set.
typedef _NativeDhtTableSize =
    Size Function(RatsClient);
typedef _DartDhtTableSize   =
    int Function(RatsClient);

// rats_get_public_address — STUN-discovered "host:port" string.
typedef _NativePublicAddrRet =
    Pointer<Utf8> Function(RatsClient);
typedef _DartPublicAddrRet   =
    Pointer<Utf8> Function(RatsClient);

// rats_disconnect_peer_by_id(client, peer_id) — returns rats_error_t.
typedef _NativeDisconnectPeer =
    Int32 Function(RatsClient, Pointer<Utf8>);
typedef _DartDisconnectPeer   =
    int Function(RatsClient, Pointer<Utf8>);

typedef _NativeSetConnCb =
    Void Function(RatsClient, Pointer<NativeFunction<_NativeConnCb>>, Pointer<Void>);
typedef _DartSetConnCb   =
    void Function(RatsClient, Pointer<NativeFunction<_NativeConnCb>>, Pointer<Void>);

typedef _NativeSetBinaryCb =
    Void Function(RatsClient, Pointer<NativeFunction<NativeBinaryCb>>, Pointer<Void>);
typedef _DartSetBinaryCb   =
    void Function(RatsClient, Pointer<NativeFunction<NativeBinaryCb>>, Pointer<Void>);

// -- Native callback signatures (must match librats_c.h exactly) -------
// Public so callers can declare matching trampolines.

typedef NativeConnCb    = Void Function(Pointer<Void>, Pointer<Utf8>);
typedef NativeMessageCb =
    Void Function(Pointer<Void>, Pointer<Utf8>, Pointer<Utf8>);
// rats_binary_cb: (void* user_data, const char* peer_id, const void* data, size_t size)
typedef NativeBinaryCb  =
    Void Function(Pointer<Void>, Pointer<Utf8>, Pointer<Void>, IntPtr);
// rats_peers_found_cb: (void* user_data, const char** peer_addresses, int count)
typedef NativePeersFoundCb =
    Void Function(Pointer<Void>, Pointer<Pointer<Utf8>>, Int32);

typedef _NativeConnCb    = NativeConnCb;
typedef _NativeMessageCb = NativeMessageCb;

// -- Bindings facade ---------------------------------------------------

class RatsBindings {
  RatsBindings(DynamicLibrary lib)
      : create          = lib.lookupFunction<_NativeCreate,         _DartCreate>('rats_create'),
        createWithId    = lib.providesSymbol('rats_create_with_id')
                            ? lib.lookupFunction<_NativeCreateWithId, _DartCreateWithId>('rats_create_with_id')
                            : null,
        destroy         = lib.lookupFunction<_NativeVoidPtr,        _DartVoidPtr>('rats_destroy'),
        start           = lib.lookupFunction<_NativeIntPtr,         _DartIntPtr>('rats_start'),
        stop            = lib.lookupFunction<_NativeVoidPtr,        _DartVoidPtr>('rats_stop'),
        connect         = lib.lookupFunction<_NativeConnect,        _DartConnect>('rats_connect'),
        getPeerCount    = lib.lookupFunction<_NativeIntPtr,         _DartIntPtr>('rats_get_peer_count'),
        getOurPeerId    = lib.lookupFunction<_NativeStringRet,      _DartStringRet>('rats_get_our_peer_id'),
        stringFree      = lib.lookupFunction<_NativeStringFree,     _DartStringFree>('rats_string_free'),
        addStunServer   = lib.lookupFunction<_NativeAddStun,        _DartAddStun>('rats_add_stun_server'),
        setMaxPeers     = lib.lookupFunction<_NativeSetMaxPeers,    _DartSetMaxPeers>('rats_set_max_peers'),
        discoverPublic  = lib.lookupFunction<_NativeDiscoverPublic, _DartDiscoverPublic>('rats_discover_public_address'),
        onMessage       = lib.lookupFunction<_NativeOnMessage,      _DartOnMessage>('rats_on_message'),
        sendMessage     = lib.lookupFunction<_NativeSendMessage,    _DartSendMessage>('rats_send_message'),
        setConnectionCb = lib.lookupFunction<_NativeSetConnCb,      _DartSetConnCb>('rats_set_connection_callback'),
        setDisconnectCb = lib.lookupFunction<_NativeSetConnCb,      _DartSetConnCb>('rats_set_disconnect_callback'),
        setBinaryCb     = lib.lookupFunction<_NativeSetBinaryCb,    _DartSetBinaryCb>('rats_set_binary_callback'),
        sendBinary      = lib.lookupFunction<_NativeSendBinary,     _DartSendBinary>('rats_send_binary'),
        getValidatedPeerIds = lib.lookupFunction<_NativeGetPeerIds, _DartGetPeerIds>('rats_get_validated_peer_ids'),
        startDhtDiscovery  = lib.lookupFunction<_NativeStartDht,     _DartStartDht>('rats_start_dht_discovery'),
        dhtSetBootstrap    = lib.providesSymbol('rats_dht_set_bootstrap')
                               ? lib.lookupFunction<_NativeDhtSetBootstrap, _DartDhtSetBootstrap>('rats_dht_set_bootstrap')
                               : null,
        stopDhtDiscovery   = lib.lookupFunction<_NativeVoidPtr,      _DartVoidPtr>('rats_stop_dht_discovery'),
        isDhtRunning       = lib.lookupFunction<_NativeIsDht,        _DartIsDht>('rats_is_dht_running'),
        announceForHash    = lib.lookupFunction<_NativeAnnounce,     _DartAnnounce>('rats_announce_for_hash'),
        dhtRoutingTableSize = lib.lookupFunction<_NativeDhtTableSize, _DartDhtTableSize>('rats_get_dht_routing_table_size'),
        getPublicAddress   = lib.lookupFunction<_NativePublicAddrRet,_DartPublicAddrRet>('rats_get_public_address'),
        disconnectPeerById = lib.lookupFunction<_NativeDisconnectPeer,_DartDisconnectPeer>('rats_disconnect_peer_by_id');

  final _DartCreate         create;
  final _DartCreateWithId?  createWithId;  // null if loaded lib predates wallet-as-id
  final _DartVoidPtr        destroy;
  final _DartIntPtr         start;
  final _DartVoidPtr        stop;
  final _DartConnect        connect;
  final _DartIntPtr         getPeerCount;
  final _DartStringRet      getOurPeerId;
  final _DartStringFree     stringFree;
  final _DartAddStun        addStunServer;
  final _DartSetMaxPeers    setMaxPeers;
  final _DartDiscoverPublic discoverPublic;
  final _DartOnMessage      onMessage;
  final _DartSendMessage    sendMessage;
  final _DartSetConnCb      setConnectionCb;
  final _DartSetConnCb      setDisconnectCb;
  final _DartSetBinaryCb    setBinaryCb;
  final _DartSendBinary     sendBinary;
  final _DartGetPeerIds     getValidatedPeerIds;
  final _DartStartDht       startDhtDiscovery;
  final _DartDhtSetBootstrap? dhtSetBootstrap;
  final _DartVoidPtr        stopDhtDiscovery;
  final _DartIsDht          isDhtRunning;
  final _DartAnnounce       announceForHash;
  final _DartDhtTableSize   dhtRoutingTableSize;
  final _DartPublicAddrRet  getPublicAddress;
  final _DartDisconnectPeer disconnectPeerById;
}
