// Battery + screen-on/off capture for the offline play-proof bundle.
//
// These two sensor streams are the hardest of the bundle's signals
// for a bot to fake: a real phone playing audio for 4 hours loses 8-
// 20 % battery and the user locks/unlocks the screen N times. A
// sandbox attacker has to fabricate plausible curves for both.
//
// We avoid pulling in `battery_plus` to honour the pubspec lean rule.
// Instead we sketch a platform-channel stub (`_readBatteryPercentStub`)
// that the follow-up PR replaces with a real method-channel call to
// `BatteryManager` on Android / `UIDevice.batteryLevel` on iOS. The
// table records whatever the stub returns; until the channel ships
// every sample is `percent = -1` so the home-node heuristic knows to
// skip the battery rule for this device.
//
// Screen on/off uses Flutter's `WidgetsBindingObserver` lifecycle as a
// proxy. It's not perfectly accurate (the system lock screen doesn't
// always trigger `paused`) but it's the best the SDK gives us without
// a platform channel. The bundle field is documented as "screen
// intervals" to capture the proxy nature.

import 'dart:async';

import 'package:flutter/widgets.dart';

import 'heartbeat_capture.dart';

class SensorCapture with WidgetsBindingObserver {
  SensorCapture._();
  static final SensorCapture instance = SensorCapture._();

  Timer? _batteryTimer;
  int?   _openScreenIntervalId;

  Future<void> start() async {
    // Battery sample cadence is 60 s — dense enough to catch a 4-hour
    // session's slope, sparse enough to barely register against the
    // app's overall power draw.
    if (_batteryTimer != null) {
      // Already started — re-running start() (e.g. on hot reload) must
      // not double-register the lifecycle observer or orphan the
      // previously opened screen interval.
      return;
    }
    // Run an initial sample so even very short sessions have at
    // least one row.
    await _sampleBattery();
    _batteryTimer = Timer.periodic(
        const Duration(seconds: 60), (_) => _sampleBattery());
    // Hook the lifecycle observer once. The first onResume after
    // attach is treated as the opening screen-on event for the bundle.
    WidgetsBinding.instance.addObserver(this);
    // Open an interval right away — we're presumably foregrounded at
    // service-start time (app just launched).
    _openScreenIntervalId =
        await HeartbeatCapture.instance.openScreenInterval();
  }

  void stop() {
    _batteryTimer?.cancel();
    _batteryTimer = null;
    WidgetsBinding.instance.removeObserver(this);
    // Close the interval cleanly so submit picks it up.
    final id = _openScreenIntervalId;
    if (id != null) {
      unawaited(HeartbeatCapture.instance.closeScreenInterval(id));
      _openScreenIntervalId = null;
    }
  }

  Future<void> _sampleBattery() async {
    final pct = await _readBatteryPercentStub();
    final charging = await _readChargingStub();
    await HeartbeatCapture.instance.recordBattery(
      percent: pct,
      charging: charging,
    );
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) async {
    switch (state) {
      case AppLifecycleState.resumed:
        // Screen came back on (or the app moved to foreground). Open
        // a new interval if we don't already have one.
        _openScreenIntervalId ??=
            await HeartbeatCapture.instance.openScreenInterval();
        break;
      case AppLifecycleState.paused:
      case AppLifecycleState.detached:
      case AppLifecycleState.hidden:
        final id = _openScreenIntervalId;
        if (id != null) {
          await HeartbeatCapture.instance.closeScreenInterval(id);
          _openScreenIntervalId = null;
        }
        break;
      case AppLifecycleState.inactive:
        // Transient — incoming call, app switcher. Don't close the
        // interval here; the system fires `paused` for real backgrounding.
        break;
    }
  }

  /// TODO(offline_play_proof): wire to Android `BatteryManager
  /// .BATTERY_PROPERTY_CAPACITY` / iOS `UIDevice.batteryLevel` via a
  /// platform channel. Returning -1 tells the home node heuristic to
  /// skip the battery rule for this row.
  Future<int> _readBatteryPercentStub() async {
    return -1;
  }

  /// TODO(offline_play_proof): wire to Android `BatteryManager
  /// .BATTERY_STATUS_CHARGING` / iOS `UIDevice.batteryState ==
  /// charging`. Returning false until the channel lands keeps the
  /// schema column populated.
  Future<bool> _readChargingStub() async {
    return false;
  }
}
