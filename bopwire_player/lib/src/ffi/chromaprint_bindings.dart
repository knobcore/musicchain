// Dart FFI binding for the slice of libchromaprint the player uses.
//
// We feed 16-bit signed mono PCM (decoded on the Android side via
// MediaCodec — see android/app/src/main/kotlin/.../FingerprintBridge.kt)
// and recover a base64-compressed fingerprint string. The full node hashes
// SHA256(fingerprint_string) and reuses the same digest as the on-chain
// fingerprint_hash field — see src/api/rats_api.cpp fingerprint.submit.
//
// chromaprint API surface used:
//   chromaprint_new                  → ChromaprintContext*
//   chromaprint_start                → start(ctx, sample_rate, num_channels)
//   chromaprint_feed                 → feed(ctx, data, size_in_int16_samples)
//   chromaprint_finish               → finish(ctx)
//   chromaprint_get_fingerprint      → get base64 fingerprint string
//   chromaprint_dealloc              → free strings returned by chromaprint
//   chromaprint_free                 → free the context

import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';

typedef _CpNewC      = Pointer<Void> Function(Int32);
typedef _CpNew       = Pointer<Void> Function(int);

typedef _CpFreeC     = Void Function(Pointer<Void>);
typedef _CpFree      = void Function(Pointer<Void>);

typedef _CpStartC    = Int32 Function(Pointer<Void>, Int32, Int32);
typedef _CpStart     = int  Function(Pointer<Void>, int, int);

typedef _CpFeedC     = Int32 Function(Pointer<Void>, Pointer<Int16>, Int32);
typedef _CpFeed      = int  Function(Pointer<Void>, Pointer<Int16>, int);

typedef _CpFinishC   = Int32 Function(Pointer<Void>);
typedef _CpFinish    = int  Function(Pointer<Void>);

typedef _CpGetFpC    = Int32 Function(Pointer<Void>, Pointer<Pointer<Utf8>>);
typedef _CpGetFp     = int  Function(Pointer<Void>, Pointer<Pointer<Utf8>>);

typedef _CpDeallocC  = Void Function(Pointer<Void>);
typedef _CpDealloc   = void Function(Pointer<Void>);

/// Algorithm enum value used by chromaprint_new. CHROMAPRINT_ALGORITHM_DEFAULT
/// is 1 in chromaprint 1.5.x (it's a 0-indexed enum starting at TEST1).
const int kChromaprintAlgorithmDefault = 1;

class ChromaprintBindings {
  ChromaprintBindings._(this._lib)
      : create   = _lib.lookupFunction<_CpNewC,     _CpNew>('chromaprint_new'),
        destroy  = _lib.lookupFunction<_CpFreeC,    _CpFree>('chromaprint_free'),
        start    = _lib.lookupFunction<_CpStartC,   _CpStart>('chromaprint_start'),
        feed     = _lib.lookupFunction<_CpFeedC,    _CpFeed>('chromaprint_feed'),
        finish   = _lib.lookupFunction<_CpFinishC,  _CpFinish>('chromaprint_finish'),
        getFp    = _lib.lookupFunction<_CpGetFpC,   _CpGetFp>('chromaprint_get_fingerprint'),
        dealloc  = _lib.lookupFunction<_CpDeallocC, _CpDealloc>('chromaprint_dealloc');

  static ChromaprintBindings? _instance;
  static ChromaprintBindings get instance =>
      _instance ??= ChromaprintBindings._(_loadLibrary());

  final DynamicLibrary _lib;
  final _CpNew      create;
  final _CpFree     destroy;
  final _CpStart    start;
  final _CpFeed     feed;
  final _CpFinish   finish;
  final _CpGetFp    getFp;
  final _CpDealloc  dealloc;

  static DynamicLibrary _loadLibrary() {
    if (Platform.isAndroid) return DynamicLibrary.open('libchromaprint.so');
    if (Platform.isWindows) return DynamicLibrary.open('chromaprint.dll');
    if (Platform.isLinux)   return DynamicLibrary.open('libchromaprint.so');
    if (Platform.isMacOS)   return DynamicLibrary.open('libchromaprint.dylib');
    throw UnsupportedError('Unsupported platform: ${Platform.operatingSystem}');
  }
}
