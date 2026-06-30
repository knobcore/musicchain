// Legacy background-scan shim.
//
// Older builds registered a workmanager PERIODIC task that woke a SEPARATE
// isolate to scan the library. That isolate brought up its OWN librats client
// (scanOnce submits fingerprints over rats); on the isolate's teardown a native
// librats worker thread fired an FFI callback into the isolate's freed
// trampoline, which aborts the WHOLE process ("Callback invoked after it has
// been deleted") and drops the network connection — observed as repeated
// crashes and "network flapping". workmanager then retried the crashed task,
// looping the crash.
//
// The periodic scan now runs in the MAIN isolate (see main.dart), which owns
// the single process-wide librats client (kept alive in the background by
// BopwireService). This file's only remaining job is to CANCEL any
// previously-registered workmanager task so the crashing path can never fire
// again. The dispatcher is retained — and made inert — only so workmanager has
// a valid entry-point to cancel against.

import 'package:workmanager/workmanager.dart';

const String _kUniqueName = 'mc_library_scan_unique';

@pragma('vm:entry-point')
void backgroundScannerDispatcher() {
  // Inert: NEVER create a librats client in a workmanager isolate (see above).
  Workmanager().executeTask((task, inputData) async => true);
}

class BackgroundScanner {
  BackgroundScanner._();

  static bool _initialized = false;

  /// Cancel the legacy workmanager periodic task. Idempotent. The scan itself
  /// now runs in the main isolate; a workmanager isolate would spin up a second
  /// librats client and crash the process on teardown.
  static Future<void> initialize() async {
    if (_initialized) return;
    _initialized = true;
    await Workmanager().initialize(backgroundScannerDispatcher);
    await Workmanager().cancelByUniqueName(_kUniqueName);
  }
}
