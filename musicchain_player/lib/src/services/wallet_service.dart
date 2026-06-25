import 'dart:ffi';
import 'dart:typed_data';
import 'package:ffi/ffi.dart';
import 'package:flutter_secure_storage/flutter_secure_storage.dart';

import '../ffi/native_library.dart';
import '../ffi/wallet_mnemonic_bindings.dart';
import '../models/wallet.dart';

class WalletService {
  static const _secureStorage = FlutterSecureStorage();
  // The 12-word BIP39 mnemonic. The recovery secret lives ONLY here —
  // platform secure storage (Keychain / KeyStore / DPAPI). The legacy
  // disk-save path that wrote the 32 priv-key bytes to a plain file
  // was removed because the file sat right next to the mnemonic-
  // protected entry and could be read by any process with data-dir
  // access. On launch we rederive via mc_wallet_from_mnemonic.
  static const _walletMnemonicKey = 'mc_wallet_mnemonic';
  // Username the user picked at wallet creation. Cached locally so the
  // login screen can pre-fill it; the canonical record is on chain.
  static const _walletUsernameKey = 'mc_wallet_username';

  Pointer<Void>? _walletHandle;
  WalletInfo? _cachedInfo;

  bool get hasWallet => _walletHandle != null;
  WalletInfo? get info => _cachedInfo;

  /// True when a mnemonic exists in platform secure storage. Used by
  /// the boot path to decide whether to show first-launch UI.
  Future<bool> hasSavedWallet() async {
    return (await _secureStorage.read(key: _walletMnemonicKey)) != null;
  }

  /// Reconstruct the wallet handle from the stored BIP39 mnemonic.
  /// Returns null on first launch (no saved mnemonic). Idempotent.
  Future<WalletInfo?> tryAutoLoad() async {
    final mnemonic = await _secureStorage.read(key: _walletMnemonicKey);
    if (mnemonic == null || mnemonic.isEmpty) return null;
    final handle = WalletMnemonicBindings.walletFromMnemonic(mnemonic);
    if (handle == null) return null;
    _walletHandle = handle;
    _updateCache();
    return _cachedInfo;
  }

  // ---- BIP39 mnemonic creation / restore ------------------------
  //
  // The Dart side calls into mc_bip39_generate_12 / mc_bip39_validate /
  // mc_wallet_from_mnemonic which live in musicchain.dll. The same
  // libwally-pedigree key derivation runs on the home node so the
  // address shown on chain matches the address shown in the player.

  /// Returns a fresh 12-word English BIP39 mnemonic. The caller
  /// presents this to the user on the first-launch backup screen.
  /// Does NOT persist anything — call createWalletFromMnemonic next.
  String? generateMnemonic() {
    return WalletMnemonicBindings.bip39Generate12();
  }

  /// Validate a typed-in mnemonic against the BIP39 wordlist +
  /// checksum. Used by the login / restore screen before attempting
  /// derivation.
  bool validateMnemonic(String mnemonic) {
    return WalletMnemonicBindings.bip39Validate(mnemonic.trim());
  }

  /// Build (or restore) a wallet from a 12-word mnemonic. Both create
  /// and restore land here. The mnemonic alone is the recovery secret;
  /// it's persisted in platform secure storage and the libwally
  /// derivation runs fresh on every launch. `username` is stored
  /// locally for UI pre-fill; the on-chain registration is a separate
  /// call (see submitUsernameRegister).
  Future<WalletInfo> createWalletFromMnemonic({
    required String mnemonic,
    String username = '',
  }) async {
    final cleaned = mnemonic.trim().toLowerCase();
    if (!WalletMnemonicBindings.bip39Validate(cleaned)) {
      throw Exception('Invalid BIP39 mnemonic');
    }
    final handle = WalletMnemonicBindings.walletFromMnemonic(cleaned);
    if (handle == null) {
      throw Exception('Failed to derive wallet from mnemonic');
    }
    // Re-import path: free the old native handle before we drop the
    // pointer, otherwise the previous wallet's libwally context leaks.
    freeWallet();
    _walletHandle = handle;
    _updateCache();
    await _secureStorage.write(key: _walletMnemonicKey, value: cleaned);
    if (username.isNotEmpty) {
      await _secureStorage.write(key: _walletUsernameKey, value: username);
    }
    return _cachedInfo!;
  }

  /// Look up the locally-cached mnemonic for the current wallet, used
  /// by the "Show recovery phrase" UI in settings. Returns null if the
  /// user imported via raw private key (no mnemonic to show).
  Future<String?> readSavedMnemonic() async {
    return _secureStorage.read(key: _walletMnemonicKey);
  }

  /// Username the user picked at wallet creation. Returns empty string
  /// if not set. The CANONICAL source is the chain; this is a local
  /// cache for UI pre-fill on the login screen.
  Future<String> readSavedUsername() async {
    return (await _secureStorage.read(key: _walletUsernameKey)) ?? '';
  }

  /// Wipe everything we cached for this wallet — the mnemonic plus
  /// the username display cache. Used by the "Sign out / log into a
  /// different wallet" flow. The chain entries for the wallet
  /// (username, level, etc.) are untouched because they're on-chain.
  Future<void> clearLocalWallet() async {
    freeWallet();
    await _secureStorage.delete(key: _walletMnemonicKey);
    await _secureStorage.delete(key: _walletUsernameKey);
  }

  /// Returns the EIP-55-checksummed 0x-prefixed Base address derived
  /// from the same secp256k1 key the wallet holds — same key, different
  /// derivation. Used by the bridge / external transfer UI.
  String? ethAddress() {
    if (_walletHandle == null) return null;
    return WalletMnemonicBindings.walletGetEthAddress(_walletHandle!);
  }

  // Sign data, returns hex signature
  String sign(Uint8List data) {
    if (_walletHandle == null) throw Exception('No wallet loaded');
    // Never hand the native signer a zero-length buffer — some signers
    // touch data[0] and would over-read a 0-byte allocation (native SIGSEGV).
    if (data.isEmpty) throw Exception('sign: empty data');
    final dataPtr = malloc.allocate<Uint8>(data.length);
    dataPtr.asTypedList(data.length).setAll(0, data);
    final sigPtr = NativeLibrary.bindings
        .mc_wallet_sign(_walletHandle!, dataPtr, data.length);
    malloc.free(dataPtr);
    if (sigPtr == nullptr) throw Exception('Signing failed');
    final sig = sigPtr.cast<Utf8>().toDartString();
    NativeLibrary.bindings.mc_free(sigPtr.cast());
    return sig;
  }

  void freeWallet() {
    if (_walletHandle != null) {
      NativeLibrary.bindings.mc_wallet_free(_walletHandle!);
      _walletHandle = null;
      _cachedInfo   = null;
    }
  }

  // ---- Internal -------------------------------------------------------

  void _updateCache() {
    if (_walletHandle == null) return;
    final addrPtr   = NativeLibrary.bindings.mc_wallet_get_address(_walletHandle!);
    final pubkeyPtr = NativeLibrary.bindings.mc_wallet_get_public_key(_walletHandle!);

    // NULL-check BEFORE toDartString — on a null return toDartString() walks
    // memory from address 0 hunting a NUL terminator → native SIGSEGV (a
    // frequent Android crash). Free whichever pointer is non-null and bail
    // (leave _cachedInfo as-is) rather than dereferencing 0x0. Mirrors sign().
    if (addrPtr == nullptr || pubkeyPtr == nullptr) {
      if (addrPtr   != nullptr) NativeLibrary.bindings.mc_free(addrPtr.cast());
      if (pubkeyPtr != nullptr) NativeLibrary.bindings.mc_free(pubkeyPtr.cast());
      return;
    }

    final addr   = addrPtr.cast<Utf8>().toDartString();
    final pubkey = pubkeyPtr.cast<Utf8>().toDartString();

    NativeLibrary.bindings.mc_free(addrPtr.cast());
    NativeLibrary.bindings.mc_free(pubkeyPtr.cast());

    _cachedInfo = WalletInfo(address: addr, publicKey: pubkey, balance: '0.00000000');
  }
}
