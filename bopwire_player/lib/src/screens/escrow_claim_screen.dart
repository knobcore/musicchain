// Escrow claim flow for artists. Tokens minted from the first 10 000
// plays of each song sit in a per-artist escrow account (see Ledger /
// CandidateManager on the full node). Only a moderator can release
// them, and they need to attach the release to a real human — so this
// screen surfaces the artist's wallet address and lets them push a
// KYC form (PDF / scan / photo of ID) straight to the full node's KYC
// inbox, where the moderator reviews it on the TUI before releasing.

import 'dart:io';
import 'dart:typed_data';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:provider/provider.dart';

import '../providers/wallet_provider.dart';
import '../services/node_client.dart';
import '../services/node_service.dart';

class EscrowClaimScreen extends StatelessWidget {
  const EscrowClaimScreen({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Claim escrow')),
      body: SingleChildScrollView(
        padding: const EdgeInsets.fromLTRB(20, 24, 20, 32),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: const [
            _ExplainerCard(),
            SizedBox(height: 16),
            _AddressCard(),
            SizedBox(height: 16),
            _KycUploadCard(),
          ],
        ),
      ),
    );
  }
}

class _ExplainerCard extends StatelessWidget {
  const _ExplainerCard();
  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Icon(Icons.savings_outlined,
                     color: theme.colorScheme.primary),
                const SizedBox(width: 8),
                Text('Per-artist escrow',
                     style: theme.textTheme.titleMedium),
              ],
            ),
            const SizedBox(height: 8),
            Text(
              "Tokens minted for your songs during the discovery tier "
              "(first 10 000 plays per track) sit in a per-artist "
              "escrow account, releasable only by a moderator. To "
              "claim, share your wallet address below and upload a "
              "KYC form so the moderator can match the form to the "
              "escrow they're about to release.",
              style: theme.textTheme.bodyMedium,
            ),
          ],
        ),
      ),
    );
  }
}

class _AddressCard extends StatelessWidget {
  const _AddressCard();
  @override
  Widget build(BuildContext context) {
    final theme  = Theme.of(context);
    final wallet = context.watch<WalletProvider>().info;
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Icon(Icons.account_balance_wallet_outlined,
                     color: theme.colorScheme.primary),
                const SizedBox(width: 8),
                Text('Wallet address',
                     style: theme.textTheme.titleMedium),
              ],
            ),
            const SizedBox(height: 8),
            if (wallet == null)
              Text(
                'No wallet loaded yet. Open the Wallet tab and create or '
                'import one — it has to be in this app so the moderator '
                'can release escrow into it.',
                style: theme.textTheme.bodyMedium,
              )
            else ...[
              SelectableText(
                wallet.address,
                style: const TextStyle(
                  fontFamily: 'monospace', fontSize: 13),
              ),
              const SizedBox(height: 8),
              Row(
                children: [
                  OutlinedButton.icon(
                    icon: const Icon(Icons.copy, size: 16),
                    label: const Text('Copy'),
                    onPressed: () {
                      Clipboard.setData(ClipboardData(
                          text: wallet.address));
                      ScaffoldMessenger.of(context).showSnackBar(
                        const SnackBar(content: Text(
                            'Wallet address copied')));
                    },
                  ),
                ],
              ),
            ],
          ],
        ),
      ),
    );
  }
}

class _KycUploadCard extends StatefulWidget {
  const _KycUploadCard();
  @override
  State<_KycUploadCard> createState() => _KycUploadCardState();
}

class _KycUploadCardState extends State<_KycUploadCard> {
  bool   _uploading = false;
  String _status    = '';

  Future<void> _pickAndSubmit() async {
    final wallet = context.read<WalletProvider>().info;
    if (wallet == null) {
      _flash('Load or create a wallet first — the moderator needs an '
             'address to release escrow into.');
      return;
    }

    final result = await FilePicker.platform.pickFiles(
      type:              FileType.custom,
      allowedExtensions: const ['pdf', 'jpg', 'jpeg', 'png'],
      withData:          true,
    );
    if (result == null || result.files.isEmpty) return;
    final picked = result.files.single;

    Uint8List? bytes = picked.bytes;
    if (bytes == null && picked.path != null) {
      bytes = await File(picked.path!).readAsBytes();
    }
    if (bytes == null) {
      _flash('Could not read the picked file.');
      return;
    }
    if (bytes.length > 32 * 1024 * 1024) {
      _flash('File is over the 32 MB inbox limit.');
      return;
    }

    setState(() {
      _uploading = true;
      _status    = 'Uploading…';
    });
    try {
      final pid = await NodeService.getRatsPeerId();
      if (pid.isEmpty) {
        throw Exception('No full node discovered yet. Open Settings → '
                         'Refresh nodes and try again.');
      }
      final client   = NodeClient(ratsPeerId: pid);
      final storedAs = await client.submitKycForm(
        filename:    picked.name,
        bytes:       bytes,
        fromAddress: wallet.address,
      );
      if (!mounted) return;
      setState(() => _status = 'Delivered as $storedAs');
      _flash('KYC form delivered. A moderator will review it and '
             'release your escrow.');
    } catch (e) {
      if (!mounted) return;
      setState(() => _status = 'Upload failed.');
      _flash('Upload failed: $e');
    } finally {
      if (mounted) setState(() => _uploading = false);
    }
  }

  void _flash(String msg) {
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text(msg)));
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Icon(Icons.upload_file_outlined,
                     color: theme.colorScheme.primary),
                const SizedBox(width: 8),
                Text('Upload KYC form',
                     style: theme.textTheme.titleMedium),
              ],
            ),
            const SizedBox(height: 8),
            Text(
              "Attach a PDF of your filled-in artist KYC form, or a "
              "scan / photo of a government-issued ID. The file lands "
              "in the full node's KYC inbox tagged with your wallet "
              "address; the moderator reviews it on the node's console "
              "and releases your escrow when approved. PDF, JPG, JPEG, "
              "PNG accepted up to 32 MB.",
              style: theme.textTheme.bodyMedium,
            ),
            const SizedBox(height: 12),
            FilledButton.icon(
              icon: _uploading
                  ? const SizedBox(
                      width: 16, height: 16,
                      child: CircularProgressIndicator(strokeWidth: 2))
                  : const Icon(Icons.upload_file_outlined),
              label: Text(_uploading
                  ? 'Uploading…'
                  : 'Upload KYC form'),
              onPressed: _uploading ? null : _pickAndSubmit,
            ),
            if (_status.isNotEmpty) ...[
              const SizedBox(height: 8),
              Text(_status,
                style: theme.textTheme.bodySmall?.copyWith(
                  color: theme.colorScheme.outline)),
            ],
          ],
        ),
      ),
    );
  }
}
