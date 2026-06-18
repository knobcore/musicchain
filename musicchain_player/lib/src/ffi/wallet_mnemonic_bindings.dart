// Hand-written FFI bindings for the BIP39 + EIP-55 wallet exports
// added to include/musicchain.h after the generated bindings.dart was
// last regenerated. Mirrors the C signatures:
//
//   char*       mc_bip39_generate_12(void);
//   int         mc_bip39_validate(const char* mnemonic);
//   mc_wallet_t mc_wallet_from_mnemonic(const char* mnemonic,
//                                       const char* passphrase);
//   char*       mc_wallet_get_eth_address(mc_wallet_t wallet);
//
// Keeping this in a separate file means a future ffigen regeneration
// of bindings.dart doesn't clobber these or vice versa.

import 'dart:ffi';
import 'package:ffi/ffi.dart';

import 'native_library.dart';

typedef _CharRet = Pointer<Utf8> Function();
typedef _IntFromCharPtr = Int Function(Pointer<Utf8>);
typedef _DartIntFromCharPtr = int Function(Pointer<Utf8>);
typedef _WalletFromTwoCharPtrs =
    Pointer<Void> Function(Pointer<Utf8>, Pointer<Utf8>);
typedef _CharRetFromWalletPtr = Pointer<Utf8> Function(Pointer<Void>);

class WalletMnemonicBindings {
  static final _lib = NativeLibrary.bindings;

  /// Generate a fresh 12-word BIP39 mnemonic. Caller frees with mc_free.
  static String? bip39Generate12() {
    final fn = _resolve<_CharRet, _CharRet>('mc_bip39_generate_12');
    if (fn == null) return null;
    final ptr = fn();
    if (ptr.address == 0) return null;
    final s = ptr.toDartString();
    _mcFree(ptr.cast());
    return s;
  }

  /// Returns true if the mnemonic passes BIP39 wordlist + checksum.
  static bool bip39Validate(String mnemonic) {
    final fn = _resolve<_IntFromCharPtr, _DartIntFromCharPtr>('mc_bip39_validate');
    if (fn == null) return false;
    final ptr = mnemonic.toNativeUtf8();
    try {
      return fn(ptr) == 1;
    } finally {
      calloc.free(ptr);
    }
  }

  /// Derive a wallet handle from a (mnemonic, passphrase) pair.
  /// passphrase may be empty.
  static Pointer<Void>? walletFromMnemonic(String mnemonic,
                                            {String passphrase = ''}) {
    final fn = _resolve<_WalletFromTwoCharPtrs, _WalletFromTwoCharPtrs>(
        'mc_wallet_from_mnemonic');
    if (fn == null) return null;
    final mnemonicPtr = mnemonic.toNativeUtf8();
    final passphrasePtr = passphrase.toNativeUtf8();
    try {
      final handle = fn(mnemonicPtr, passphrasePtr);
      if (handle.address == 0) return null;
      return handle;
    } finally {
      calloc.free(mnemonicPtr);
      calloc.free(passphrasePtr);
    }
  }

  /// 0x-prefixed lowercase 40-hex Ethereum / Base address derived from
  /// the same secp256k1 key the wallet already holds. Caller does NOT
  /// free — we handle that internally.
  static String? walletGetEthAddress(Pointer<Void> wallet) {
    final fn = _resolve<_CharRetFromWalletPtr, _CharRetFromWalletPtr>(
        'mc_wallet_get_eth_address');
    if (fn == null) return null;
    final ptr = fn(wallet);
    if (ptr.address == 0) return null;
    final s = ptr.toDartString();
    _mcFree(ptr.cast());
    return s;
  }

  // ---- Internals --------------------------------------------------

  /// Returns null if the symbol isn't exported by the loaded DLL — lets
  /// the app degrade gracefully when running against an older mc_rats
  /// build that pre-dates these exports.
  static T? _resolve<N extends Function, T extends Function>(String name) {
    try {
      final lookup = NativeLibrary.lib;
      final ptr = lookup.lookup<NativeFunction<N>>(name);
      return ptr.asFunction<T>();
    } catch (_) {
      return null;
    }
  }

  static void _mcFree(Pointer<Void> p) {
    if (p.address != 0) NativeLibrary.bindings.mc_free(p);
  }
}
