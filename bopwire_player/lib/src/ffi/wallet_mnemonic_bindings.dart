// Hand-written FFI bindings for the BIP39 + EIP-55 wallet exports
// added to include/bopwire.h after the generated bindings.dart was
// last regenerated. Mirrors the C signatures:
//
//   char*       mc_bip39_generate_12(void);
//   int         mc_bip39_validate(const char* mnemonic);
//   mc_wallet_t mc_wallet_from_mnemonic(const char* mnemonic,
//                                       const char* passphrase);
//
// Keeping this in a separate file means a future ffigen regeneration
// of bindings.dart doesn't clobber these or vice versa.
//
// Note on the FFI lookup style below: Dart's `lookup<NativeFunction<F>>`
// requires F to be a concrete, instantiated function type at compile
// time. A generic helper that takes F as a type parameter trips
// G1DE28886. So each symbol resolves itself inline with its concrete
// NativeFunction<...> type.

import 'dart:ffi';
import 'package:ffi/ffi.dart';

import 'native_library.dart';

typedef _CharRetC                = Pointer<Utf8> Function();
typedef _CharRetDart             = Pointer<Utf8> Function();
typedef _IntFromCharPtrC         = Int Function(Pointer<Utf8>);
typedef _IntFromCharPtrDart      = int Function(Pointer<Utf8>);
typedef _WalletFromTwoCharPtrsC  =
    Pointer<Void> Function(Pointer<Utf8>, Pointer<Utf8>);
typedef _WalletFromTwoCharPtrsDart =
    Pointer<Void> Function(Pointer<Utf8>, Pointer<Utf8>);

class WalletMnemonicBindings {
  /// Generate a fresh 12-word BIP39 mnemonic. Caller frees with mc_free.
  static String? bip39Generate12() {
    final fn = _lookupSym<_CharRetC, _CharRetDart>(
        'mc_bip39_generate_12',
        (ptr) => ptr.asFunction<_CharRetDart>());
    if (fn == null) return null;
    final ptr = fn();
    if (ptr.address == 0) return null;
    final s = ptr.toDartString();
    _mcFree(ptr.cast());
    return s;
  }

  /// Returns true if the mnemonic passes BIP39 wordlist + checksum.
  static bool bip39Validate(String mnemonic) {
    final fn = _lookupSym<_IntFromCharPtrC, _IntFromCharPtrDart>(
        'mc_bip39_validate',
        (ptr) => ptr.asFunction<_IntFromCharPtrDart>());
    if (fn == null) return false;
    final p = mnemonic.toNativeUtf8();
    try {
      return fn(p) == 1;
    } finally {
      calloc.free(p);
    }
  }

  /// Derive a wallet handle from a (mnemonic, passphrase) pair.
  /// passphrase may be empty.
  static Pointer<Void>? walletFromMnemonic(String mnemonic,
                                            {String passphrase = ''}) {
    final fn = _lookupSym<_WalletFromTwoCharPtrsC, _WalletFromTwoCharPtrsDart>(
        'mc_wallet_from_mnemonic',
        (ptr) => ptr.asFunction<_WalletFromTwoCharPtrsDart>());
    if (fn == null) return null;
    final m = mnemonic.toNativeUtf8();
    final p = passphrase.toNativeUtf8();
    try {
      final handle = fn(m, p);
      if (handle.address == 0) return null;
      return handle;
    } finally {
      calloc.free(m);
      calloc.free(p);
    }
  }

  // ---- Internals --------------------------------------------------

  /// Look up a symbol with a concrete NativeFunction<NativeT> type and
  /// run the caller-supplied converter to its Dart function shape.
  /// Returns null on any failure so the UI degrades to "feature not
  /// available" rather than crashing.
  ///
  /// NativeT is constrained to Function but Dart will only accept this
  /// helper when callers instantiate it with a concrete NativeFunction
  /// signature at the call site — which is what the wrappers above do.
  static DartT? _lookupSym<NativeT extends Function, DartT extends Function>(
      String name,
      DartT Function(Pointer<NativeFunction<NativeT>>) convert) {
    try {
      final ptr = NativeLibrary.lib.lookup<NativeFunction<NativeT>>(name);
      return convert(ptr);
    } catch (_) {
      return null;
    }
  }

  static void _mcFree(Pointer<Void> p) {
    if (p.address != 0) NativeLibrary.bindings.mc_free(p);
  }
}
