// Watches wifi / cellular up-and-down events and records them into the
// HeartbeatCapture log so the offline bundle can prove the device's
// radio state matched its claimed play history.
//
// We deliberately avoid pulling in `connectivity_plus` to keep the
// pubspec lean — the user explicitly asked us not to add deps we
// don't strictly need. Instead we poll `NetworkInterface.list()` from
// dart:io on a 30 s cadence and diff against the last snapshot. That's
// coarse but enough to catch the multi-minute radio events the bot
// heuristics actually look for (a 5-hour cell session with no
// transitions, a flatline wifi BSSID across a claimed road trip).
//
// BSSID / cell-id collection requires a platform channel (Android's
// `WifiManager.getConnectionInfo().getBSSID()` is a method call;
// `TelephonyManager.getAllCellInfo()` likewise). We sketch the call
// site as `_readBssidStub` / `_readCellIdStub` so the followup PR has
// a single place to wire the channel into. For now they return empty
// fingerprints. The bundle still has the up/down transition wall_ms
// and monotonic_ms, which is the load-bearing forgery signal — the
// fingerprint is the cherry on top.

import 'dart:async';
import 'dart:io';

import 'package:permission_handler/permission_handler.dart';

import 'heartbeat_capture.dart';
import 'offline_submit_service.dart';

enum _Iface { none, wifi, cellular }

class NetworkTransitionWatcher {
  NetworkTransitionWatcher._();
  static final NetworkTransitionWatcher instance =
      NetworkTransitionWatcher._();

  Timer? _timer;
  _Iface _lastWifi      = _Iface.none;
  _Iface _lastCellular  = _Iface.none;
  String _lastWifiFp    = '';
  String _lastCellFp    = '';

  Future<void> start() async {
    if (_timer != null) return;
    // Best-effort permission request. The location permission is what
    // gates BSSID reads on Android 10+; on iOS the equivalent is
    // `Location When In Use`. We don't block startup on the user's
    // answer — the watcher works without BSSID, it just records zero
    // fingerprints.
    try {
      if (Platform.isAndroid || Platform.isIOS) {
        // Don't `await` actually — fire-and-forget so the watcher
        // still ticks even before the user taps Allow.
        unawaited(Permission.locationWhenInUse.request());
      }
    } catch (_) {
      // permission_handler throws UnimplementedError on desktop; fine.
    }
    // Run a tick immediately so the bundle has a baseline up-event for
    // the current interface state.
    await _tick();
    _timer = Timer.periodic(const Duration(seconds: 30), (_) => _tick());
  }

  void stop() {
    _timer?.cancel();
    _timer = null;
  }

  Future<void> _tick() async {
    final wifi = await _wifiState();
    final cell = await _cellularState();
    final wifiFp = wifi == _Iface.wifi     ? await _readBssidStub()  : '';
    final cellFp = cell == _Iface.cellular ? await _readCellIdStub() : '';

    bool wentOnline = false;
    if (wifi != _lastWifi) {
      final kind = wifi == _Iface.wifi ? 'wifi_up' : 'wifi_down';
      await HeartbeatCapture.instance.recordTransition(
        kind: kind, fingerprint: wifi == _Iface.wifi ? wifiFp : _lastWifiFp);
      if (wifi == _Iface.wifi && _lastWifi == _Iface.none) wentOnline = true;
      _lastWifi = wifi;
    } else if (wifi == _Iface.wifi && wifiFp.isNotEmpty &&
               wifiFp != _lastWifiFp) {
      // BSSID changed without a full down → log a roam transition. Real
      // wifi roams (one AP to another) are an important forgery signal:
      // bots tend to use a single static BSSID for the whole bundle.
      await HeartbeatCapture.instance.recordTransition(
        kind: 'wifi_roam', fingerprint: wifiFp);
    }
    if (cell != _lastCellular) {
      final kind = cell == _Iface.cellular ? 'cell_up' : 'cell_down';
      await HeartbeatCapture.instance.recordTransition(
        kind: kind, fingerprint: cell == _Iface.cellular ? cellFp : _lastCellFp);
      if (cell == _Iface.cellular && _lastCellular == _Iface.none) {
        wentOnline = true;
      }
      _lastCellular = cell;
    } else if (cell == _Iface.cellular && cellFp.isNotEmpty &&
               cellFp != _lastCellFp) {
      await HeartbeatCapture.instance.recordTransition(
        kind: 'cell_handoff', fingerprint: cellFp);
    }
    if (wifiFp.isNotEmpty) _lastWifiFp = wifiFp;
    if (cellFp.isNotEmpty) _lastCellFp = cellFp;

    // Kick the submitter so the buffered bundle goes out within a few
    // seconds of regaining connectivity instead of waiting for its own
    // 60 s poll.
    if (wentOnline) {
      unawaited(OfflineSubmitService.instance.kick());
    }
  }

  Future<_Iface> _wifiState() async {
    try {
      final ifaces = await NetworkInterface.list(
        includeLoopback: false,
        type: InternetAddressType.any,
      );
      for (final i in ifaces) {
        final name = i.name.toLowerCase();
        // Common interface names for wifi adapters across platforms.
        // Not authoritative — on Android the interface is `wlan0` but
        // can be e.g. `wlp3s0` on Linux desktop builds.
        if (name.contains('wlan') ||
            name.startsWith('wl')  ||
            name.contains('wifi')  ||
            name.startsWith('en0')) {
          if (i.addresses.isNotEmpty) return _Iface.wifi;
        }
      }
    } catch (_) {}
    return _Iface.none;
  }

  Future<_Iface> _cellularState() async {
    try {
      final ifaces = await NetworkInterface.list(
        includeLoopback: false,
        type: InternetAddressType.any,
      );
      for (final i in ifaces) {
        final name = i.name.toLowerCase();
        // `rmnet*` is the Android cellular interface family; `pdp_ip*`
        // on iOS. Anything matching counts as cellular up.
        if (name.startsWith('rmnet')  ||
            name.startsWith('pdp_ip') ||
            name.contains('ccmni')) {
          if (i.addresses.isNotEmpty) return _Iface.cellular;
        }
      }
    } catch (_) {}
    return _Iface.none;
  }

  /// TODO(offline_play_proof): implement via Android `WifiManager` /
  /// iOS `NEHotspotNetwork`. Both require a platform channel; until
  /// that's wired we return empty so the recorder logs a zero
  /// fingerprint. The bundle's wall_ms / monotonic_ms timestamps are
  /// the load-bearing forgery signal — the BSSID is supplemental.
  Future<String> _readBssidStub() async {
    return '';
  }

  /// TODO(offline_play_proof): Android `TelephonyManager.getAllCellInfo()`
  /// → CellInfoLte.getCellIdentity().getCi(). iOS does not expose this.
  /// Until the channel ships, return empty.
  Future<String> _readCellIdStub() async {
    return '';
  }
}
