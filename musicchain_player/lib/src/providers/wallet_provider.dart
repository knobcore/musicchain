import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/foundation.dart';

import '../models/wallet.dart';
import '../services/wallet_service.dart';
import '../services/node_client.dart';
import '../services/node_service.dart';

class WalletProvider extends ChangeNotifier {
  WalletProvider() {
    // Fire-and-forget: pulls the password we cached in secure storage
    // (Keychain / KeyStore / DPAPI) at create/load/import time and
    // re-opens the on-disk wallet so the user doesn't have to re-enter
    // a password every cold start. tryAutoLoad returns null when no
    // saved wallet exists, which keeps the no-wallet UI as-is.
    _tryAutoLoad();
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

  Future<void> _tryAutoLoad() async {
    try {
      final info = await _service.tryAutoLoad();
      if (info != null) {
        _info = info;
        notifyListeners();
        unawaited(refreshBalance());
      }
    } catch (_) { /* no saved wallet or stale keyring entry — ignore */ }
  }

  // Try to load existing wallet on startup
  Future<void> tryLoadWallet(String password) async {
    _loading = true;
    notifyListeners();
    try {
      _info  = await _service.loadWallet(password);
      _error = null;
      if (_info != null) await refreshBalance();
    } catch (e) {
      _error = e.toString();
    }
    _loading = false;
    notifyListeners();
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

      final msg = Uint8List(60);
      writeU32LE(msg, 0, mcChainId);
      msg.setRange(4,  24, hexToBytes(_info!.address));
      msg.setRange(24, 44, hexToBytes(toAddress));
      writeU64LE(msg, 44, amount);
      writeU64LE(msg, 52, nonce);

      // Sign via FFI (mc_wallet_sign hashes internally then ECDSA signs)
      final sig = _service.sign(msg);

      await client.submitTransfer(
        fromAddress: _info!.address,
        toAddress:   toAddress,
        amountStr:   amountStr,
        signature:   sig,
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
