import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:provider/provider.dart';

import '../providers/wallet_provider.dart';
import '../services/wallet_service.dart';
import '../widgets/balance_display.dart';

class WalletScreen extends StatelessWidget {
  const WalletScreen({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Wallet')),
      body: Consumer<WalletProvider>(
        builder: (context, wallet, _) {
          // WalletGate enforces wallet existence before this screen is
          // reachable, so we always have an info to display. Defensive
          // fallback shouldn't normally fire.
          if (!wallet.hasWallet) {
            return const Center(child: CircularProgressIndicator());
          }
          return _WalletView(wallet: wallet);
        },
      ),
    );
  }
}

class _WalletView extends StatelessWidget {
  final WalletProvider wallet;
  const _WalletView({required this.wallet});

  @override
  Widget build(BuildContext context) {
    final info = wallet.info!;
    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        Card(
          child: Padding(
            padding: const EdgeInsets.all(16),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                const Text('Address',
                    style: TextStyle(fontWeight: FontWeight.bold)),
                const SizedBox(height: 4),
                Row(
                  children: [
                    Expanded(
                      child: Text(info.address,
                          style: const TextStyle(
                              fontFamily: 'monospace', fontSize: 12)),
                    ),
                    IconButton(
                      icon: const Icon(Icons.copy, size: 18),
                      onPressed: () => Clipboard.setData(
                          ClipboardData(text: info.address)),
                    ),
                  ],
                ),
              ],
            ),
          ),
        ),
        const SizedBox(height: 8),
        BalanceDisplay(
            balance: info.balance, onRefresh: wallet.refreshBalance),
        const SizedBox(height: 16),
        const Divider(),
        ListTile(
          leading: const Icon(Icons.send),
          title: const Text('Send Tokens'),
          onTap: () => _showSendDialog(context),
        ),
        ListTile(
          leading: const Icon(Icons.vpn_key),
          title: const Text('Show recovery phrase'),
          subtitle: const Text(
              'Your 12-word BIP39 mnemonic — never share it'),
          onTap: () => _showMnemonicDialog(context),
        ),
        ListTile(
          leading: const Icon(Icons.logout, color: Colors.redAccent),
          title: const Text('Sign out / use a different wallet',
              style: TextStyle(color: Colors.redAccent)),
          subtitle: const Text(
              'Wipes the wallet from this device. The chain still '
              'knows your address. Have your recovery phrase first.'),
          onTap: () => _confirmSignOut(context),
        ),
      ],
    );
  }

  void _showSendDialog(BuildContext context) {
    final toCtrl     = TextEditingController();
    final amountCtrl = TextEditingController();
    showDialog(
      context: context,
      builder: (dialogCtx) => AlertDialog(
        title: const Text('Send Tokens'),
        content: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            TextField(
              controller: toCtrl,
              decoration: const InputDecoration(labelText: 'To Address'),
            ),
            TextField(
              controller: amountCtrl,
              decoration: const InputDecoration(labelText: 'Amount'),
              keyboardType:
                  const TextInputType.numberWithOptions(decimal: true),
            ),
          ],
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(dialogCtx),
            child: const Text('Cancel'),
          ),
          ElevatedButton(
            onPressed: () async {
              Navigator.pop(dialogCtx);
              final err = await context.read<WalletProvider>().sendTokens(
                    toCtrl.text.trim(),
                    amountCtrl.text.trim(),
                  );
              if (!context.mounted) return;
              ScaffoldMessenger.of(context).showSnackBar(SnackBar(
                content: Text(
                  err != null ? 'Transfer failed: $err' : 'Transfer sent!',
                ),
              ));
            },
            child: const Text('Send'),
          ),
        ],
      ),
    );
  }

  Future<void> _showMnemonicDialog(BuildContext context) async {
    final mnemonic = await WalletService().readSavedMnemonic();
    if (!context.mounted) return;
    if (mnemonic == null || mnemonic.isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
        content: Text('No recovery phrase saved on this device.'),
      ));
      return;
    }
    await showDialog<void>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Recovery phrase'),
        content: SelectableText(mnemonic,
            style:
                const TextStyle(fontFamily: 'monospace', fontSize: 14)),
        actions: [
          TextButton(
            onPressed: () {
              Clipboard.setData(ClipboardData(text: mnemonic));
              Navigator.pop(ctx);
            },
            child: const Text('Copy'),
          ),
          TextButton(
            onPressed: () => Navigator.pop(ctx),
            child: const Text('Close'),
          ),
        ],
      ),
    );
  }

  Future<void> _confirmSignOut(BuildContext context) async {
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Wipe wallet from this device?'),
        content: const Text(
            'If you don\'t have your recovery phrase, you will lose '
            'access to this wallet forever.'),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(ctx, false),
            child: const Text('Cancel'),
          ),
          ElevatedButton(
            style: ElevatedButton.styleFrom(
                backgroundColor: Colors.redAccent),
            onPressed: () => Navigator.pop(ctx, true),
            child: const Text('Wipe wallet'),
          ),
        ],
      ),
    );
    if (confirmed != true || !context.mounted) return;
    await WalletService().clearLocalWallet();
    if (!context.mounted) return;
    context.read<WalletProvider>().freeWallet();
    // WalletGate observes the wallet state and will route us back to
    // first-launch as soon as the next frame paints.
  }
}
