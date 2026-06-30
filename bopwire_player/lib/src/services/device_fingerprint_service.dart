import 'dart:ffi';
import 'dart:io';
import 'dart:math';

import 'package:ffi/ffi.dart';
import 'package:flutter/services.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../ffi/native_library.dart';

/// Structural device attestation (#5, Axis-A anti-farming). Carries a
/// hardware-derived identifier so the full node's per-device mint limiter
/// (~2,880/device/day) buckets real hardware instead of a resettable random
/// token. This is the "structural / accept + record" tier: the full node's
/// AcceptAllVerifier derives `device_id = sha256(device_key)` and records
/// the `level`, but accepts every request. A future hardware-attested
/// verifier (Play Integrity / TPM) swaps in server-side WITHOUT any client
/// change — it just starts requiring a stronger `level` + a cert chain.
///
/// Sources, by platform:
///   - Windows / Linux / macOS → native `mc_device_fingerprint()` over FFI
///     (MAC + board/CPU/disk serials + OS + MachineGuid), level `desktop_pci`.
///   - Android → `bopwire/device` MethodChannel (ANDROID_ID + Build.*),
///     level `android_hw`.
///   - anything else / read failure → a cached random id, level `software`.
class DeviceAttestation {
  /// Hex of the hardware fingerprint. The server hashes this to the
  /// canonical device_id; we also use it as the bundle's `device_id`.
  final String deviceKey;
  final String level;     // desktop_pci | android_hw | software
  final String platform;  // windows | linux | macos | android | unknown
  const DeviceAttestation(this.deviceKey, this.level, this.platform);

  /// The optional `attestation` object on session.start / the offline
  /// bundle. Matches src/api/device_attestation.h's wire shape; the
  /// extra fields a real verifier needs (cert chain, integrity token,
  /// nonce, device_sig) are absent in the structural tier.
  Map<String, dynamic> toJson() => {
        'platform':   platform,
        'level':      level,
        'device_key': deviceKey,
      };
}

class DeviceFingerprintService {
  DeviceFingerprintService._();
  static final DeviceFingerprintService instance = DeviceFingerprintService._();

  static const MethodChannel _chan = MethodChannel('bopwire/device');

  DeviceAttestation? _cached;

  /// Resolve (and cache for the process lifetime) the device attestation.
  Future<DeviceAttestation> get() async {
    final existing = _cached;
    if (existing != null) return existing;

    String? fp;
    String level;
    String platform;

    if (Platform.isAndroid) {
      platform = 'android';
      try {
        fp = await _chan.invokeMethod<String>('fingerprint');
      } catch (_) {
        fp = null;
      }
      level = _looksHashed(fp) ? 'android_hw' : 'software';
    } else if (Platform.isWindows || Platform.isLinux || Platform.isMacOS) {
      platform = Platform.operatingSystem;
      try {
        fp = _nativeFingerprint();
      } catch (_) {
        fp = null;
      }
      level = _looksHashed(fp) ? 'desktop_pci' : 'software';
    } else {
      platform = 'unknown';
      level = 'software';
    }

    if (!_looksHashed(fp)) {
      // No hardware id available — fall back to a stable per-install random,
      // and mark the weak level so the server can choose to rate-limit
      // software-level devices harder once a real verifier lands.
      fp = await _fallbackRandom();
      level = 'software';
    }

    final att = DeviceAttestation(fp!, level, platform);
    _cached = att;
    return att;
  }

  bool _looksHashed(String? s) => s != null && s.length == 64;

  String? _nativeFingerprint() {
    final ptr = NativeLibrary.bindings.mc_device_fingerprint();
    if (ptr == nullptr) return null;
    try {
      return ptr.cast<Utf8>().toDartString();
    } finally {
      NativeLibrary.bindings.mc_free(ptr.cast());
    }
  }

  Future<String> _fallbackRandom() async {
    final prefs = await SharedPreferences.getInstance();
    final cached = prefs.getString('mc_device_id_hex');
    if (cached != null && cached.length == 64) return cached;
    final rng = Random.secure();
    final hex = List<int>.generate(32, (_) => rng.nextInt(256))
        .map((b) => b.toRadixString(16).padLeft(2, '0'))
        .join();
    await prefs.setString('mc_device_id_hex', hex);
    return hex;
  }
}
