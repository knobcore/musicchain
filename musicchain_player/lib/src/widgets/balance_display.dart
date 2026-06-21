import 'package:flutter/material.dart';

class BalanceDisplay extends StatefulWidget {
  final String balance;
  final Future<void> Function() onRefresh;

  const BalanceDisplay({super.key, required this.balance, required this.onRefresh});

  @override
  State<BalanceDisplay> createState() => _BalanceDisplayState();
}

class _BalanceDisplayState extends State<BalanceDisplay> {
  bool _refreshing = false;

  Future<void> _handleRefresh() async {
    if (_refreshing) return;
    setState(() => _refreshing = true);
    try {
      await widget.onRefresh();
    } finally {
      if (mounted) setState(() => _refreshing = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Row(
          children: [
            const Icon(Icons.token, size: 32),
            const SizedBox(width: 12),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text('Balance', style: TextStyle(fontWeight: FontWeight.bold)),
                  Text(widget.balance,
                      style: const TextStyle(fontSize: 24, fontFamily: 'monospace')),
                ],
              ),
            ),
            IconButton(
              icon: _refreshing
                  ? const SizedBox(
                      width: 20,
                      height: 20,
                      child: CircularProgressIndicator(strokeWidth: 2),
                    )
                  : const Icon(Icons.refresh),
              onPressed: _refreshing ? null : _handleRefresh,
            ),
          ],
        ),
      ),
    );
  }
}
