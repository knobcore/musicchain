// Bundles the offline play-log into a signed payload and POSTs it to
// the home node via `offline.play_proof.submit`.
//
// Trigger: on a 60 s cadence we ask the RatsClient whether the home
// node is reachable. If yes AND there's at least one unsubmitted
// session AND a wallet is loaded, we build, sign, send. Cadence is
// also kicked on every wifi_up / cell_up transition so a freshly-
// online listener doesn't have to wait up to a minute for the next
// poll tick.
//
// Bundle format is documented in docs/offline_play_proof.md. Signing
// uses the player's wallet key via `WalletService.sign` (ECDSA over
// secp256k1, same key that signs token transfers).

import 'dart:async';
import 'dart:convert';
import 'dart:math';
import 'dart:typed_data';

import '../device_fingerprint_service.dart';
import '../node_service.dart';
import '../rats_client.dart';
import '../wallet_service.dart';
import 'heartbeat_capture.dart';

class OfflineSubmitService {
  OfflineSubmitService._();
  static final OfflineSubmitService instance = OfflineSubmitService._();

  Timer? _timer;
  bool   _submitting = false;
  WalletService? _wallet;

  /// Caller (main.dart) hands us a WalletService that already has its
  /// auto-load future in flight. We don't try to load a wallet ourselves
  /// — that's WalletProvider's job and the user could pick any of N
  /// wallet flows. If [wallet] never has a key by submit time we just
  /// skip the tick and wait.
  Future<void> start({WalletService? wallet}) async {
    if (_timer != null) return;
    _wallet = wallet;
    // First tick on a short delay so we don't race the rats client's
    // initial handshake (which has its own ~3 s warmup).
    Timer(const Duration(seconds: 10), () => _tick());
    _timer = Timer.periodic(const Duration(seconds: 60), (_) => _tick());
  }

  /// Lets callers (NetworkTransitionWatcher) poke the service when they
  /// see a wifi/cell up event — saves the user up to 60 s of buffered
  /// rows sitting around when they walk back into wifi range.
  Future<void> kick() => _tick();

  void stop() {
    _timer?.cancel();
    _timer = null;
  }

  Future<void> _tick() async {
    if (_submitting) return;
    final wallet = _wallet;
    if (wallet == null || wallet.info == null) return;
    final addr = wallet.info!.address;

    // Need a reachable home node — go through NodeService which already
    // resolves the auto-selected node's rats peer id.
    String? peerId;
    try {
      peerId = await NodeService.getRatsPeerId();
    } catch (_) {
      peerId = null;
    }
    if (peerId == null || peerId.isEmpty) return;

    // Need rats actually up (mini-node connection alive). If
    // validatedPeerIds is empty we're offline at the transport layer.
    try {
      if (RatsClient.instance.validatedPeerIds.isEmpty) return;
    } catch (_) {
      return;
    }

    _submitting = true;
    try {
      await HeartbeatCapture.instance.trim();
      final sessions = await HeartbeatCapture.instance
          .unsubmittedSessions(addr);
      if (sessions.isEmpty) return;
      final transitions = await HeartbeatCapture.instance
          .unsubmittedTransitions();
      final battery = await HeartbeatCapture.instance.unsubmittedBattery();
      final screens = await HeartbeatCapture.instance
          .unsubmittedScreenIntervals();

      final bundle = await _buildBundle(
        playerAddress: addr,
        pubkey:        wallet.info!.publicKey,
        sessions:      sessions,
        transitions:   transitions,
        battery:       battery,
        screens:       screens,
        wallet:        wallet,
      );

      // Snapshot the cutoff BEFORE the RPC. The mark-submitted call
      // below uses this to flush sensor/transition rows; if we instead
      // took "now" after the RPC (which can be up to 60 s later) we'd
      // silently mark rows captured during the RPC as submitted even
      // though they were never in the bundle, losing them forever.
      final cutoffWallMs = HeartbeatCapture.instance.wallMs();

      try {
        await RatsClient.instance.request(
          peerId, 'offline.play_proof.submit', bundle,
          timeout: const Duration(seconds: 60),
        );
        await HeartbeatCapture.instance.markSubmitted(
          sessions.map((s) => s.sessionId).toList(),
          cutoffWallMs: cutoffWallMs,
        );
        // ignore: avoid_print
        print('[offline-play-log] submitted bundle: '
              '${sessions.length} sessions, '
              '${transitions.length} transitions, '
              '${battery.length} batt, ${screens.length} screen');
      } catch (e) {
        // ignore: avoid_print
        print('[offline-play-log] submit failed (will retry): $e');
      }
    } finally {
      _submitting = false;
    }
  }

  Future<Map<String, dynamic>> _buildBundle({
    required String                  playerAddress,
    required String                  pubkey,
    required List<CapturedSession>   sessions,
    required List<Map<String, dynamic>> transitions,
    required List<Map<String, dynamic>> battery,
    required List<Map<String, dynamic>> screens,
    required WalletService           wallet,
  }) async {
    final rng = Random.secure();
    final nonceBytes = List<int>.generate(32, (_) => rng.nextInt(256));
    final nonceHex   = nonceBytes
        .map((b) => b.toRadixString(16).padLeft(2, '0')).join();
    // #5: hardware-derived device attestation. device_id is now the hardware
    // fingerprint (stable across reinstalls) rather than a random token, and
    // the `attestation` object rides INSIDE the wallet-signed bundle so the
    // signature binds (device ↔ wallet) — exactly the "include the wallet"
    // requirement, without baking the wallet into the per-device id itself.
    final attest     = await DeviceFingerprintService.instance.get();
    final deviceId   = attest.deviceKey;

    // Bundle base time = the earliest captured signal we're shipping.
    final wallBase = sessions
        .map((s) => s.startedWallMs)
        .followedBy(transitions.map((t) => t['wall_ms'] as int))
        .followedBy(battery.map((b) => b['wall_ms'] as int))
        .followedBy(screens.map((s) => s['on_wall_ms'] as int))
        .fold<int>(1 << 62, min);
    final monoBase = sessions
        .map((s) => s.startedMonotonicMs)
        .followedBy(transitions.map((t) => t['monotonic_ms'] as int))
        .followedBy(battery.map((b) => b['monotonic_ms'] as int))
        .followedBy(screens.map((s) => s['on_monotonic_ms'] as int))
        .fold<int>(1 << 62, min);

    final body = <String, dynamic>{
      'bundle_version':    1,
      'player_address':    playerAddress,
      'pubkey':            pubkey,
      'bundle_nonce':      nonceHex,
      'created_at_ms':     DateTime.now().millisecondsSinceEpoch,
      'device_id':         deviceId,
      'attestation':       attest.toJson(),
      'monotonic_base_ms': monoBase,
      'wall_base_ms':      wallBase,
      'sessions':          sessions.map((s) => s.toJson()).toList(),
      'network_transitions': transitions,
      'battery_samples':   battery,
      'screen_intervals':  screens,
    };

    // Canonicalize: both the player and home node build the signed
    // payload by sorting object keys ascending, emitting no whitespace,
    // preserving array order, and JSON-encoding primitives. We pass
    // the canonical UTF-8 bytes directly to `wallet.sign`, which
    // sha256-hashes them inside the FFI call (mc_wallet_sign matches
    // verify_ecdsa(sha256(canonical), sig, pubkey)).
    final canonical = utf8.encode(_canonicalJson(body));
    final sig       = wallet.sign(Uint8List.fromList(canonical));
    body['signature'] = sig;
    return body;
  }

  String _canonicalJson(Object? v) {
    if (v is Map) {
      final keys = v.keys.cast<String>().toList()..sort();
      final buf = StringBuffer('{');
      for (var i = 0; i < keys.length; i++) {
        if (i > 0) buf.write(',');
        buf
          ..write(jsonEncode(keys[i]))
          ..write(':')
          ..write(_canonicalJson(v[keys[i]]));
      }
      buf.write('}');
      return buf.toString();
    }
    if (v is List) {
      final buf = StringBuffer('[');
      for (var i = 0; i < v.length; i++) {
        if (i > 0) buf.write(',');
        buf.write(_canonicalJson(v[i]));
      }
      buf.write(']');
      return buf.toString();
    }
    return jsonEncode(v);
  }

}
