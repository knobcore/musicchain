// Per-song piece manifest — Swarm Transfer v2, Phase 1.
//
// A manifest is the list of SHA-256 hashes of each fixed-size (256 KB) piece of
// a song's audio file. It lets a downloader verify EACH chunk as it arrives,
// instead of only hashing the whole file at the very end. That per-chunk check
// is what makes safe MULTI-SOURCE downloads possible: a bad/corrupt chunk from
// one misbehaving seeder fails its own piece hash and is refetched from someone
// else — whereas the whole-file-only check can only tell you the assembled file
// is wrong, not WHICH of N sources lied.
//
// The on-chain whole-file content_hash stays the ultimate root of trust (the
// downloader still verifies the assembled file against it). The manifest is an
// off-chain artifact: generated at ingest from the full bytes, submitted to the
// full node with the fingerprint, and served back to downloaders via
// `stream.open`. Because Phase-1 manifests reach the downloader from the full
// node (a trusted anchor) over its existing channel, they are not signed yet;
// the optional `signer`/`sig` fields exist so peer-to-peer manifest
// distribution (Phase 2+) can add a seeder signature without a wire change.
//
// Wire form (JSON): { v:1, piece_size, total_size, pieces } where `pieces` is
// the concatenation of every piece's 64-hex SHA-256 (32 bytes each), in order.

import 'dart:typed_data';

import 'package:crypto/crypto.dart' as crypto;

class PieceManifest {
  PieceManifest({
    required this.pieceSize,
    required this.totalSize,
    required this.pieceHashesHex,
    this.signerPubkey = '',
    this.signature = '',
  });

  /// Bytes per piece (must match PieceDownloaderConfig.pieceSize to be usable).
  final int pieceSize;

  /// Total file size the manifest describes (== on-chain content length).
  final int totalSize;

  /// Concatenated 64-hex SHA-256 of each piece, in order. Length is always
  /// pieceCount * 64 for a well-formed manifest.
  final String pieceHashesHex;

  /// Optional seeder identity for P2P manifest distribution (Phase 2+).
  final String signerPubkey;
  final String signature;

  int get pieceCount =>
      totalSize <= 0 ? 0 : (totalSize + pieceSize - 1) ~/ pieceSize;

  /// 64-hex SHA-256 of piece [i], or '' if out of range / manifest truncated.
  String pieceHashAt(int i) {
    if (i < 0) return '';
    final start = i * 64;
    final end = start + 64;
    if (end > pieceHashesHex.length) return '';
    return pieceHashesHex.substring(start, end);
  }

  /// True iff sha256([bytes]) matches the manifest hash for piece [index].
  /// Returns false when the manifest can't vouch for that index, so the caller
  /// decides whether an un-coverable piece is fatal.
  bool verifyPiece(int index, List<int> bytes) {
    final want = pieceHashAt(index);
    if (want.isEmpty) return false;
    return crypto.sha256.convert(bytes).toString() == want;
  }

  /// Guard against a peer/node serving a manifest for a different piece size or
  /// total, or a truncated hash blob — any of which would make per-piece
  /// verification meaningless.
  bool isWellFormedFor(int expectedPieceSize, int expectedTotalSize) {
    if (pieceSize != expectedPieceSize) return false;
    if (totalSize != expectedTotalSize) return false;
    final pc = pieceCount;
    if (pc <= 0) return false;
    return pieceHashesHex.length == pc * 64;
  }

  /// Build a manifest by hashing each [pieceSize] slice of the full file bytes.
  /// Runs at ingest where we already hold the whole file in memory.
  static PieceManifest generate(Uint8List bytes,
      {int pieceSize = 256 * 1024}) {
    final sb = StringBuffer();
    for (int off = 0; off < bytes.length; off += pieceSize) {
      final end =
          (off + pieceSize > bytes.length) ? bytes.length : off + pieceSize;
      final slice = Uint8List.sublistView(bytes, off, end);
      sb.write(crypto.sha256.convert(slice).toString());
    }
    return PieceManifest(
      pieceSize: pieceSize,
      totalSize: bytes.length,
      pieceHashesHex: sb.toString(),
    );
  }

  Map<String, dynamic> toJson() => {
        'v': 1,
        'piece_size': pieceSize,
        'total_size': totalSize,
        'pieces': pieceHashesHex,
        if (signerPubkey.isNotEmpty) 'signer': signerPubkey,
        if (signature.isNotEmpty) 'sig': signature,
      };

  /// Parse a `manifest` object from a stream.open reply (or any RPC). Returns
  /// null when absent or malformed — callers treat a null manifest as "verify
  /// whole-file only", the legacy behavior.
  static PieceManifest? fromJson(Object? raw) {
    if (raw is! Map) return null;
    final m = raw.cast<String, dynamic>();
    if (m['v'] != 1) return null;
    final ps = (m['piece_size'] as num?)?.toInt() ?? 0;
    final ts = (m['total_size'] as num?)?.toInt() ?? 0;
    final pieces = m['pieces'] as String? ?? '';
    if (ps <= 0 || ts <= 0 || pieces.isEmpty || pieces.length % 64 != 0) {
      return null;
    }
    return PieceManifest(
      pieceSize: ps,
      totalSize: ts,
      pieceHashesHex: pieces,
      signerPubkey: m['signer'] as String? ?? '',
      signature: m['sig'] as String? ?? '',
    );
  }
}
