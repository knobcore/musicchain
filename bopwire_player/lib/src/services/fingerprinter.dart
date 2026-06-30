// Local audio fingerprinting on the player side.
//
// Pipeline:
//   1. Hand the file path to the Android MediaCodec bridge in
//      MainActivity.kt → it returns interleaved 16-bit little-endian PCM
//      plus sample rate + channel count.
//   2. Feed the PCM into libchromaprint via the Dart FFI binding in
//      lib/src/ffi/chromaprint_bindings.dart.
//   3. Return the base64 compressed fingerprint string + SHA256 of it.
//      The SHA256 is what fingerprint.submit takes as a lookup key.

import 'dart:async';
import 'dart:convert';
import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

import 'package:crypto/crypto.dart' as crypto;
import 'package:ffi/ffi.dart';
import 'package:flutter/services.dart';

import '../ffi/chromaprint_bindings.dart';
import '../ffi/mc_decoder_bindings.dart';

class FingerprintResult {
  FingerprintResult({
    required this.compressed,
    required this.fingerprintHash,
    required this.sampleRate,
    required this.channelCount,
    required this.pcmSamples,
  });

  final String compressed;       // base64 string from chromaprint
  final String fingerprintHash;  // SHA256(compressed) hex — what we submit
  final int    sampleRate;
  final int    channelCount;
  final int    pcmSamples;
}

class Fingerprinter {
  static const MethodChannel _decode =
      MethodChannel('bopwire/fingerprint_decode');

  /// Decode + fingerprint the audio at [path]. Routes through the
  /// platform-specific decoder, then feeds the resulting 16-bit PCM
  /// through libchromaprint via the FFI binding.
  ///
  /// * Android: MethodChannel "fingerprint" — Kotlin streams PCM
  ///   straight into chromaprint via JNI in one pass, no Dart-side
  ///   buffer or feed loop. Falls back to the legacy PCM-roundtrip
  ///   path only if the native lib didn't load (e.g. dev / emulator
  ///   without arm64 binaries).
  /// * Windows: FFI → mc_decoder.dll (Media Foundation) → PCM, then
  ///   chromaprint via Dart FFI.
  /// * Linux/macOS: not yet wired; throws UnsupportedError.
  static Future<FingerprintResult> ofFile(String path) async {
    if (Platform.isAndroid) {
      // Fast path: Kotlin does the streaming decode + feed and returns
      // the compressed fingerprint string already. Saves the ~30 MB
      // PCM round-trip plus the Dart-side copy + chunked FFI feed.
      try {
        final reply = await _decode.invokeMethod<Map<dynamic, dynamic>>(
            'fingerprint', {'path': path});
        if (reply != null) {
          // Kotlin returns Map<String, Any?>; values come through as
          // nullable on the Dart side. Bail out to the legacy decode
          // path rather than NPE-via-TypeError if the bridge omitted
          // the fingerprint string (e.g. older APK, malformed reply).
          final compressedRaw = reply['compressed'];
          // (#crash) coerce every field — a raw `as int` here would TypeError
          // on a null/double from the bridge and ESCAPE (the on PlatformException
          // clauses don't catch a TypeError). Null fields → fall through to legacy.
          final compressed   = compressedRaw is String ? compressedRaw : '';
          final sampleRate   = (reply['sample_rate']   as num?)?.toInt();
          final channelCount = (reply['channel_count'] as num?)?.toInt();
          final pcmSamples   = (reply['pcm_samples']   as num?)?.toInt() ?? 0;
          if (compressed.isNotEmpty && sampleRate != null && channelCount != null) {
            final digest = crypto.sha256.convert(utf8.encode(compressed));
            return FingerprintResult(
              compressed:      compressed,
              fingerprintHash: digest.toString(),
              sampleRate:      sampleRate,
              channelCount:    channelCount,
              pcmSamples:      pcmSamples,
            );
          }
        }
      } on MissingPluginException {/* fall through to legacy */}
      on PlatformException catch (e) {
        // "decode_failed" / "file_missing" — rethrow so the scanner
        // counts it as an error.  Any other PlatformException
        // (e.g. notImplemented from a stale APK) drops to the legacy
        // PCM path below.
        if (e.code == 'decode_failed' || e.code == 'file_missing') rethrow;
      }
    }

    final Uint8List pcm;
    final int sampleRate;
    final int channelCount;
    if (Platform.isAndroid) {
      final reply = await _decode.invokeMethod<Map<dynamic, dynamic>>(
          'decodeToPcm', {'path': path});
      if (reply == null) {
        throw StateError('decodeToPcm returned null');
      }
      // (#crash) validate before assigning — a raw `as Uint8List`/`as int` on
      // a malformed bridge reply TypeErrors out of the scan with no context.
      final pcmRaw = reply['pcm'];
      final sr     = (reply['sample_rate']   as num?)?.toInt();
      final cc     = (reply['channel_count'] as num?)?.toInt();
      if (pcmRaw is! Uint8List || sr == null || cc == null) {
        throw StateError('decodeToPcm returned a malformed reply');
      }
      pcm          = pcmRaw;
      sampleRate   = sr;
      channelCount = cc;
    } else if (Platform.isWindows) {
      final d = MediaFoundationDecoder.instance.decode(path);
      pcm          = d.pcm;
      sampleRate   = d.sampleRate;
      channelCount = d.channelCount;
    } else {
      throw UnsupportedError(
          'Audio decode not yet wired for ${Platform.operatingSystem}; '
          'add a native decoder path before fingerprinting.');
    }

    final cp     = ChromaprintBindings.instance;
    final ctx    = cp.create(kChromaprintAlgorithmDefault);
    if (ctx.address == 0) {
      throw StateError('chromaprint_new returned null');
    }
    final pcm16   = pcm.buffer.asInt16List(pcm.offsetInBytes, pcm.length ~/ 2);
    final samples = pcm16.length;
    final native  = malloc<Int16>(samples);
    try {
      if (cp.start(ctx, sampleRate, channelCount) != 1) {
        throw StateError('chromaprint_start rejected sr=$sampleRate '
                         'ch=$channelCount');
      }
      // Bulk-copy the whole PCM into the native buffer using
      // asTypedList — replaces a hand-rolled byte-by-byte Dart loop
      // that took 5–10 s per song on a phone. The view aliases the
      // FFI-owned memory so `setRange` does a single memcpy under the
      // hood.
      final nativeView = native.asTypedList(samples);
      nativeView.setRange(0, samples, pcm16);
      // Feed in 256 KiB-sample chunks so we yield to the event loop a
      // few times on long files (so progress callbacks fire) without
      // adding meaningful overhead.
      const chunk = 256 * 1024;
      var fed = 0;
      while (fed < samples) {
        final n = (fed + chunk <= samples) ? chunk : (samples - fed);
        if (cp.feed(ctx, native.elementAt(fed), n) != 1) {
          throw StateError('chromaprint_feed failed at offset $fed');
        }
        fed += n;
        // Let any pending Dart microtasks (progress UI updates) run.
        if (fed < samples) await Future<void>.delayed(Duration.zero);
      }
      if (cp.finish(ctx) != 1) {
        throw StateError('chromaprint_finish failed');
      }
      final outPtr = malloc<Pointer<Utf8>>();
      try {
        if (cp.getFp(ctx, outPtr) != 1) {
          throw StateError('chromaprint_get_fingerprint failed');
        }
        final cstr = outPtr.value;
        final compressed = cstr.toDartString();
        cp.dealloc(cstr.cast());
        final digest = crypto.sha256.convert(utf8.encode(compressed));
        return FingerprintResult(
          compressed:      compressed,
          fingerprintHash: digest.toString(),
          sampleRate:      sampleRate,
          channelCount:    channelCount,
          pcmSamples:      samples,
        );
      } finally {
        malloc.free(outPtr);
      }
    } finally {
      cp.destroy(ctx);
      malloc.free(native);
    }
  }

}
