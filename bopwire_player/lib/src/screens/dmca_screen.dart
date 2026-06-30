
// DMCA / safe-harbor page reachable from Settings and from the
// "About copyright" entry in the download long-press menu. The text is
// intentionally informal — it's part of the project's voice and the
// user wrote it that way. The escrow-claim CTA jumps to the wallet
// screen, which already exposes the artist's escrow balance via the
// full node's wallet.escrow_balance RPC.

import 'dart:io';
import 'dart:typed_data';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../providers/wallet_provider.dart';
import '../services/node_client.dart';
import '../services/node_service.dart';
import 'escrow_claim_screen.dart';

class DmcaScreen extends StatelessWidget {
  const DmcaScreen({super.key});

  static const _body = '''
Yes, we understand that sharing copyrighted music is illegal, and we are able to definitively hide your music from the network. Since we don't store any copyrighted material (that is handled by the users), safe harbor applies to this application.

We are also able to bring it back at any time should you reconsider being an asshole and choose to claim your escrowed tokens instead and join the revolution.

If you wish to submit a DMCA takedown request anyway, please have your lawyer submit a PDF and our node administrators will review its validity and comply. Thanks.
''';

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Scaffold(
      appBar: AppBar(title: const Text('DMCA / Safe Harbor')),
      body: SingleChildScrollView(
        padding: const EdgeInsets.fromLTRB(20, 24, 20, 32),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Text(
              _body.trim(),
              style: theme.textTheme.bodyLarge?.copyWith(height: 1.45),
            ),
            const SizedBox(height: 32),
            _EscrowClaimCard(),
            const SizedBox(height: 16),
            _ContactCard(),
          ],
        ),
      ),
    );
  }
}

class _EscrowClaimCard extends StatelessWidget {
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
                Icon(Icons.savings_outlined,
                     color: theme.colorScheme.primary),
                const SizedBox(width: 8),
                Text('Claim your escrowed tokens',
                     style: theme.textTheme.titleMedium),
              ],
            ),
            const SizedBox(height: 8),
            Text(
              'Tokens minted for your songs during the discovery '
              'tier (first 10 000 plays per track) sit in a '
              'per-artist escrow account, releasable by a moderator '
              'after they review a KYC form. The Claim escrow page '
              'walks you through it.',
              style: theme.textTheme.bodyMedium,
            ),
            const SizedBox(height: 12),
            if (wallet != null)
              SelectableText(
                wallet.address,
                style: const TextStyle(
                  fontFamily: 'monospace', fontSize: 12),
              ),
            const SizedBox(height: 12),
            FilledButton.icon(
              icon: const Icon(Icons.savings_outlined),
              label: const Text('Open claim page'),
              onPressed: () => Navigator.of(context).push(
                MaterialPageRoute(
                  builder: (_) => const EscrowClaimScreen())),
            ),
          ],
        ),
      ),
    );
  }
}

class _ContactCard extends StatefulWidget {
  @override
  State<_ContactCard> createState() => _ContactCardState();
}

class _ContactCardState extends State<_ContactCard> {
  bool   _uploading = false;
  String _status    = '';

  Future<void> _pickAndSubmit() async {
    final result = await FilePicker.platform.pickFiles(
      type:              FileType.custom,
      allowedExtensions: const ['pdf'],
      withData:          true,
    );
    if (result == null || result.files.isEmpty) return;
    final picked = result.files.single;

    // Mobile picks return `bytes` directly; desktop pickers may only set
    // `path`. Handle both so the same button works on Android + Windows.
    Uint8List? bytes = picked.bytes;
    if (bytes == null && picked.path != null) {
      bytes = await File(picked.path!).readAsBytes();
    }
    if (bytes == null) {
      _flash('Could not read the picked file.');
      return;
    }
    if (bytes.length > 32 * 1024 * 1024) {
      _flash('PDF is over the 32 MB inbox limit.');
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
      final storedAs = await client.submitDmcaPdf(picked.name, bytes);
      if (!mounted) return;
      setState(() => _status = 'Delivered as $storedAs');
      _flash('Takedown PDF delivered. The node moderator will review it.');
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
                Icon(Icons.description_outlined,
                     color: theme.colorScheme.primary),
                const SizedBox(width: 8),
                Text('Submit a takedown',
                     style: theme.textTheme.titleMedium),
              ],
            ),
            const SizedBox(height: 8),
            Text(
              "Have your lawyer prepare a PDF that identifies the work "
              "and includes the standard 17 U.S.C. § 512(c)(3) sworn "
              "statements. Upload it here; it lands in the full node's "
              "DMCA inbox and the moderator reviews it on the node's "
              "console — when honored, the matched fingerprints are "
              "hidden from every connected player.",
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
                  : 'Upload takedown PDF'),
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
