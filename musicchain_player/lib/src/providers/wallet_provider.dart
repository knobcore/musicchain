import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/foundation.dart';

import '../models/wallet.dart';
import '../services/wallet_service.dart';
import '../services/node_client.dart';
import '../services/node_service.dart';
import '../services/library_scanner.dart';

class WalletProvider extends ChangeNotifier {
  /// Process-wide reference to the active WalletProvider so leaf code
  /// (PlayerProvider.\_completeSessionSilently, offline-play submit, etc.)
  /// can request a balance refresh without dragging BuildContext into
  /// services. Set in the constructor, cleared in dispose.
  static WalletProvider? _active;
  static void refreshNow() {
    final w = _active;
    if (w != null && w._info != null) unawaited(w.refreshBalance());
  }

  /// The active wallet provider (if a wallet is loaded). Lets non-widget
  /// services (e.g. LibraryPublisher) sign with the user's key without a
  /// BuildContext.
  static WalletProvider? get active => _active;

  WalletProvider() {
    _active = this;
    // Fire-and-forget: pulls the BIP39 mnemonic from platform secure
    // storage (Keychain / KeyStore / DPAPI) and rederives the keypair
    // via libwally. Returns null on first run, which keeps the no-
    // wallet UI as-is until first-launch completes.
    _tryAutoLoad();

    // Defensive periodic refresh: the player path that fires
    // session.complete is silent (errors swallowed, no UI feedback),
    // so a successful mint lands on chain without anyone telling
    // the wallet to re-fetch. A 20 s tick catches it within one
    // listening session's worth of plays — invisible to the user
    // beyond the balance digit ticking up.
    _refreshTimer = Timer.periodic(const Duration(seconds: 20), (_) {
      if (_info != null) unawaited(refreshBalance());
    });
  }

  Timer? _refreshTimer;

  @override
  void dispose() {
    _refreshTimer?.cancel();
    if (identical(_active, this)) _active = null;
    super.dispose();
  }

  final WalletService _service = WalletService();
  final NodeClient    _client  = NodeClient();

  Future<NodeClient> _getClient() async {
    final pid = await NodeService.getRatsPeerId();
    if (pid.isEmpty) {
      throw Exception('No node discovered yet. Open Settings to refresh.');
    }
    _client.ratsPeerId = pid;
    return _client;
  }

  WalletInfo? _info;
  bool        _loading = false;
  String?     _error;

  WalletInfo? get info    => _info;
  bool        get loading => _loading;
  String?     get error   => _error;
  bool        get hasWallet => _info != null;

  /// ECDSA-sign arbitrary bytes with the wallet key (secp256k1; the native
  /// signer SHA-256s the data internally). Returns the 64-byte signature as
  /// 128-char hex. Used by LibraryPublisher to sign DB2 library deltas.
  String sign(Uint8List data) => _service.sign(data);

  Future<void> _tryAutoLoad() async {
    try {
      final info = await _service.tryAutoLoad();
      if (info != null) {
        _info = info;
        LibraryScanner.artistAddress = info.address;
        notifyListeners();
        unawaited(refreshBalance());
      }
    } catch (_) { /* no saved wallet or stale keyring entry — ignore */ }
  }

  /// Re-run the auto-load path. Used after the wallet first-launch
  /// flow finishes — this provider was constructed at app startup
  /// before the mnemonic was written to secure storage, so `_info`
  /// is still null. Idempotent: safe to call at any time.
  Future<void> reload() async {
    await _tryAutoLoad();
  }

  /// Adopt a freshly-derived WalletInfo directly. The create/login
  /// screens already hold the keypair in memory after calling
  /// createWalletFromMnemonic; pushing it here lets the wallet tab
  /// render immediately while the next launch will autoload from the
  /// stored mnemonic.
  void setWallet(WalletInfo info) {
    _info  = info;
    _error = null;
    LibraryScanner.artistAddress = info.address;
    notifyListeners();
    unawaited(refreshBalance());
  }


  Future<void> refreshBalance() async {
    if (_info == null) return;
    try {
      final bal = await (await _getClient()).getBalance(_info!.address);
      _info = WalletInfo(
        address:   _info!.address,
        publicKey: _info!.publicKey,
        balance:   bal,
      );
      notifyListeners();
    } catch (_) {}
  }

  /// Send tokens to another address.
  /// Returns null on success, or an error string on failure.
  Future<String?> sendTokens(String toAddress, String amountStr) async {
    if (_info == null) return 'No wallet loaded';
    try {
      final client = await _getClient();
      final nonce = await client.getWalletNonce(_info!.address);

      // Parse decimal amount string → internal units (8 decimals)
      final parts = amountStr.split('.');
      final whole = int.parse(parts[0]);
      final frac  = (parts.length > 1 ? parts[1] : '')
          .padRight(8, '0')
          .substring(0, 8);
      final amount = whole * 100000000 + int.parse(frac);
      if (amount <= 0) return 'Amount must be greater than zero';

      // Build 60-byte sign message:
      //   chain_id(4 LE) | from(20) | to(20) | amount(8 LE) | nonce(8 LE)
      //
      // Must match TransferTx::sign_message() in
      // musicchain/src/core/transaction.cpp — EIP-155-style chain_id is
      // mixed in first so a signature can't replay on Ethereum /
      // BSC / Base / any other chain. MC_CHAIN_ID = 19779 (0x4D43, "MC").
      const int mcChainId = 19779;
      Uint8List hexToBytes(String hex) {
        final h = hex.replaceAll('0x', '');
        return Uint8List.fromList(List.generate(
          h.length ~/ 2,
          (i) => int.parse(h.substring(i * 2, i * 2 + 2), radix: 16),
        ));
      }
      void writeU32LE(Uint8List buf, int offset, int value) {
        for (int i = 0; i < 4; i++) {
          buf[offset + i] = (value >> (i * 8)) & 0xFF;
        }
      }
      void writeU64LE(Uint8List buf, int offset, int value) {
        for (int i = 0; i < 8; i++) {
          buf[offset + i] = (value >> (i * 8)) & 0xFF;
        }
      }

      // C++ TransferTx::sign_message() appends the 33-byte compressed from_pubkey
      // AFTER the nonce (verify_signature cross-checks the inline pubkey against
      // from_address), so we MUST sign over the same 93 bytes — signing only the
      // first 60 would never verify and the transfer would be unmineable.
      final pubBytes = hexToBytes(_info!.publicKey);   // 33-byte compressed
      final msg = Uint8List(60 + pubBytes.length);
      writeU32LE(msg, 0, mcChainId);
      msg.setRange(4,  24, hexToBytes(_info!.address));
      msg.setRange(24, 44, hexToBytes(toAddress));
      writeU64LE(msg, 44, amount);
      writeU64LE(msg, 52, nonce);
      msg.setRange(60, 60 + pubBytes.length, pubBytes);

      // Sign via FFI (mc_wallet_sign hashes internally then ECDSA signs)
      final sig = _service.sign(msg);

      await client.submitTransfer(
        fromAddress: _info!.address,
        toAddress:   toAddress,
        amountStr:   amountStr,
        signature:   sig,
        fromPubkey:  _info!.publicKey,
        nonce:       nonce,
      );
      await refreshBalance();
      return null;
    } catch (e) {
      return e.toString();
    }
  }

  void freeWallet() {
    _service.freeWallet();
    _info = null;
    notifyListeners();
  }
}
