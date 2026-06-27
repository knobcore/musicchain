// BackgroundScanner schedules a workmanager periodic task that re-runs
// LibraryScanner.scanOnce so files newly added to one of the user's
// folders get fingerprinted without requiring the app to be in the
// foreground.
//
// Android workmanager guarantees a periodic task fires "at least once
// per interval" but defers it to a battery-friendly window — the actual
// cadence is best-effort. The minimum interval Android honours is 15 min;
// we use 30 min to keep the wakeup count low. We also re-run the scan
// once at process start (in main.dart) so a fresh launch picks up any
// files added while the app was killed.

import 'dart:async';

import 'package:flutter/widgets.dart';
import 'package:workmanager/workmanager.dart';

import 'library_scanner.dart';
import 'library_service.dart';
import 'rats_client.dart';

const String _kTaskName    = 'mc_library_scan';
const String _kUniqueName  = 'mc_library_scan_unique';

@pragma('vm:entry-point')
void backgroundScannerDispatcher() {
  Workmanager().executeTask((task, inputData) async {
    if (task != _kTaskName) return true;
    try {
      // The dispatcher runs in a fresh isolate. Bring up just enough of
      // the player stack to scan + submit fingerprints: RatsClient (so
      // we can reach the full node) and LibraryService (the folder list
      // + entry index live in SharedPreferences and reload here).
      WidgetsFlutterBinding.ensureInitialized();
      await LibraryService.instance.ensureLoaded();
      await RatsClient.initialize();
      try {
        await LibraryScanner.instance.scanOnce();
      } finally {
        // CRITICAL: this dispatcher runs in a short-lived workmanager isolate.
        // RatsClient.initialize() started native librats worker threads (DHT,
        // connections) that hold pointers to this isolate's NativeCallable
        // trampolines. If we let the isolate be torn down without stopping the
        // native client, those threads fire into freed trampolines and the VM
        // aborts ("Callback invoked after it has been deleted") — the ~30-min
        // crash. Dispose synchronously stops + destroys the native client
        // (joining its threads) BEFORE the isolate ends, so nothing dangles.
        try { RatsClient.instance.dispose(); } catch (_) {}
      }
    } catch (_) {
      // Best-effort. Returning true tells workmanager not to retry; the
      // next periodic firing will pick up anything missed.
    }
    return true;
  });
}

class BackgroundScanner {
  BackgroundScanner._();

  static bool _initialized = false;

  /// Register the workmanager periodic task. Idempotent. Should be
  /// called once during app startup after RatsClient + LibraryService
  /// are up.
  static Future<void> initialize() async {
    if (_initialized) return;
    _initialized = true;
    await Workmanager().initialize(
      backgroundScannerDispatcher,
      isInDebugMode: false,
    );
    await Workmanager().registerPeriodicTask(
      _kUniqueName,
      _kTaskName,
      // Android floors this at 15 min; 30 keeps the wakeup count modest.
      frequency: const Duration(minutes: 30),
      // Don't insist on Wi-Fi / charging — fingerprinting a few files is
      // cheap and the resulting fingerprint.submit RPC is small.
      constraints: Constraints(
        networkType: NetworkType.connected,
      ),
      existingWorkPolicy: ExistingPeriodicWorkPolicy.keep,
      backoffPolicy: BackoffPolicy.exponential,
      backoffPolicyDelay: const Duration(minutes: 5),
    );
  }
}
