import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../services/librats_discovery.dart';
import 'dmca_screen.dart';
import 'escrow_claim_screen.dart';

class SettingsScreen extends StatefulWidget {
  const SettingsScreen({super.key});

  @override
  State<SettingsScreen> createState() => _SettingsScreenState();
}

class _SettingsScreenState extends State<SettingsScreen> {
  final _cacheMbCtrl = TextEditingController();

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    final prefs = await SharedPreferences.getInstance();
    setState(() {
      _cacheMbCtrl.text =
          (prefs.getInt('cache_max_mb') ?? 1000).toString();
    });
  }

  Future<void> _save() async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setInt('cache_max_mb', int.tryParse(_cacheMbCtrl.text) ?? 1000);
    if (mounted) {
      ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('Settings saved')));
    }
  }

  @override
  Widget build(BuildContext context) {
    final disc = context.watch<LibratsDiscovery>();

    return Scaffold(
      appBar: AppBar(title: const Text('Settings')),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          const Text('Node', style: TextStyle(fontWeight: FontWeight.bold)),
          const SizedBox(height: 8),
          Text(
            'The player talks to full nodes over librats TCP via the VPS '
            'rendezvous (85.239.238.226:8080). Nodes whose NAT blocks '
            'inbound connections are reached by tunneling every RPC '
            'through the mini-node.',
            style: TextStyle(fontSize: 12, color: Colors.grey.shade400),
          ),
          if (disc.autoSelectedRatsPeerId.isNotEmpty) ...[
            const SizedBox(height: 8),
            Container(
              padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
              decoration: BoxDecoration(
                color: Colors.green.withOpacity(0.15),
                borderRadius: BorderRadius.circular(6),
                border: Border.all(color: Colors.green.shade700, width: 1),
              ),
              child: Row(
                children: [
                  Icon(Icons.check_circle, color: Colors.green.shade400, size: 16),
                  const SizedBox(width: 8),
                  Expanded(
                    child: Text(
                      'rats peer: ${disc.autoSelectedRatsPeerId}',
                      style: const TextStyle(fontSize: 11, fontFamily: 'monospace'),
                      overflow: TextOverflow.ellipsis,
                    ),
                  ),
                ],
              ),
            ),
          ],
          const SizedBox(height: 16),
          const Text('Cache', style: TextStyle(fontWeight: FontWeight.bold)),
          const SizedBox(height: 8),
          TextField(
            controller: _cacheMbCtrl,
            keyboardType: TextInputType.number,
            decoration: const InputDecoration(
                labelText: 'Max Cache (MB)', border: OutlineInputBorder()),
          ),
          const SizedBox(height: 16),
          ElevatedButton(onPressed: _save, child: const Text('Save Settings')),
          const Divider(height: 32),

          // ---- Full nodes via VPS routing table ----
          Row(
            children: [
              const Text('Full nodes',
                  style: TextStyle(fontWeight: FontWeight.bold)),
              const SizedBox(width: 8),
              if (disc.isRefreshing)
                const SizedBox(
                    width: 14, height: 14,
                    child: CircularProgressIndicator(strokeWidth: 2))
              else
                IconButton(
                  icon: const Icon(Icons.refresh, size: 18),
                  tooltip: 'Refresh from VPS',
                  padding: EdgeInsets.zero,
                  constraints: const BoxConstraints(),
                  onPressed: disc.refresh,
                ),
            ],
          ),
          if (disc.vpsStatus.isNotEmpty) ...[
            const SizedBox(height: 4),
            Text(disc.vpsStatus,
                style: const TextStyle(fontSize: 12, color: Colors.grey)),
          ],
          if (disc.lastError.isNotEmpty) ...[
            const SizedBox(height: 4),
            Text('Error: ${disc.lastError}',
                style: const TextStyle(color: Colors.red, fontSize: 12)),
          ],
          const SizedBox(height: 8),
          if (disc.routes.isEmpty)
            const Text('No full nodes registered with the VPS yet.',
                style: TextStyle(color: Colors.grey))
          else
            ...disc.routes.values.map((r) {
              final isAuto = (r['rats_peer_id'] as String? ?? '') ==
                  disc.autoSelectedRatsPeerId;
              final pid = r['rats_peer_id'] as String? ?? '';
              final pub = r['public_address']  as String? ?? '';
              return Card(
                margin: const EdgeInsets.only(bottom: 8),
                color: isAuto ? Colors.green.withOpacity(0.08) : null,
                child: ListTile(
                  dense: true,
                  leading: isAuto
                      ? Icon(Icons.bolt, color: Colors.green.shade400, size: 18)
                      : null,
                  title: Text(pub.isEmpty ? '(no public address)' : pub,
                      style: const TextStyle(
                          fontFamily: 'monospace', fontSize: 12)),
                  subtitle: Text(
                      'rats: ${pid.length >= 14 ? pid.substring(0, 14) : pid}…'
                      '${isAuto ? "  ⚡ selected" : ""}',
                      style: const TextStyle(fontSize: 11)),
                ),
              );
            }),

          const Divider(height: 32),
          const Text('Legal', style: TextStyle(fontWeight: FontWeight.bold)),
          const SizedBox(height: 4),
          ListTile(
            dense: true,
            contentPadding: EdgeInsets.zero,
            leading: const Icon(Icons.savings_outlined),
            title: const Text('Claim escrow'),
            subtitle: const Text(
                'Share your wallet address and upload a KYC form so a '
                'moderator can release your escrowed tokens.'),
            trailing: const Icon(Icons.chevron_right),
            onTap: () => Navigator.of(context).push(
              MaterialPageRoute(builder: (_) => const EscrowClaimScreen()),
            ),
          ),
          ListTile(
            dense: true,
            contentPadding: EdgeInsets.zero,
            leading: const Icon(Icons.gavel_outlined),
            title: const Text('DMCA / Safe Harbor'),
            subtitle: const Text(
                'How takedowns work and where to submit one.'),
            trailing: const Icon(Icons.chevron_right),
            onTap: () => Navigator.of(context).push(
              MaterialPageRoute(builder: (_) => const DmcaScreen()),
            ),
          ),
          const Divider(height: 32),
          const Text('About', style: TextStyle(fontWeight: FontWeight.bold)),
          const SizedBox(height: 8),
          const Text('Bopwire Player v0.1.0'),
          const Text('librats TCP relay via VPS mini-node. No DHT, no HTTP.'),
        ],
      ),
    );
  }
}
