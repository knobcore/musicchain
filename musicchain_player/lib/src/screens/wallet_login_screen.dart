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

import '../services/wallet_service.dart';
import 'wallet_first_launch_screen.dart';

class WalletLoginScreen extends StatefulWidget {
  final WalletService walletService;
  final VoidCallback onLoggedIn;

  const WalletLoginScreen({
    super.key,
    required this.walletService,
    required this.onLoggedIn,
  });

  @override
  State<WalletLoginScreen> createState() => _WalletLoginScreenState();
}

class _WalletLoginScreenState extends State<WalletLoginScreen> {
  final _passwordController = TextEditingController();
  final _mnemonicController = TextEditingController();
  String _savedUsername = '';
  bool _busy = false;
  String? _error;
  bool _showMnemonicPath = false;

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
    _passwordController.dispose();
    _mnemonicController.dispose();
    super.dispose();
  }

  Future<void> _loginWithPassword() async {
    setState(() {
      _busy = true;
      _error = null;
    });
    try {
      final info = await widget.walletService.loadWallet(_passwordController.text);
      if (info == null) {
        throw Exception('Wrong password — try again or use your recovery phrase.');
      }
      widget.onLoggedIn();
    } catch (e) {
      if (mounted) {
        setState(() {
          _busy = false;
          _error = e.toString().replaceFirst('Exception: ', '');
        });
      }
    }
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
      // Same call path as first-launch restore: derive the keypair from
      // the mnemonic, persist under a fresh local password. We default
      // the password to the mnemonic's first word for now so the user
      // doesn't get a third prompt — they can change it in settings.
      final firstWord = mnemonic.split(' ').first;
      await widget.walletService.createWalletFromMnemonic(
        mnemonic: mnemonic,
        password: firstWord, // see comment above; fine for MVP
      );
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
    Navigator.of(context).pushReplacement(MaterialPageRoute(
      builder: (_) => WalletFirstLaunchScreen(
        walletService: widget.walletService,
        onComplete: widget.onLoggedIn,
      ),
    ));
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Log in')),
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: SingleChildScrollView(
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
              if (!_showMnemonicPath) ...[
                const Text(
                  'Unlock with your password',
                  style: TextStyle(fontWeight: FontWeight.w600),
                ),
                const SizedBox(height: 8),
                TextField(
                  controller: _passwordController,
                  obscureText: true,
                  onSubmitted: (_) => _loginWithPassword(),
                  decoration: const InputDecoration(
                    border: OutlineInputBorder(),
                    labelText: 'Password',
                  ),
                ),
                if (_error != null) ...[
                  const SizedBox(height: 12),
                  Text(_error!, style: const TextStyle(color: Colors.redAccent)),
                ],
                const SizedBox(height: 16),
                ElevatedButton(
                  onPressed: _busy ? null : _loginWithPassword,
                  child: _busy
                      ? const CircularProgressIndicator()
                      : const Text('Unlock'),
                ),
                const SizedBox(height: 16),
                TextButton(
                  onPressed: () =>
                      setState(() => _showMnemonicPath = true),
                  child: const Text(
                      'Use my 12-word recovery phrase instead'),
                ),
              ] else ...[
                const Text(
                  'Restore from recovery phrase',
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
                      ? const CircularProgressIndicator()
                      : const Text('Restore'),
                ),
                const SizedBox(height: 16),
                TextButton(
                  onPressed: () =>
                      setState(() => _showMnemonicPath = false),
                  child: const Text('Back to password'),
                ),
              ],
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
