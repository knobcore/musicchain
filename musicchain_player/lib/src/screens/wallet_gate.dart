// Routes between the wallet setup screens and the main app surface
// based on what's stored on the device.
//
//   * No wallet on disk → first-launch (create or restore)
//   * Wallet on disk but auto-unlock failed → login (password or
//     re-enter mnemonic)
//   * Wallet unlocked in memory → HomeScreen
//
// We do this at the top of the widget tree (above HomeScreen) so the
// rest of the app can assume there's always a logged-in wallet.

import 'package:flutter/material.dart';

import '../services/wallet_service.dart';
import 'wallet_first_launch_screen.dart';
import 'wallet_login_screen.dart';

enum _GateState { loading, firstLaunch, login, home }

class WalletGate extends StatefulWidget {
  /// The host app's primary surface — shown once the wallet is unlocked.
  final Widget child;

  const WalletGate({super.key, required this.child});

  @override
  State<WalletGate> createState() => _WalletGateState();
}

class _WalletGateState extends State<WalletGate> {
  final _walletService = WalletService();
  _GateState _state = _GateState.loading;

  @override
  void initState() {
    super.initState();
    _decide();
  }

  Future<void> _decide() async {
    // Three-state decision tree:
    //   1. Has a wallet file? If not → first-launch.
    //   2. Does auto-unlock (keychain-stashed password) succeed? If yes
    //      → home.
    //   3. Otherwise → login (user types password or re-enters
    //      mnemonic).
    final hasSaved = await _walletService.hasSavedWallet();
    if (!hasSaved) {
      if (mounted) setState(() => _state = _GateState.firstLaunch);
      return;
    }
    final auto = await _walletService.tryAutoLoad();
    if (auto != null) {
      if (mounted) setState(() => _state = _GateState.home);
      return;
    }
    if (mounted) setState(() => _state = _GateState.login);
  }

  void _onLoggedIn() {
    if (mounted) setState(() => _state = _GateState.home);
  }

  @override
  Widget build(BuildContext context) {
    return switch (_state) {
      _GateState.loading => const Scaffold(
          body: Center(child: CircularProgressIndicator()),
        ),
      _GateState.firstLaunch => WalletFirstLaunchScreen(
          walletService: _walletService,
          onComplete: _onLoggedIn,
        ),
      _GateState.login => WalletLoginScreen(
          walletService: _walletService,
          onLoggedIn: _onLoggedIn,
        ),
      _GateState.home => widget.child,
    };
  }
}
