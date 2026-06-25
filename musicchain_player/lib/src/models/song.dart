class Song {
  final String contentHash;
  /// Hash of compressed chromaprint blob. Lets a downloader announce
  /// itself to the swarm under the exact same matching key the original
  /// uploader used, no re-decoding needed.
  final String fingerprintHash;
  final String title;
  final String artist;
  final String genre;
  final String album;
  final int    year;        // 0 == unknown
  final int    trackNumber; // 0 == unknown
  final int    durationMs;
  final int    playCount;
  /// How many peers (including ourselves if we hold a variant) the home
  /// node currently counts in this song's swarm. The chain library hides
  /// entries with [swarmSize] == 0 so the user only sees fetchable songs.
  final int    swarmSize;
  final String? discovererAddress;
  final String? blockHash;
  final int? blockHeight;

  const Song({
    required this.contentHash,
    this.fingerprintHash = '',
    required this.title,
    required this.artist,
    required this.genre,
    this.album = '',
    this.year = 0,
    this.trackNumber = 0,
    required this.durationMs,
    this.playCount = 0,
    this.swarmSize = 0,
    this.discovererAddress,
    this.blockHash,
    this.blockHeight,
  });

  factory Song.fromJson(Map<String, dynamic> json) {
    // nlohmann::json on the full node serialises uint as plain numbers,
    // which can come across as `num` not `int` depending on size. The
    // narrow `as int?` cast was failing silently and defaulting
    // swarmSize to 0 for songs with valid peers (so the chain library
    // filter was hiding everything). Coerce through `toInt()` so any
    // numeric value lands as an int.
    int _i(dynamic v) {
      if (v is int)    return v;
      if (v is double) return v.toInt();
      if (v is num)    return v.toInt();
      if (v is String) return int.tryParse(v) ?? 0;
      return 0;
    }
    return Song(
      // (#crash) never hard-cast: one bad/missing field would TypeError and
      // abort decoding the WHOLE songs list. content_hash empty → caller skips.
      contentHash:       json['content_hash']     as String? ?? '',
      fingerprintHash:   json['fingerprint_hash'] as String? ?? '',
      title:             json['title']            as String? ?? '',
      artist:            json['artist']           as String? ?? '',
      genre:             json['genre']            as String? ?? '',
      album:             json['album']            as String? ?? '',
      year:              _i(json['year']),
      trackNumber:       _i(json['track_number']),
      durationMs:        _i(json['duration_ms']),
      playCount:         _i(json['play_count']),
      swarmSize:         _i(json['swarm_size']),
      discovererAddress: json['discoverer'] as String?,
      blockHash:         json['block_hash'] as String?,
      blockHeight:       json['block_height'] == null ? null : _i(json['block_height']),
    );
  }

  String get durationFormatted {
    final total = durationMs ~/ 1000;
    final min = total ~/ 60;
    final sec = total % 60;
    return '$min:${sec.toString().padLeft(2, '0')}';
  }

  bool get rewardTierFull => playCount < 50000;
}
