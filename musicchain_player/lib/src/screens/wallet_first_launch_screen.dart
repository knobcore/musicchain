// Three-step wallet setup that runs on first launch (or whenever the
// player can't find a stored wallet on disk):
//
//   1. Choose between "create new" and "restore from existing seed."
//   2. (Create path) Display a freshly-generated 12-word mnemonic; user
//      acknowledges they've written it down before continuing.
//   3. Pick a username (optional) + a local-only unlock password. The
//      keypair is derived from the BIP39 seed, persisted to disk
//      encrypted under the password, and the mnemonic itself is
//      stashed in the platform keyring for one-tap auto-unlock on
//      subsequent app launches.
//
// On completion, calls the onComplete callback so the host
// (HomeScreen / login router) can swap to the main app surface.

import 'dart:convert';
import 'dart:io';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:provider/provider.dart';

import '../services/librats_discovery.dart';
import '../services/wallet_service.dart';

class WalletFirstLaunchScreen extends StatefulWidget {
  final WalletService walletService;
  final VoidCallback onComplete;

  const WalletFirstLaunchScreen({
    super.key,
    required this.walletService,
    required this.onComplete,
  });

  @override
  State<WalletFirstLaunchScreen> createState() => _WalletFirstLaunchScreenState();
}

enum _Step { chooseFlow, showSeed, restoreSeed, pickIdentity }

class _WalletFirstLaunchScreenState extends State<WalletFirstLaunchScreen> {
  _Step _step = _Step.chooseFlow;
  String? _generatedMnemonic;
  final _restoreController  = TextEditingController();
  final _usernameController = TextEditingController();
  final _passwordController = TextEditingController();
  bool _busy = false;
  String? _error;

  @override
  void dispose() {
    _restoreController.dispose();
    _usernameController.dispose();
    _passwordController.dispose();
    super.dispose();
  }

  void _startCreate() {
    final mnemonic = widget.walletService.generateMnemonic();
    if (mnemonic == null || mnemonic.isEmpty) {
      setState(() => _error = "Couldn't generate a recovery phrase. Try again.");
      return;
    }
    setState(() {
      _generatedMnemonic = mnemonic;
      _error             = null;
      _step              = _Step.showSeed;
    });
  }

  void _startRestore() {
    setState(() {
      _generatedMnemonic = null;
      _error             = null;
      _step              = _Step.restoreSeed;
    });
  }

  void _proceedToIdentityFromCreate() {
    // We already generated the mnemonic; just advance.
    setState(() => _step = _Step.pickIdentity);
  }

  void _proceedToIdentityFromRestore() {
    final entered = _restoreController.text.trim().toLowerCase();
    if (!widget.walletService.validateMnemonic(entered)) {
      setState(() => _error =
          'That phrase isn\'t a valid 12-word BIP39 mnemonic. '
          'Check for typos.');
      return;
    }
    setState(() {
      _generatedMnemonic = entered;
      _error             = null;
      _step              = _Step.pickIdentity;
    });
  }

  Future<void> _saveMnemonicToFile() async {
    final mnemonic = _generatedMnemonic;
    if (mnemonic == null || mnemonic.isEmpty) return;
    final body = '$mnemonic\n\n'
        'Keep this file offline. Anyone with these 12 words can spend '
        'from your wallet.\n';
    try {
      // On Android/iOS, file_picker's saveFile uses the `bytes`
      // parameter to write through the system file picker (the app
      // can't reach the chosen path directly from its sandbox). On
      // desktop the picker just returns the chosen path and we write
      // ourselves. Belt-and-braces: pass bytes AND fall back to a
      // manual File write — on desktop the manual write is the real
      // one; on mobile it'll throw a permission error which we eat.
      final pickedPath = await FilePicker.platform.saveFile(
        dialogTitle: 'Save recovery phrase',
        fileName:    'musicchain-recovery-phrase.txt',
        bytes:       Uint8List.fromList(utf8.encode(body)),
      );
      if (pickedPath == null) return; // user cancelled
      try {
        await File(pickedPath).writeAsString(body, flush: true);
      } catch (_) {
        // Mobile sandbox — the bytes path handled the actual write.
      }
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
        content: Text('Saved to $pickedPath'),
        duration: const Duration(seconds: 4),
      ));
    } catch (e) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
        content: Text('Save failed: $e'),
        duration: const Duration(seconds: 4),
      ));
    }
  }

  Future<void> _finish() async {
    final mnemonic = _generatedMnemonic;
    final password = _passwordController.text;
    final username = _usernameController.text.trim();
    if (mnemonic == null || mnemonic.isEmpty) {
      setState(() => _error = 'No mnemonic available. Restart the wallet setup.');
      return;
    }
    if (password.length < 6) {
      setState(() => _error = 'Pick an unlock password of at least 6 characters.');
      return;
    }
    setState(() {
      _busy = true;
      _error = null;
    });
    try {
      await widget.walletService.createWalletFromMnemonic(
        mnemonic: mnemonic,
        password: password,
        username: username,
      );
      widget.onComplete();
    } catch (e) {
      setState(() {
        _busy  = false;
        _error = 'Wallet setup failed: $e';
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    // Watch the discovery layer so the connection banner — and the
    // pickIdentity-step "Finish setup" gate — re-render every time the
    // mini-node handshake or the auto-selected full node changes.
    final disc = context.watch<LibratsDiscovery>();
    return Scaffold(
      appBar: AppBar(title: const Text('Set up your wallet')),
      body: SafeArea(
        child: SingleChildScrollView(
          padding: const EdgeInsets.all(16),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              _buildConnectionBanner(disc),
              switch (_step) {
                _Step.chooseFlow    => _buildChooseFlow(),
                _Step.showSeed      => _buildShowSeed(),
                _Step.restoreSeed   => _buildRestoreSeed(),
                _Step.pickIdentity  => _buildPickIdentity(connected: _hasFullNode(disc)),
              },
            ],
          ),
        ),
      ),
    );
  }

  /// True once the discovery layer has handshaken with the mini-node
  /// AND received at least one full node in the routes.get response.
  /// We require both because the wallet's first publish-fingerprint
  /// call after setup hits the full node directly.
  bool _hasFullNode(LibratsDiscovery disc) =>
      disc.autoSelectedRatsPeerId.isNotEmpty;

  /// Banner shown on every step. Reads green when a full-node handshake
  /// is live, amber while the player is still discovering, red if the
  /// VPS rendezvous threw an error. Same widget across all four steps
  /// so the user has constant feedback throughout the setup flow.
  Widget _buildConnectionBanner(LibratsDiscovery disc) {
    final hasNode  = _hasFullNode(disc);
    final hasError = disc.lastError.isNotEmpty;
    final Color color;
    final IconData icon;
    final String text;
    if (hasNode) {
      color = Colors.green.shade700;
      icon  = Icons.check_circle;
      text  = 'Connected to full node '
              '${disc.autoSelectedRatsPeerId.substring(
                  0, disc.autoSelectedRatsPeerId.length < 10
                        ? disc.autoSelectedRatsPeerId.length : 10)}…'
              '${disc.autoSelectedReachability == 'direct'
                    ? '  (direct)'
                    : '  (via VPS)'}';
    } else if (hasError) {
      color = Colors.red.shade700;
      icon  = Icons.error_outline;
      text  = 'Network error: ${disc.lastError}';
    } else {
      color = Colors.orange.shade700;
      icon  = Icons.sync;
      text  = disc.vpsStatus.isEmpty
              ? 'Connecting to the musicchain mesh…'
              : disc.vpsStatus;
    }
    return Container(
      margin: const EdgeInsets.only(bottom: 16),
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
      decoration: BoxDecoration(
        // ignore: deprecated_member_use
        color: color.withOpacity(0.08),
        border: Border.all(color: color),
        borderRadius: BorderRadius.circular(8),
      ),
      child: Row(
        children: [
          Icon(icon, color: color, size: 20),
          const SizedBox(width: 10),
          Expanded(
            child: Text(text,
                style: TextStyle(
                  color: color,
                  fontSize: 13,
                  fontWeight: FontWeight.w500,
                )),
          ),
        ],
      ),
    );
  }

  Widget _buildChooseFlow() {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        const SizedBox(height: 24),
        const Text(
          'Welcome to musicchain.',
          style: TextStyle(fontSize: 22, fontWeight: FontWeight.w600),
        ),
        const SizedBox(height: 12),
        const Text(
          'Your wallet is your identity here — you sign in to other devices '
          'with a 12-word recovery phrase, and you can pick a username to be '
          'discoverable to other people. Choose one of:',
        ),
        const SizedBox(height: 24),
        ElevatedButton.icon(
          onPressed: _startCreate,
          icon: const Icon(Icons.add_circle_outline),
          label: const Text('Create a new wallet'),
        ),
        const SizedBox(height: 12),
        OutlinedButton.icon(
          onPressed: _startRestore,
          icon: const Icon(Icons.restore),
          label: const Text('Restore from a recovery phrase'),
        ),
        if (_error != null) ...[
          const SizedBox(height: 16),
          Text(_error!, style: const TextStyle(color: Colors.redAccent)),
        ],
      ],
    );
  }

  Widget _buildShowSeed() {
    final words = _generatedMnemonic!.split(' ');
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const Text(
          'Write these 12 words down in order',
          style: TextStyle(fontSize: 18, fontWeight: FontWeight.w600),
        ),
        const SizedBox(height: 8),
        const Text(
          'This is the only way to recover your wallet if you lose this '
          'device. Don\'t screenshot it — keep it offline.',
        ),
        const SizedBox(height: 16),
        Container(
          padding: const EdgeInsets.all(12),
          decoration: BoxDecoration(
            color: Colors.black12,
            borderRadius: BorderRadius.circular(8),
          ),
          child: Wrap(
            spacing: 8,
            runSpacing: 8,
            children: [
              for (var i = 0; i < words.length; i++)
                Chip(
                  label: Text('${i + 1}. ${words[i]}'),
                ),
            ],
          ),
        ),
        const SizedBox(height: 12),
        Row(
          children: [
            Expanded(
              child: TextButton.icon(
                onPressed: () {
                  Clipboard.setData(ClipboardData(text: _generatedMnemonic!));
                  ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
                    content: Text('Copied to clipboard. Paste somewhere safe.'),
                    duration: Duration(seconds: 3),
                  ));
                },
                icon: const Icon(Icons.copy),
                label: const Text('Copy to clipboard'),
              ),
            ),
            Expanded(
              child: TextButton.icon(
                onPressed: _saveMnemonicToFile,
                icon: const Icon(Icons.save_alt),
                label: const Text('Save to a file'),
              ),
            ),
          ],
        ),
        const SizedBox(height: 24),
        ElevatedButton(
          onPressed: _proceedToIdentityFromCreate,
          child: const Text('I\'ve written it down'),
        ),
      ],
    );
  }

  Widget _buildRestoreSeed() {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const Text(
          'Restore from your recovery phrase',
          style: TextStyle(fontSize: 18, fontWeight: FontWeight.w600),
        ),
        const SizedBox(height: 8),
        const Text(
          'Type or paste the 12 words you wrote down when you first set up '
          'this wallet. We check the checksum locally before doing anything.',
        ),
        const SizedBox(height: 16),
        TextField(
          controller: _restoreController,
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
        const SizedBox(height: 24),
        ElevatedButton(
          onPressed: _proceedToIdentityFromRestore,
          child: const Text('Continue'),
        ),
      ],
    );
  }

  Widget _buildPickIdentity({required bool connected}) {
    // Gate Finish on having a live full-node handshake. The first
    // operation right after _finish (username register + initial
    // library announce) needs to reach a full node; running it before
    // discovery completes drops every tx on the floor.
    final canFinish = !_busy && connected;
    final String buttonLabel;
    if (_busy) {
      buttonLabel = 'Setting up…';
    } else if (!connected) {
      buttonLabel = 'Waiting for full node connection…';
    } else {
      buttonLabel = 'Finish setup';
    }
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const Text(
          'Pick a username and an unlock password',
          style: TextStyle(fontSize: 18, fontWeight: FontWeight.w600),
        ),
        const SizedBox(height: 8),
        const Text(
          'Username is optional — leave blank to be visible only by your '
          'wallet address. The unlock password is local; it encrypts the '
          'wallet file on this device so a thief who gets your unlocked '
          'phone can\'t spend.',
        ),
        const SizedBox(height: 24),
        TextField(
          controller: _usernameController,
          autocorrect: false,
          decoration: const InputDecoration(
            border: OutlineInputBorder(),
            labelText: 'Username (3–30 chars, a-z 0-9 _, optional)',
          ),
        ),
        const SizedBox(height: 12),
        TextField(
          controller: _passwordController,
          obscureText: true,
          decoration: const InputDecoration(
            border: OutlineInputBorder(),
            labelText: 'Unlock password (min 6 chars)',
          ),
        ),
        if (_error != null) ...[
          const SizedBox(height: 12),
          Text(_error!, style: const TextStyle(color: Colors.redAccent)),
        ],
        const SizedBox(height: 24),
        ElevatedButton(
          onPressed: canFinish ? _finish : null,
          child: _busy
              ? const SizedBox(
                  height: 20,
                  width:  20,
                  child:  CircularProgressIndicator(strokeWidth: 2),
                )
              : Text(buttonLabel),
        ),
      ],
    );
  }
}
