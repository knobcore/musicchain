// Login surface when:
//
//   * The player has a wallet on disk from a previous run but the
//     in-memory handle is gone (cold start without auto-unlock).
//   * The user explicitly signed out (clearLocalWallet) and wants to
//     log back in either by typing their unlock password OR by entering
//     the 12-word recovery phrase from another device.
//
// The username field is for UX only — we display the cached value as a
// hint, but auth is purely on the password (for the local-disk wallet)
// or on the mnemonic (for cross-device login). The chain-level
// username lookup happens elsewhere when the player wants to resolve
// a friend's handle to an address.

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../providers/wallet_provider.dart';
import '../services/wallet_service.dart';

class WalletLoginScreen extends StatefulWidget {
  final WalletService walletService;
  final VoidCallback onLoggedIn;
  /// Called when the user taps "Sign out and start fresh" after the
  /// stored wallet has been cleared from disk. The gate uses this to
  /// transition `_state` to `firstLaunch` WITHOUT swapping the route
  /// (which would unmount the gate's State and silently drop the
  /// subsequent onLoggedIn callback).
  final VoidCallback onResetWallet;

  const WalletLoginScreen({
    super.key,
    required this.walletService,
    required this.onLoggedIn,
    required this.onResetWallet,
  });

  @override
  State<WalletLoginScreen> createState() => _WalletLoginScreenState();
}

class _WalletLoginScreenState extends State<WalletLoginScreen> {
  final _mnemonicController = TextEditingController();
  String _savedUsername = '';
  bool _busy = false;
  String? _error;

  @override
  void initState() {
    super.initState();
    _loadSavedUsername();
  }

  Future<void> _loadSavedUsername() async {
    final u = await widget.walletService.readSavedUsername();
    if (mounted) setState(() => _savedUsername = u);
  }

  @override
  void dispose() {
    _mnemonicController.dispose();
    super.dispose();
  }

  Future<void> _loginWithMnemonic() async {
    final mnemonic = _mnemonicController.text.trim().toLowerCase();
    if (!widget.walletService.validateMnemonic(mnemonic)) {
      setState(() => _error = 'That\'s not a valid 12-word recovery phrase.');
      return;
    }
    setState(() {
      _busy = true;
      _error = null;
    });
    try {
      // The mnemonic is the only credential. createWalletFromMnemonic
      // derives the keypair via libwally BIP39+BIP32, caches it in
      // memory, and stores the mnemonic in platform secure storage
      // (Keychain / KeyStore / DPAPI) for the next launch to rederive.
      final info = await widget.walletService.createWalletFromMnemonic(
        mnemonic: mnemonic,
      );
      // Push WalletInfo directly into WalletProvider so the wallet
      // tab renders immediately — tryAutoLoad on next cold start
      // rebuilds the same WalletInfo from the saved mnemonic.
      //
      // If the provider lookup or setWallet itself throws (e.g. the
      // provider isn't in the tree, or notifyListeners blows up in a
      // listener), DON'T swallow it and march on to onLoggedIn — the
      // gate would flip to "signed in" with an empty WalletProvider
      // and the wallet tab would render as if there were no wallet,
      // with no error shown anywhere. Surface it as a restore failure
      // instead so the user can retry.
      try {
        if (mounted) context.read<WalletProvider>().setWallet(info);
      } catch (e) {
        if (mounted) {
          setState(() {
            _busy = false;
            _error = 'Restore failed: $e';
          });
        }
        return;
      }
      widget.onLoggedIn();
    } catch (e) {
      if (mounted) {
        setState(() {
          _busy = false;
          _error = 'Restore failed: $e';
        });
      }
    }
  }

  Future<void> _signOut() async {
    await widget.walletService.clearLocalWallet();
    if (!mounted) return;
    // Defer to the gate to swap to the first-launch screen. Using
    // Navigator.pushReplacement here would TEAR DOWN the WalletGate
    // (since the gate IS the home route), so by the time
    // WalletFirstLaunchScreen calls its onComplete the gate's State
    // is already disposed and the setState that should transition us
    // to home gets dropped on the floor. Symptom: the spinner just
    // keeps spinning after Create account.
    widget.onResetWallet();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Sign in')),
      body: SafeArea(
        child: SingleChildScrollView(
          padding: const EdgeInsets.all(16),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              if (_savedUsername.isNotEmpty) ...[
                const SizedBox(height: 16),
                Row(
                  children: [
                    const CircleAvatar(child: Icon(Icons.person)),
                    const SizedBox(width: 12),
                    Expanded(
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Text(
                            _savedUsername,
                            style: const TextStyle(
                              fontSize: 18,
                              fontWeight: FontWeight.w600,
                            ),
                          ),
                          const Text('Stored wallet on this device',
                              style: TextStyle(color: Colors.black54)),
                        ],
                      ),
                    ),
                  ],
                ),
              ],
              const SizedBox(height: 24),
              const Text(
                'Sign in with your recovery phrase',
                style: TextStyle(fontWeight: FontWeight.w600),
              ),
              const SizedBox(height: 8),
              const Text(
                'Type the 12 words from any device that uses the same '
                'wallet. We derive the keypair locally; no part of the '
                'phrase leaves the device.',
              ),
              const SizedBox(height: 12),
              TextField(
                controller: _mnemonicController,
                maxLines: 3,
                autocorrect: false,
                decoration: const InputDecoration(
                  border: OutlineInputBorder(),
                  hintText: 'word1 word2 word3 …',
                ),
              ),
              if (_error != null) ...[
                const SizedBox(height: 12),
                Text(_error!, style: const TextStyle(color: Colors.redAccent)),
              ],
              const SizedBox(height: 16),
              ElevatedButton(
                onPressed: _busy ? null : _loginWithMnemonic,
                child: _busy
                    ? const SizedBox(
                        height: 20,
                        width:  20,
                        child:  CircularProgressIndicator(strokeWidth: 2),
                      )
                    : const Text('Sign in'),
              ),
              const Divider(height: 48),
              TextButton.icon(
                onPressed: _signOut,
                icon: const Icon(Icons.delete_outline,
                    color: Colors.redAccent),
                label: const Text(
                  'Sign out of this device and start fresh',
                  style: TextStyle(color: Colors.redAccent),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
