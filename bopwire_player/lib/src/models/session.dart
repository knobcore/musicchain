class PlaySession {
  final String sessionId;
  final String contentHash;
  final String blockHash;
  bool isActive;
  int heartbeatCount;
  DateTime startTime;

  PlaySession({
    required this.sessionId,
    required this.contentHash,
    required this.blockHash,
    this.isActive = true,
    this.heartbeatCount = 0,
    required this.startTime,
  });

  factory PlaySession.fromJson(Map<String, dynamic> json) => PlaySession(
        sessionId: json['session_id'] as String,
        contentHash: json['content_hash'] as String? ?? '',
        blockHash: json['block_hash'] as String? ?? '',
        startTime: DateTime.now(),
      );
}

class MintResult {
  final String artistAmount;
  final String nodeAmount;
  final String discovererAmount;
  final bool isDiscoverer;

  const MintResult({
    required this.artistAmount,
    required this.nodeAmount,
    required this.discovererAmount,
    required this.isDiscoverer,
  });

  factory MintResult.fromJson(Map<String, dynamic> json) {
    final minted = json['tokens_minted'] as Map<String, dynamic>? ?? {};
    return MintResult(
      artistAmount: minted['artist_amount'] as String? ?? '0.00000000',
      nodeAmount: minted['node_amount'] as String? ?? '0.00000000',
      discovererAmount: minted['discoverer_amount'] as String? ?? '0.00000000',
      isDiscoverer: json['is_discoverer'] as bool? ?? false,
    );
  }
}
