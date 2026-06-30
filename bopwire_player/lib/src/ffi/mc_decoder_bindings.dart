// Dart FFI binding for the Windows-only `mc_decoder.dll` audio decoder
// (Media Foundation → 16-bit signed PCM). Built from
// `windows/decoder/mc_decoder.cpp`; shipped alongside the player exe.

import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

/// Layout of the C `mc_decoded` struct that the decoder returns. Field
/// offsets are 8-aligned on x64. Keep in sync with mc_decoder.cpp.
final class _McDecodedStruct extends Struct {
  external Pointer<Int16> pcm;
  @Int64()
  external int sampleCount;
  @Int32()
  external int sampleRate;
  @Int32()
  external int channelCount;
}

typedef _NativeOpen  = Int32 Function(Pointer<Utf16>, Pointer<Pointer<_McDecodedStruct>>);
typedef _DartOpen    = int Function(Pointer<Utf16>, Pointer<Pointer<_McDecodedStruct>>);

typedef _NativeFree  = Void Function(Pointer<_McDecodedStruct>);
typedef _DartFree    = void Function(Pointer<_McDecodedStruct>);

class DecodedPcm {
  DecodedPcm({
    required this.pcm,
    required this.sampleRate,
    required this.channelCount,
  });
  final Uint8List pcm;          // interleaved 16-bit LE
  final int       sampleRate;
  final int       channelCount;
}

class MediaFoundationDecoder {
  MediaFoundationDecoder._(this._open, this._free);

  static MediaFoundationDecoder? _instance;
  static MediaFoundationDecoder get instance =>
      _instance ??= _load();

  static MediaFoundationDecoder _load() {
    if (!Platform.isWindows) {
      throw UnsupportedError(
          'MediaFoundationDecoder is Windows-only — use the Android '
          'MethodChannel decoder on mobile.');
    }
    final lib = DynamicLibrary.open('mc_decoder.dll');
    return MediaFoundationDecoder._(
      lib.lookupFunction<_NativeOpen, _DartOpen>('mc_decoder_open'),
      lib.lookupFunction<_NativeFree, _DartFree>('mc_decoder_free'),
    );
  }

  final _DartOpen _open;
  final _DartFree _free;

  /// Decode the file at [path] synchronously. Throws on failure.
  /// The returned PCM buffer is a copy owned by Dart; the native
  /// allocation is freed before this returns.
  DecodedPcm decode(String path) {
    final pathPtr = path.toNativeUtf16();
    final outPtr  = malloc<Pointer<_McDecodedStruct>>();
    try {
      final rc = _open(pathPtr, outPtr);
      if (rc != 0 || outPtr.value.address == 0) {
        throw StateError('mc_decoder_open failed for "$path" (rc=0x'
            '${rc.toUnsigned(32).toRadixString(16)})');
      }
      final s = outPtr.value.ref;
      final byteLen = s.sampleCount * 2;
      final pcm = Uint8List.fromList(
          s.pcm.cast<Uint8>().asTypedList(byteLen));
      final rate     = s.sampleRate;
      final channels = s.channelCount;
      _free(outPtr.value);
      return DecodedPcm(
        pcm:          pcm,
        sampleRate:   rate,
        channelCount: channels,
      );
    } finally {
      malloc.free(pathPtr);
      malloc.free(outPtr);
    }
  }
}
