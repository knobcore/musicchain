class WalletInfo {
  final String address;
  final String publicKey;
  final String balance; // formatted decimal string

  const WalletInfo({
    required this.address,
    required this.publicKey,
    required this.balance,
  });
}

class TokenTransaction {
  final String txHash;
  final String fromAddress;
  final String toAddress;
  final String amount;
  final String status; // pending, confirmed
  final DateTime timestamp;

  const TokenTransaction({
    required this.txHash,
    required this.fromAddress,
    required this.toAddress,
    required this.amount,
    required this.status,
    required this.timestamp,
  });

  factory TokenTransaction.fromJson(Map<String, dynamic> json) =>
      TokenTransaction(
        txHash: json['tx_hash'] as String,
        fromAddress: json['from_address'] as String? ?? '',
        toAddress: json['to_address'] as String? ?? '',
        amount: json['amount'] as String? ?? '0.00000000',
        status: json['status'] as String? ?? 'pending',
        timestamp: DateTime.fromMillisecondsSinceEpoch(
          (json['timestamp'] as int?) ?? 0,
        ),
      );
}
