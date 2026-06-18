import 'dart:ffi';
import 'dart:typed_data';
import 'package:ffi/ffi.dart';
import 'package:flutter_secure_storage/flutter_secure_storage.dart';
import 'package:path_provider/path_provider.dart';
import 'dart:io';

import '../ffi/native_library.dart';
import '../ffi/wallet_mnemonic_bindings.dart';
import '../models/wallet.dart';

class WalletService {
  static const _secureStorage = FlutterSecureStorage();
  static const _walletPathKey     = 'mc_wallet_path';
  // Stored alongside the path so the next app launch can auto-load
  // without prompting. flutter_secure_storage backs onto Keychain on
  // iOS / KeyStore on Android / DPAPI on Windows, so the password
  // doesn't sit in plain prefs.
  static const _walletPasswordKey = 'mc_wallet_password';
  // The 12-word BIP39 mnemonic. Stored encrypted via the platform
  // keyring (same backing as the password) so a device wipe doesn't
  // leave it recoverable from a cold disk image.
  static const _walletMnemonicKey = 'mc_wallet_mnemonic';
  // Username the user picked at wallet creation. Cached locally so the
  // login screen can pre-fill it; the canonical record is on chain.
  static const _walletUsernameKey = 'mc_wallet_username';

  Pointer<Void>? _walletHandle;
  WalletInfo? _cachedInfo;

  bool get hasWallet => _walletHandle != null;
  WalletInfo? get info => _cachedInfo;

  /// True when a wallet file exists on disk (regardless of whether we've
  /// loaded it into memory yet). Used by the auto-load path to decide
  /// whether prompting the user for a password is even necessary.
  Future<bool> hasSavedWallet() async {
    return (await _secureStorage.read(key: _walletPathKey)) != null;
  }

  /// Auto-load the persisted wallet using the password we cached in
  /// secure storage at create/import/load time. Returns null when there
  /// is no saved wallet (first run) or when the keyring entry is
  /// missing — caller falls back to the password prompt in that case.
  Future<WalletInfo?> tryAutoLoad() async {
    final path = await _secureStorage.read(key: _walletPathKey);
    final pw   = await _secureStorage.read(key: _walletPasswordKey);
    if (path == null || pw == null) return null;
    return loadWallet(pw);
  }

  // Load wallet from secure storage
  Future<WalletInfo?> loadWallet(String password) async {
    final pathStr = await _secureStorage.read(key: _walletPathKey);
    if (pathStr == null) return null;

    final pathPtr     = pathStr.toNativeUtf8();
    final passwordPtr = password.toNativeUtf8();
    try {
      final handle = NativeLibrary.bindings
          .mc_wallet_load(pathPtr.cast(), passwordPtr.cast());
      if (handle == nullptr) return null;
      _walletHandle = handle;
      _updateCache();
      // Store the password so the next app launch auto-loads via
      // tryAutoLoad() — the keyring is the only persistent place we
      // keep it, never plain prefs.
      await _secureStorage.write(key: _walletPasswordKey, value: password);
      return _cachedInfo;
    } finally {
      calloc.free(pathPtr);
      calloc.free(passwordPtr);
    }
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
  /// and restore land here because the path is the same: derive
  /// keypair → persist to disk under `password` → cache mnemonic in
  /// the platform keyring for auto-load. `username` is stored locally
  /// for UI pre-fill; the actual on-chain username registration is a
  /// separate call (see `submitUsernameRegister`) so the user can opt
  /// out of being publicly searchable.
  Future<WalletInfo> createWalletFromMnemonic({
    required String mnemonic,
    required String password,
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
    _walletHandle = handle;
    await _saveAndCache(password);
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

  /// Wipe everything we cached for this wallet — keyring entries +
  /// on-disk wallet file. Used by the "Sign out / log into a different
  /// wallet" flow. The chain entries for the wallet (username, level,
  /// etc.) are untouched because they're on-chain.
  Future<void> clearLocalWallet() async {
    freeWallet();
    final pathStr = await _secureStorage.read(key: _walletPathKey);
    if (pathStr != null) {
      final f = File(pathStr);
      if (await f.exists()) await f.delete();
    }
    await _secureStorage.delete(key: _walletPathKey);
    await _secureStorage.delete(key: _walletPasswordKey);
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

  Future<void> _saveAndCache(String password) async {
    final dir = await getApplicationSupportDirectory();
    final path = '${dir.path}/wallet/player.key';
    await Directory('${dir.path}/wallet').create(recursive: true);

    final pathPtr = path.toNativeUtf8();
    NativeLibrary.bindings.mc_wallet_save(_walletHandle!, pathPtr.cast());
    calloc.free(pathPtr);

    await _secureStorage.write(key: _walletPathKey,     value: path);
    await _secureStorage.write(key: _walletPasswordKey, value: password);
    _updateCache();
  }

  void _updateCache() {
    if (_walletHandle == null) return;
    final addrPtr   = NativeLibrary.bindings.mc_wallet_get_address(_walletHandle!);
    final pubkeyPtr = NativeLibrary.bindings.mc_wallet_get_public_key(_walletHandle!);

    final addr   = addrPtr.cast<Utf8>().toDartString();
    final pubkey = pubkeyPtr.cast<Utf8>().toDartString();

    NativeLibrary.bindings.mc_free(addrPtr.cast());
    NativeLibrary.bindings.mc_free(pubkeyPtr.cast());

    _cachedInfo = WalletInfo(address: addr, publicKey: pubkey, balance: '0.00000000');
  }
}
