// "My Library" tab — same drill + resizable split layout as the
// Discover tab so the two surfaces feel flush. Artist / Genre at the
// root, drill into albums, and a bottom pane shows the selected
// album's tracks. The Folders and Scan-now actions stay in the AppBar
// (the only difference from Discover beyond the data source).

import 'dart:async';

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/song.dart';
import '../providers/download_provider.dart';
import '../providers/player_provider.dart';
import '../providers/wallet_provider.dart';
import '../services/library_scanner.dart';
import '../services/library_service.dart';
import '../services/local_library_actions.dart';
import 'dmca_screen.dart';
import 'folders_screen.dart';

enum _FacetMode { artist, genre }

extension _FacetModeLabel on _FacetMode {
  String get label => switch (this) {
    _FacetMode.artist => 'Artist',
    _FacetMode.genre  => 'Genre',
  };
  IconData get icon => switch (this) {
    _FacetMode.artist => Icons.person_outline,
    _FacetMode.genre  => Icons.style_outlined,
  };
  String get rootLabel => switch (this) {
    _FacetMode.artist => 'Artists',
    _FacetMode.genre  => 'Genres',
  };
}

class LocalLibraryScreen extends StatefulWidget {
  const LocalLibraryScreen({super.key});

  @override
  State<LocalLibraryScreen> createState() => _LocalLibraryScreenState();
}

class _LocalLibraryScreenState extends State<LocalLibraryScreen> {
  _FacetMode _mode = _FacetMode.artist;
  String? _drillGenre;
  String? _drillArtist;
  String? _selectedAlbum;
  double  _topFraction = 0.5;
  bool    _scanning    = false;

  Future<void> _openFolders() async {
    await Navigator.of(context).push(MaterialPageRoute(
      builder: (_) => const FoldersScreen(),
    ));
  }

  Future<void> _scanNow() async {
    setState(() => _scanning = true);
    try {
      await LibraryScanner.instance.scanOnce(
        force: true,
        onProgress: () { if (mounted) setState(() {}); },
      );
    } finally {
      if (mounted) setState(() => _scanning = false);
    }
  }

  // ---- Bucket / sort helpers ------------------------------------------
  //
  // Tag spellings rarely line up — "FIDLAR" / "Fidlar", "Rock" / "rock"
  // etc. all collide on the same audio. We group case-insensitively
  // and pick the most-frequent original spelling for display so the
  // chip shows what the user actually has.

  String _artistKey(LibraryEntry e) {
    final t = e.artist.trim();
    return t.isEmpty ? 'Unknown Artist' : t;
  }
  String _artistKeyNorm(LibraryEntry e) => _artistKey(e).toLowerCase();

  String _genreKey(LibraryEntry e) {
    final t = e.genre.trim();
    return t.isEmpty ? 'Unknown Genre' : t;
  }
  String _genreKeyNorm(LibraryEntry e) => _genreKey(e).toLowerCase();

  String _albumKey(LibraryEntry e) {
    final t = e.album.trim();
    return t.isEmpty ? 'Singles' : t;
  }
  String _albumKeyNorm(LibraryEntry e) => _albumKey(e).toLowerCase();

  int _sortAlpha(String a, String b) {
    final ua = a.startsWith('Unknown') || a == 'Singles';
    final ub = b.startsWith('Unknown') || b == 'Singles';
    if (ua && !ub) return 1;
    if (ub && !ua) return -1;
    return a.toLowerCase().compareTo(b.toLowerCase());
  }

  int _earliestYear(Iterable<LibraryEntry> tracks) {
    int y = 0;
    for (final e in tracks) {
      if (e.year > 0 && (y == 0 || e.year < y)) y = e.year;
    }
    return y;
  }

  Iterable<LibraryEntry> _drillFilter(List<LibraryEntry> entries) {
    final wantedGenre  = _drillGenre?.toLowerCase();
    final wantedArtist = _drillArtist?.toLowerCase();
    return entries.where((e) {
      if (wantedGenre  != null && _genreKeyNorm(e) != wantedGenre)  return false;
      if (wantedArtist != null && _artistKeyNorm(e) != wantedArtist) return false;
      return true;
    });
  }

  /// Distinct track count by dedup key (fingerprint > canonical > local
  /// content_hash). Mirrors the rule the previous local-library list
  /// used so chip counts stay honest in the presence of variants.
  int _distinctTrackCount(Iterable<LibraryEntry> entries) {
    final seen = <String>{};
    for (final e in entries) {
      seen.add(_dedupKey(e));
    }
    return seen.length;
  }

  String _dedupKey(LibraryEntry e) {
    if (e.fingerprintHash.isNotEmpty) return 'fp:${e.fingerprintHash}';
    if (e.canonicalHash.isNotEmpty)   return 'ch:${e.canonicalHash}';
    return 'lh:${e.contentHash}';
  }

  // ---- Drill actions --------------------------------------------------

  void _selectMode(_FacetMode m) {
    setState(() {
      _mode = m;
      _drillGenre    = null;
      _drillArtist   = null;
      _selectedAlbum = null;
    });
  }

  void _onPillTapped(String key, _DrillLevel level) {
    setState(() {
      switch (level) {
        case _DrillLevel.genre:
          _drillGenre    = _drillGenre == key ? null : key;
          _drillArtist   = null;
          _selectedAlbum = null;
        case _DrillLevel.artist:
          _drillArtist   = _drillArtist == key ? null : key;
          _selectedAlbum = null;
        case _DrillLevel.album:
          _selectedAlbum = _selectedAlbum == key ? null : key;
      }
    });
  }

  void _crumbBack(int targetDepth) {
    setState(() {
      if (targetDepth < 3) _selectedAlbum = null;
      if (targetDepth < 2) _drillArtist   = null;
      if (targetDepth < 1) _drillGenre    = null;
    });
  }

  // ---- Playback / queue helpers ---------------------------------------

  Song _toSong(LibraryEntry e) {
    final hash = e.canonicalHash.isNotEmpty ? e.canonicalHash : e.contentHash;
    return Song(
      contentHash:     hash,
      fingerprintHash: e.fingerprintHash,
      title:           e.title,
      artist:          e.artist,
      album:           e.album,
      genre:           e.genre,
      year:            e.year,
      trackNumber:     e.trackNumber,
      durationMs:      e.durationMs,
    );
  }

  /// Play [tracks] as a queue starting at [startIndex] (the row the user
  /// tapped). Remote / not-yet-downloaded entries get filtered out before
  /// they reach the player — they have no local file to stream — and the
  /// start index slides to the tapped track's position in that local
  /// subset (or to 0 if the tapped track itself isn't playable, which
  /// should be impossible since the row's tap handler is null for remote
  /// entries, but we guard anyway). This is what makes tapping a track in
  /// an open album play the whole album instead of just that one song.
  void _playFromAlbum(List<LibraryEntry> tracks, int startIndex) {
    if (tracks.isEmpty) return;
    final wallet = context.read<WalletProvider>().info;
    if (wallet == null) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Create a wallet first')),
      );
      return;
    }
    final tapped = (startIndex >= 0 && startIndex < tracks.length)
        ? tracks[startIndex]
        : null;
    final playable = tracks.where((e) => e.isLocal).toList();
    if (playable.isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content:
          Text('Nothing in this album is downloaded yet.')),
      );
      return;
    }
    var idx = 0;
    if (tapped != null) {
      final foundAt = playable.indexWhere(
          (e) => e.contentHash == tapped.contentHash);
      if (foundAt >= 0) idx = foundAt;
    }
    final playlist = playable.map(_toSong).toList();
    context.read<PlayerProvider>()
        .playPlaylist(playlist, idx, wallet.address);
  }

  void _playGroup(List<LibraryEntry> entries) {
    if (entries.isEmpty) return;
    final wallet = context.read<WalletProvider>().info;
    if (wallet == null) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Create a wallet first')),
      );
      return;
    }
    final playable = _sortEntries(entries).where((e) => e.isLocal).toList();
    if (playable.isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content:
          Text('Nothing in this group is downloaded yet.')),
      );
      return;
    }
    final playlist = playable.map(_toSong).toList();
    context.read<PlayerProvider>()
        .playPlaylist(playlist, 0, wallet.address);
  }

  List<LibraryEntry> _sortEntries(Iterable<LibraryEntry> entries) {
    final out = [...entries];
    out.sort((a, b) {
      if (_drillArtist != null && _drillGenre != null) {
        // Inside an album already — track number is enough.
      } else {
        final aa = a.album.trim();
        final ab = b.album.trim();
        if (aa.isEmpty && ab.isNotEmpty) return 1;
        if (ab.isEmpty && aa.isNotEmpty) return -1;
        if (a.year > 0 && b.year > 0 && a.year != b.year) {
          return a.year.compareTo(b.year);
        }
        final ac = aa.toLowerCase().compareTo(ab.toLowerCase());
        if (ac != 0) return ac;
      }
      if (a.trackNumber > 0 && b.trackNumber > 0
          && a.trackNumber != b.trackNumber) {
        return a.trackNumber.compareTo(b.trackNumber);
      }
      if (a.trackNumber > 0 && b.trackNumber == 0) return -1;
      if (b.trackNumber > 0 && a.trackNumber == 0) return 1;
      return a.title.toLowerCase().compareTo(b.title.toLowerCase());
    });
    return out;
  }

  // ---- Build ----------------------------------------------------------

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('My Library'),
        actions: [
          IconButton(
            tooltip: 'Folders',
            icon: const Icon(Icons.create_new_folder_outlined),
            onPressed: _openFolders,
          ),
          IconButton(
            tooltip: 'Scan now',
            icon: _scanning
                ? const SizedBox(
                    width: 18, height: 18,
                    child: CircularProgressIndicator(strokeWidth: 2))
                : const Icon(Icons.refresh),
            onPressed: _scanning ? null : _scanNow,
          ),
        ],
      ),
      body: Consumer<LibraryService>(
        builder: (context, lib, _) {
          final entries = lib.entries;
          if (entries.isEmpty) {
            return Center(
              child: Padding(
                padding: const EdgeInsets.all(32),
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Icon(Icons.library_music_outlined,
                         size: 64,
                         color: Theme.of(context)
                             .colorScheme.onSurfaceVariant),
                    const SizedBox(height: 16),
                    const Text(
                      'No songs yet. Tap the folder-plus icon above to add '
                      'a music folder, then tap refresh to scan.',
                      textAlign: TextAlign.center,
                      style: TextStyle(color: Colors.grey),
                    ),
                  ],
                ),
              ),
            );
          }
          return Column(
            children: [
              _ModeToolbar(mode: _mode, onChange: _selectMode),
              _Breadcrumb(segments: _breadcrumb()),
              if (_scanning)
                _ScannerProgressBar(scanner: LibraryScanner.instance),
              Expanded(child: _splitBody(entries)),
            ],
          );
        },
      ),
    );
  }

  List<_CrumbSeg> _breadcrumb() {
    final out = <_CrumbSeg>[
      _CrumbSeg(label: _mode.rootLabel, onTap: () => _crumbBack(0)),
    ];
    if (_drillGenre != null) {
      out.add(_CrumbSeg(label: _drillGenre!, onTap: () => _crumbBack(1)));
    }
    if (_drillArtist != null) {
      out.add(_CrumbSeg(label: _drillArtist!, onTap: () => _crumbBack(2)));
    }
    if (_selectedAlbum != null) {
      out.add(_CrumbSeg(label: _selectedAlbum!, onTap: null));
    }
    return out;
  }

  Widget _splitBody(List<LibraryEntry> allEntries) {
    final wantedAlbum = _selectedAlbum?.toLowerCase();
    final selectedTracks = wantedAlbum == null
        ? const <LibraryEntry>[]
        : _sortEntries(_drillFilter(allEntries)
            .where((e) => _albumKeyNorm(e) == wantedAlbum));

    return LayoutBuilder(
      builder: (context, constraints) {
        final totalH   = constraints.maxHeight;
        final hasBottom = _selectedAlbum != null;
        final topH    = hasBottom ? totalH * _topFraction : totalH;
        final bottomH = hasBottom ? totalH - topH - _kHandleHeight : 0.0;
        return Stack(
          children: [
            Positioned(
              top: 0,
              left: 0,
              right: 0,
              height: topH,
              child: _topPane(allEntries),
            ),
            if (hasBottom) ...[
              Positioned(
                top: topH,
                left: 0,
                right: 0,
                height: _kHandleHeight,
                child: _DragHandle(
                  onDelta: (dy) => setState(() {
                    _topFraction = (_topFraction + dy / totalH)
                        .clamp(0.20, 0.80);
                  }),
                ),
              ),
              Positioned(
                top: topH + _kHandleHeight,
                left: 0,
                right: 0,
                height: bottomH,
                child: _TrackPane(
                  albumName:      _selectedAlbum!,
                  artistFallback: _drillArtist,
                  tracks:         selectedTracks,
                  onPlay:         _playFromAlbum,
                  onClose:        () => setState(() => _selectedAlbum = null),
                ),
              ),
            ],
          ],
        );
      },
    );
  }

  Widget _topPane(List<LibraryEntry> entries) {
    final pills = _pillsFor(_currentLevel(), entries);
    if (pills.isEmpty) {
      return Center(
        child: Padding(
          padding: const EdgeInsets.all(32),
          child: Text(
            'Nothing under "${_breadcrumb().last.label}" yet.',
            textAlign: TextAlign.center,
            style: TextStyle(color: Theme.of(context)
                .colorScheme.onSurfaceVariant),
          ),
        ),
      );
    }
    return SingleChildScrollView(
      padding: const EdgeInsets.fromLTRB(12, 8, 12, 12),
      child: Wrap(
        spacing: 8,
        runSpacing: 8,
        children: pills,
      ),
    );
  }

  _DrillLevel _currentLevel() {
    if (_drillArtist != null) return _DrillLevel.album;
    if (_drillGenre  != null) return _DrillLevel.artist;
    return _mode == _FacetMode.artist ? _DrillLevel.artist : _DrillLevel.genre;
  }

  List<Widget> _pillsFor(_DrillLevel level, List<LibraryEntry> entries) {
    final scoped = _drillFilter(entries).toList();
    switch (level) {
      case _DrillLevel.genre:
        final buckets = _bucketByNorm(scoped, _genreKey, _genreKeyNorm);
        final keys = buckets.keys.toList()..sort(_sortAlpha);
        return [
          for (final k in keys)
            _LocalChip(
              icon:    Icons.style_outlined,
              label:   '$k  (${_distinctTrackCount(buckets[k]!)})',
              selected: false,
              onTap:    () => _onPillTapped(k, _DrillLevel.genre),
              entries:  buckets[k]!,
              onPlay:   () => _playGroup(buckets[k]!),
            ),
        ];
      case _DrillLevel.artist:
        final buckets = _bucketByNorm(scoped, _artistKey, _artistKeyNorm);
        final keys = buckets.keys.toList()..sort(_sortAlpha);
        return [
          for (final k in keys)
            _LocalChip(
              icon:    Icons.person_outline,
              label:   '$k  (${_distinctTrackCount(buckets[k]!)})',
              selected: false,
              onTap:    () => _onPillTapped(k, _DrillLevel.artist),
              entries:  buckets[k]!,
              onPlay:   () => _playGroup(buckets[k]!),
            ),
        ];
      case _DrillLevel.album:
        final buckets = _bucketByNorm(scoped, _albumKey, _albumKeyNorm);
        final keys = buckets.keys.toList()..sort((a, b) {
          final ya = _earliestYear(buckets[a]!);
          final yb = _earliestYear(buckets[b]!);
          if (ya > 0 && yb > 0 && ya != yb) return ya.compareTo(yb);
          if (ya > 0 && yb == 0) return -1;
          if (yb > 0 && ya == 0) return 1;
          return _sortAlpha(a, b);
        });
        return [
          for (final k in keys)
            _LocalChip(
              icon:     Icons.album_outlined,
              label:    _formatAlbumLabel(k, buckets[k]!),
              selected: _selectedAlbum?.toLowerCase() == k.toLowerCase(),
              onTap:    () => _onPillTapped(k, _DrillLevel.album),
              entries:  buckets[k]!,
              onPlay:   () => _playGroup(buckets[k]!),
            ),
        ];
    }
  }

  /// Group `items` by `norm(item)` and pick the most-common spelling of
  /// `display(item)` as the bucket key. Mirrors the same fix in the
  /// Discover screen so "FIDLAR" + "Fidlar" coalesce into one chip with
  /// whichever spelling dominates the user's tags.
  Map<String, List<LibraryEntry>> _bucketByNorm(
      List<LibraryEntry> items,
      String Function(LibraryEntry) display,
      String Function(LibraryEntry) norm) {
    final normToVariants = <String, Map<String, int>>{};
    final normToTracks   = <String, List<LibraryEntry>>{};
    for (final e in items) {
      final n = norm(e);
      final d = display(e);
      (normToTracks[n] ??= []).add(e);
      (normToVariants[n] ??= <String, int>{})[d] =
          (normToVariants[n]![d] ?? 0) + 1;
    }
    final out = <String, List<LibraryEntry>>{};
    normToVariants.forEach((n, variants) {
      String best = variants.keys.first;
      int    bestCount = variants[best]!;
      variants.forEach((d, c) {
        if (c > bestCount || (c == bestCount && d.length > best.length)) {
          best = d;
          bestCount = c;
        }
      });
      out[best] = normToTracks[n]!;
    });
    return out;
  }

  String _formatAlbumLabel(String name, List<LibraryEntry> tracks) {
    final year = _earliestYear(tracks);
    final n = _distinctTrackCount(tracks);
    if (year > 0) return '$name  ($year · $n)';
    return '$name  ($n)';
  }
}

// ---- Shared bits (same shape as the Discover tab) ----------------------

enum _DrillLevel { genre, artist, album }

class _CrumbSeg {
  _CrumbSeg({required this.label, this.onTap});
  final String        label;
  final VoidCallback? onTap;
}

const double _kHandleHeight = 14;

class _ModeToolbar extends StatelessWidget {
  const _ModeToolbar({required this.mode, required this.onChange});
  final _FacetMode               mode;
  final ValueChanged<_FacetMode> onChange;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(12, 12, 12, 4),
      child: Row(
        children: [
          Expanded(
            child: SegmentedButton<_FacetMode>(
              showSelectedIcon: false,
              segments: _FacetMode.values.map((m) => ButtonSegment(
                value: m,
                label: Text(m.label),
                icon:  Icon(m.icon),
              )).toList(),
              selected: {mode},
              onSelectionChanged: (s) => onChange(s.first),
            ),
          ),
        ],
      ),
    );
  }
}

class _Breadcrumb extends StatelessWidget {
  const _Breadcrumb({required this.segments});
  final List<_CrumbSeg> segments;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return SizedBox(
      height: 32,
      child: ListView.separated(
        scrollDirection: Axis.horizontal,
        padding: const EdgeInsets.symmetric(horizontal: 12),
        itemCount: segments.length,
        separatorBuilder: (_, __) => Padding(
          padding: const EdgeInsets.symmetric(horizontal: 4),
          child: Icon(Icons.chevron_right,
              size: 14, color: theme.colorScheme.onSurfaceVariant),
        ),
        itemBuilder: (context, i) {
          final seg = segments[i];
          final isLast = i == segments.length - 1;
          final color = seg.onTap == null
              ? theme.colorScheme.onSurface
              : theme.colorScheme.primary;
          final text = Text(
            seg.label,
            style: theme.textTheme.bodyMedium?.copyWith(
              color: color,
              fontWeight: isLast ? FontWeight.w600 : FontWeight.w500,
            ),
          );
          return Center(
            child: seg.onTap == null
                ? text
                : InkWell(
                    onTap: seg.onTap,
                    borderRadius: BorderRadius.circular(4),
                    child: Padding(
                      padding: const EdgeInsets.symmetric(
                          horizontal: 4, vertical: 2),
                      child: text,
                    ),
                  ),
          );
        },
      ),
    );
  }
}

class _LocalChip extends StatelessWidget {
  const _LocalChip({
    required this.icon,
    required this.label,
    required this.selected,
    required this.onTap,
    required this.entries,
    required this.onPlay,
  });
  final IconData            icon;
  final String              label;
  final bool                selected;
  final VoidCallback        onTap;
  final List<LibraryEntry>  entries;
  final VoidCallback        onPlay;

  Future<void> _showMenu(BuildContext context, Offset pos) async {
    final overlay = Overlay.of(context).context.findRenderObject() as RenderBox;
    final picked = await showMenu<String>(
      context: context,
      position: RelativeRect.fromRect(
        pos & const Size(40, 40),
        Offset.zero & overlay.size,
      ),
      items: const [
        PopupMenuItem(
          value: 'play',
          child: ListTile(
            dense: true,
            leading: Icon(Icons.play_arrow, size: 18),
            title: Text('Play'),
            contentPadding: EdgeInsets.zero,
          ),
        ),
        PopupMenuItem(
          value: 'delete',
          child: ListTile(
            dense: true,
            leading: Icon(Icons.delete_outline,
                          size: 18, color: Colors.red),
            title: Text('Delete'),
            contentPadding: EdgeInsets.zero,
          ),
        ),
        PopupMenuDivider(),
        PopupMenuItem(
          value: 'dmca',
          child: ListTile(
            dense: true,
            leading: Icon(Icons.gavel_outlined, size: 18),
            title: Text('About copyright / DMCA'),
            contentPadding: EdgeInsets.zero,
          ),
        ),
      ],
    );
    if (picked == 'play') {
      onPlay();
    } else if (picked == 'delete' && context.mounted) {
      await _confirmAndDelete(context);
    } else if (picked == 'dmca' && context.mounted) {
      Navigator.of(context).push(MaterialPageRoute(
        builder: (_) => const DmcaScreen(),
      ));
    }
  }

  Future<void> _confirmAndDelete(BuildContext context) async {
    final messenger = ScaffoldMessenger.maybeOf(context);
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Delete?'),
        content: Text(
          'Remove ${entries.length} track${entries.length == 1 ? "" : "s"} '
          'from your library? Downloaded files are deleted from disk; '
          'files in folders you scanned stay where they are.',
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(ctx, false),
            child: const Text('Cancel'),
          ),
          FilledButton.tonal(
            style: FilledButton.styleFrom(
                foregroundColor: Theme.of(ctx).colorScheme.error),
            onPressed: () => Navigator.pop(ctx, true),
            child: const Text('Delete'),
          ),
        ],
      ),
    );
    if (ok != true) return;
    final svc = LocalLibraryActions.instance;
    int dropped = 0, fileDeleted = 0;
    for (final e in entries) {
      final r = await svc.deleteEntry(e);
      dropped++;
      if (r.fileDeleted) fileDeleted++;
    }
    messenger?.showSnackBar(SnackBar(
      content: Text(
        'Deleted $dropped track${dropped == 1 ? "" : "s"}'
        '${fileDeleted > 0 ? " ($fileDeleted file"
            "${fileDeleted == 1 ? "" : "s"} removed from disk)" : ""}.',
      ),
      duration: const Duration(seconds: 3),
    ));
  }

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      onSecondaryTapDown: (d) => _showMenu(context, d.globalPosition),
      onLongPressStart:   (d) => _showMenu(context, d.globalPosition),
      child: ChoiceChip(
        avatar:   Icon(icon, size: 18),
        label:    Text(label, overflow: TextOverflow.ellipsis),
        selected: selected,
        onSelected: (_) => onTap(),
      ),
    );
  }
}

class _DragHandle extends StatelessWidget {
  const _DragHandle({required this.onDelta});
  final ValueChanged<double> onDelta;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return MouseRegion(
      cursor: SystemMouseCursors.resizeRow,
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onVerticalDragUpdate: (details) => onDelta(details.delta.dy),
        child: Container(
          height: _kHandleHeight,
          color: theme.colorScheme.surfaceContainerHighest,
          alignment: Alignment.center,
          child: Container(
            width: 36,
            height: 3,
            decoration: BoxDecoration(
              color: theme.colorScheme.outlineVariant,
              borderRadius: BorderRadius.circular(2),
            ),
          ),
        ),
      ),
    );
  }
}

class _TrackPane extends StatelessWidget {
  const _TrackPane({
    required this.albumName,
    required this.artistFallback,
    required this.tracks,
    required this.onPlay,
    required this.onClose,
  });
  final String                          albumName;
  final String?                         artistFallback;
  final List<LibraryEntry>              tracks;
  /// Called when the user taps a row. The whole [tracks] list goes to the
  /// player as the queue; the second argument is the index of the tapped
  /// row, so playback starts there and auto-advances through the rest of
  /// the album. (Pre-fix this was `void Function(LibraryEntry)` and only
  /// the single tapped track was queued, which is the bug we're fixing.)
  final void Function(List<LibraryEntry> tracks, int index) onPlay;
  final VoidCallback                    onClose;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final artist = (tracks.isNotEmpty && tracks.first.artist.trim().isNotEmpty)
        ? tracks.first.artist
        : (artistFallback ?? '');
    final year = tracks
        .map((e) => e.year)
        .firstWhere((y) => y > 0, orElse: () => 0);
    final localCount = tracks.where((e) => e.isLocal).length;
    return Material(
      elevation: 4,
      color: theme.colorScheme.surface,
      child: Column(
        children: [
          Container(
            padding: const EdgeInsets.fromLTRB(12, 8, 6, 8),
            decoration: BoxDecoration(
              color: theme.colorScheme.surfaceContainerHighest,
            ),
            child: Row(
              children: [
                Icon(Icons.album_outlined,
                    size: 18, color: theme.colorScheme.primary),
                const SizedBox(width: 8),
                Expanded(
                  child: Column(
                    mainAxisSize: MainAxisSize.min,
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(
                        albumName,
                        maxLines: 1, overflow: TextOverflow.ellipsis,
                        style: theme.textTheme.titleSmall?.copyWith(
                          fontWeight: FontWeight.w600),
                      ),
                      Text(
                        [
                          if (artist.isNotEmpty) artist,
                          if (year > 0) '$year',
                          '${tracks.length} track${tracks.length == 1 ? "" : "s"}',
                          '$localCount local',
                        ].join(' · '),
                        maxLines: 1, overflow: TextOverflow.ellipsis,
                        style: theme.textTheme.bodySmall?.copyWith(
                          color: theme.colorScheme.onSurfaceVariant),
                      ),
                    ],
                  ),
                ),
                IconButton(
                  tooltip: 'Close',
                  icon: const Icon(Icons.close, size: 20),
                  onPressed: onClose,
                ),
              ],
            ),
          ),
          Expanded(
            child: ListView.builder(
              itemCount: tracks.length,
              itemBuilder: (context, i) => _LocalEntryRow(
                entry: tracks[i],
                index: i,
                onPlay: () => onPlay(tracks, i),
              ),
            ),
          ),
        ],
      ),
    );
  }
}

class _LocalEntryRow extends StatelessWidget {
  const _LocalEntryRow({
    required this.entry,
    required this.index,
    required this.onPlay,
  });
  final LibraryEntry entry;
  final int          index;
  final VoidCallback onPlay;

  String _fmtDuration(int ms) {
    if (ms <= 0) return '--:--';
    final total = (ms / 1000).round();
    final m = (total ~/ 60).toString();
    final s = (total % 60).toString().padLeft(2, '0');
    return '$m:$s';
  }

  Future<void> _showMenu(BuildContext context, Offset pos) async {
    final overlay = Overlay.of(context).context.findRenderObject() as RenderBox;
    final picked = await showMenu<String>(
      context: context,
      position: RelativeRect.fromRect(
        pos & const Size(40, 40),
        Offset.zero & overlay.size,
      ),
      items: const [
        PopupMenuItem(
          value: 'delete',
          child: ListTile(
            dense: true,
            leading: Icon(Icons.delete_outline,
                          size: 18, color: Colors.red),
            title: Text('Delete song'),
            contentPadding: EdgeInsets.zero,
          ),
        ),
        PopupMenuDivider(),
        PopupMenuItem(
          value: 'dmca',
          child: ListTile(
            dense: true,
            leading: Icon(Icons.gavel_outlined, size: 18),
            title: Text('About copyright / DMCA'),
            contentPadding: EdgeInsets.zero,
          ),
        ),
      ],
    );
    if (picked == 'delete' && context.mounted) {
      await _confirmAndDelete(context);
    } else if (picked == 'dmca' && context.mounted) {
      Navigator.of(context).push(MaterialPageRoute(
        builder: (_) => const DmcaScreen(),
      ));
    }
  }

  Future<void> _confirmAndDelete(BuildContext context) async {
    final messenger = ScaffoldMessenger.maybeOf(context);
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Delete song?'),
        content: Text(
          'Remove "${entry.title.isEmpty ? "Untitled" : entry.title}" from your '
          'library? Downloaded files are deleted from disk; files in folders '
          'you scanned stay where they are.',
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(ctx, false),
            child: const Text('Cancel'),
          ),
          FilledButton.tonal(
            style: FilledButton.styleFrom(
                foregroundColor: Theme.of(ctx).colorScheme.error),
            onPressed: () => Navigator.pop(ctx, true),
            child: const Text('Delete'),
          ),
        ],
      ),
    );
    if (ok != true) return;
    final r = await LocalLibraryActions.instance.deleteEntry(entry);
    messenger?.showSnackBar(SnackBar(
      content: Text(
        r.fileDeleted
            ? 'Deleted "${entry.title}" + file from disk.'
            : 'Removed "${entry.title}" from library.',
      ),
      duration: const Duration(seconds: 2),
    ));
  }

  void _queueDownload(BuildContext context) {
    final messenger = ScaffoldMessenger.maybeOf(context);
    final canonical = entry.canonicalHash.isNotEmpty
        ? entry.canonicalHash
        : entry.contentHash;
    final song = Song(
      contentHash:     canonical,
      fingerprintHash: entry.fingerprintHash,
      title:           entry.title,
      artist:          entry.artist,
      album:           entry.album,
      genre:           entry.genre,
      year:            entry.year,
      trackNumber:     entry.trackNumber,
      durationMs:      entry.durationMs,
    );
    DownloadProvider.instance.enqueueSong(song);
    messenger?.showSnackBar(SnackBar(
      content: Text('Queued "${entry.title.isEmpty
          ? entry.contentHash.substring(0, 8)
          : entry.title}"'),
      duration: const Duration(seconds: 2),
    ));
  }

  @override
  Widget build(BuildContext context) {
    final isLocal = entry.isLocal;
    final dl = context.watch<DownloadProvider>();
    DownloadJob? activeJob;
    for (final j in dl.activeJobs) {
      final canonical = entry.canonicalHash.isNotEmpty
          ? entry.canonicalHash
          : entry.contentHash;
      if (j.song.contentHash == canonical) {
        activeJob = j;
        break;
      }
    }

    final subtitle = StringBuffer();
    if (entry.album.trim().isNotEmpty) {
      subtitle.write(entry.album);
      subtitle.write('  •  ');
    }
    if (entry.artist.trim().isNotEmpty) {
      subtitle.write(entry.artist);
      subtitle.write('  •  ');
    }
    subtitle.write(_fmtDuration(entry.durationMs));

    Widget trailing;
    if (isLocal) {
      trailing = Row(
        mainAxisSize: MainAxisSize.min,
        mainAxisAlignment: MainAxisAlignment.end,
        children: const [
          Icon(Icons.audiotrack, color: Colors.green, size: 18),
          SizedBox(width: 4),
          Text('local',
              style: TextStyle(fontSize: 10, color: Colors.green)),
        ],
      );
    } else if (activeJob != null) {
      trailing = SizedBox(
        width: 18, height: 18,
        child: CircularProgressIndicator(
          strokeWidth: 2.5,
          value: activeJob.total > 0
              ? activeJob.received / activeJob.total
              : null,
        ),
      );
    } else {
      trailing = Row(
        mainAxisSize: MainAxisSize.min,
        mainAxisAlignment: MainAxisAlignment.end,
        children: [
          IconButton(
            tooltip: 'Download to library',
            padding: EdgeInsets.zero,
            constraints: const BoxConstraints(minWidth: 28, minHeight: 28),
            icon: const Icon(Icons.download_for_offline_outlined, size: 18),
            onPressed: () => _queueDownload(context),
          ),
          const SizedBox(width: 2),
          const Text('remote',
              style: TextStyle(fontSize: 10, color: Colors.blueGrey)),
        ],
      );
    }

    return GestureDetector(
      behavior: HitTestBehavior.translucent,
      onSecondaryTapDown: (d) => _showMenu(context, d.globalPosition),
      onLongPressStart:   (d) => _showMenu(context, d.globalPosition),
      child: ListTile(
        dense: true,
        leading: SizedBox(
          width: 28,
          child: Text(
            entry.trackNumber > 0 ? '${entry.trackNumber}' : '${index + 1}',
            textAlign: TextAlign.right,
            style: Theme.of(context).textTheme.bodySmall,
          ),
        ),
        title: Text(
          entry.title.isEmpty ? '(untitled)' : entry.title,
          maxLines: 1, overflow: TextOverflow.ellipsis,
        ),
        subtitle: Text(
          subtitle.toString(),
          style: const TextStyle(fontSize: 11),
          maxLines: 1, overflow: TextOverflow.ellipsis,
        ),
        onTap: isLocal ? onPlay : null,
        trailing: SizedBox(width: 96, child: trailing),
      ),
    );
  }
}

class _ScannerProgressBar extends StatelessWidget {
  const _ScannerProgressBar({required this.scanner});
  final LibraryScanner scanner;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 4, 16, 8),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const LinearProgressIndicator(),
          const SizedBox(height: 4),
          Text(
            'scanned ${scanner.scanned}  matched ${scanner.matched}  '
            'registered ${scanner.registered}  errors ${scanner.errors}',
            style: const TextStyle(fontSize: 11, color: Colors.grey),
          ),
        ],
      ),
    );
  }
}
