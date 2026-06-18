// "Discover" tab — the user's window into every song on the chain.
//
// Layout: a SegmentedButton at the top toggles the browse axis between
// Artist and Genre (Album is no longer a top-level facet — it lives one
// level deeper inside whichever artist you drill into). A breadcrumb
// strip just below the toggle reflects the drill path; tap any segment
// to jump back to it. The body splits into two panes:
//
//   * Top pane — a ChoiceChip grid that drills down through the
//     hierarchy. Artist mode goes Artists → Albums; Genre mode goes
//     Genres → Artists → Albums.
//   * Bottom pane — the track list for whichever album the user has
//     tapped. Empty until an album is picked.
//
// A drag handle separates the two panes (when both are visible); the
// user can grab it and drag to give whichever side more room. Behaves
// like a desktop splitter on Windows and a bottom-sheet gesture on
// mobile — same code, different feel.

import 'dart:async';

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/song.dart';
import '../providers/download_provider.dart';
import '../providers/library_provider.dart';
import '../providers/player_provider.dart';
import '../providers/wallet_provider.dart';
import '../services/librats_discovery.dart';
import '../services/library_service.dart';
import '../services/node_client.dart';
import '../services/node_service.dart';
import '../services/rats_client.dart';
import 'dmca_screen.dart';

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

class LibraryScreen extends StatefulWidget {
  const LibraryScreen({super.key});

  @override
  State<LibraryScreen> createState() => _LibraryScreenState();
}

class _LibraryScreenState extends State<LibraryScreen> {
  _FacetMode _mode = _FacetMode.artist;

  // Drill state. Both null = root view; setting `_drillGenre` enters
  // an artists-of-genre view (Genre mode only); setting `_drillArtist`
  // enters an albums-of-artist view; setting `_selectedAlbum` opens
  // the tracks pane at the bottom.
  String? _drillGenre;
  String? _drillArtist;
  String? _selectedAlbum;

  /// Vertical split: fraction of body height the top pane occupies.
  /// Drag handle below the top pane updates this within a clamp so
  /// neither pane can ever fully collapse on the user.
  double _topFraction = 0.5;

  /// Subscription bookkeeping so the chain library auto-refreshes the
  /// moment the first full-node handshake lands (cold boot).
  String _lastSeenHomePid = '';
  LibratsDiscovery? _disc;

  /// Periodic poller — pulls a fresh songs.list every 20 s while the
  /// tab is alive, so a peer going offline causes their tracks to
  /// vanish without the user having to tap refresh. Cheap on the wire
  /// (one swarm-size scan per song on the full node, no chain reads)
  /// and indispensable now that swarm availability is strictly
  /// connection-state driven.
  Timer? _autoRefreshTimer;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      context.read<LibraryProvider>().refresh();
    });
    _autoRefreshTimer = Timer.periodic(const Duration(seconds: 20), (_) {
      if (!mounted) return;
      final lib = context.read<LibraryProvider>();
      if (lib.loading) return;
      lib.refresh();
    });
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    final disc = context.read<LibratsDiscovery>();
    if (!identical(disc, _disc)) {
      _disc?.removeListener(_onDiscoveryChanged);
      disc.addListener(_onDiscoveryChanged);
      _disc = disc;
    }
  }

  void _onDiscoveryChanged() {
    final pid = _disc?.autoSelectedRatsPeerId ?? '';
    if (pid.isEmpty || pid == _lastSeenHomePid) return;
    _lastSeenHomePid = pid;
    final lib = context.read<LibraryProvider>();
    if (lib.songs.isEmpty) lib.refresh();
  }

  @override
  void dispose() {
    _autoRefreshTimer?.cancel();
    _disc?.removeListener(_onDiscoveryChanged);
    super.dispose();
  }

  void _play(Song song, List<Song> playlist, int index) {
    final wallet = context.read<WalletProvider>().info;
    if (wallet == null) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Create a wallet first')),
      );
      return;
    }
    context.read<PlayerProvider>()
        .playPlaylist(playlist, index, wallet.address);
  }

  // ---- Bucket / sort helpers ------------------------------------------
  //
  // Tag spellings rarely line up — "FIDLAR" / "Fidlar", "Rock" / "rock"
  // etc. all collide on the same audio. We group case-insensitively
  // (`*KeyNorm`) and pick the most-frequent original spelling for
  // display so the chip shows what the user actually has.

  String _artistKey(Song s) {
    final t = s.artist.trim();
    return t.isEmpty ? 'Unknown Artist' : t;
  }
  String _artistKeyNorm(Song s) => _artistKey(s).toLowerCase();

  String _genreKey(Song s) {
    final t = s.genre.trim();
    return t.isEmpty ? 'Unknown Genre' : t;
  }
  String _genreKeyNorm(Song s) => _genreKey(s).toLowerCase();

  String _albumKey(Song s) {
    final t = s.album.trim();
    return t.isEmpty ? 'Singles' : t;
  }
  String _albumKeyNorm(Song s) => _albumKey(s).toLowerCase();

  int _sortAlpha(String a, String b) {
    final ua = a.startsWith('Unknown') || a == 'Singles';
    final ub = b.startsWith('Unknown') || b == 'Singles';
    if (ua && !ub) return 1;
    if (ub && !ua) return -1;
    return a.toLowerCase().compareTo(b.toLowerCase());
  }

  /// Return the earliest non-zero year across a set of songs, or 0 when
  /// none of them carry a year. Drives chronological album ordering.
  int _earliestYear(Iterable<Song> tracks) {
    int y = 0;
    for (final s in tracks) {
      if (s.year > 0 && (y == 0 || s.year < y)) y = s.year;
    }
    return y;
  }

  /// Pre-flight: collapse the full song list down to the bucket the
  /// current drill state needs.
  Iterable<Song> _drillFilter(List<Song> songs) {
    final wantedGenre  = _drillGenre?.toLowerCase();
    final wantedArtist = _drillArtist?.toLowerCase();
    return songs.where((s) {
      if (wantedGenre  != null && _genreKeyNorm(s) != wantedGenre)  return false;
      if (wantedArtist != null && _artistKeyNorm(s) != wantedArtist) return false;
      return true;
    });
  }

  // ---- Drill actions --------------------------------------------------

  void _selectMode(_FacetMode m) {
    setState(() {
      _mode = m;
      _drillGenre   = null;
      _drillArtist  = null;
      _selectedAlbum = null;
    });
  }

  void _onPillTapped(String key, _DrillLevel level) {
    setState(() {
      switch (level) {
        case _DrillLevel.genre:
          // Tapping the same genre again deselects.
          _drillGenre   = _drillGenre == key ? null : key;
          _drillArtist  = null;
          _selectedAlbum = null;
        case _DrillLevel.artist:
          _drillArtist  = _drillArtist == key ? null : key;
          _selectedAlbum = null;
        case _DrillLevel.album:
          _selectedAlbum = _selectedAlbum == key ? null : key;
      }
    });
  }

  void _crumbBack(int targetDepth) {
    setState(() {
      // targetDepth 0 = root, 1 = first crumb, 2 = second crumb
      if (targetDepth < 3) _selectedAlbum = null;
      if (targetDepth < 2) _drillArtist   = null;
      if (targetDepth < 1) _drillGenre    = null;
    });
  }

  // ---- Build ----------------------------------------------------------

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Discover'),
        actions: [
          IconButton(
            icon: const Icon(Icons.refresh),
            onPressed: () => context.read<LibraryProvider>().refresh(),
          ),
        ],
      ),
      body: Consumer<LibraryProvider>(
        builder: (context, lib, _) {
          if (lib.loading && lib.songs.isEmpty) {
            return const Center(child: CircularProgressIndicator());
          }
          if (lib.error != null && lib.songs.isEmpty) {
            return Center(child: Text('Error: ${lib.error}'));
          }
          if (lib.songs.isEmpty) {
            return const Center(
              child: Text('No songs on chain yet — scan a folder to upload.'),
            );
          }
          return Column(
            children: [
              _ModeToolbar(
                mode: _mode,
                onChange: _selectMode,
              ),
              _Breadcrumb(
                segments: _breadcrumb(),
              ),
              Expanded(child: _splitBody(lib)),
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

  Widget _splitBody(LibraryProvider lib) {
    // Use filteredSongs so the rendered library hides entries whose
    // serving peer is currently offline (LibraryProvider.filteredSongs
    // drops swarmSize == 0). Without this, a song that was uploaded
    // by a phone that later disconnected stays visible but unstreamable
    // — the original "songs stay in list when user disconnects" bug.
    final live = lib.filteredSongs;
    final wantedAlbum = _selectedAlbum?.toLowerCase();
    final selectedTracks = wantedAlbum == null
        ? const <Song>[]
        : _drillFilter(live)
            .where((s) => _albumKeyNorm(s) == wantedAlbum)
            .toList();

    return LayoutBuilder(
      builder: (context, constraints) {
        final totalH = constraints.maxHeight;
        // When no album is selected the bottom pane collapses; the top
        // pane gets all the room. Once a pick lands the drag handle
        // springs up and the two share according to _topFraction.
        final hasBottom = _selectedAlbum != null;
        final topH = hasBottom ? totalH * _topFraction : totalH;
        final bottomH = hasBottom ? totalH - topH - _kHandleHeight : 0.0;
        return Stack(
          children: [
            Positioned(
              top: 0,
              left: 0,
              right: 0,
              height: topH,
              child: _topPane(lib),
            ),
            if (hasBottom) ...[
              Positioned(
                top: topH,
                left: 0,
                right: 0,
                height: _kHandleHeight,
                child: _DragHandle(
                  onDelta: (dy) {
                    setState(() {
                      _topFraction = (_topFraction + dy / totalH)
                          .clamp(0.20, 0.80);
                    });
                  },
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
                  tracks:         _TrackPane.sortTracks(selectedTracks),
                  onPlay:         _play,
                  onClose:        () => setState(() => _selectedAlbum = null),
                ),
              ),
            ],
          ],
        );
      },
    );
  }

  Widget _topPane(LibraryProvider lib) {
    final level = _currentLevel();
    final pills = _pillsFor(level, lib);
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

  List<Widget> _pillsFor(_DrillLevel level, LibraryProvider lib) {
    // Same rule as _splitBody — pills are derived from songs whose
    // serving peer is currently online so we don't pop up genre/artist
    // chips that resolve to zero playable tracks.
    final scoped = _drillFilter(lib.filteredSongs).toList();
    switch (level) {
      case _DrillLevel.genre:
        final buckets = _bucketByNorm(scoped, _genreKey, _genreKeyNorm);
        final keys = buckets.keys.toList()..sort(_sortAlpha);
        return [
          for (final k in keys)
            _DiscoverChip(
              icon:     Icons.style_outlined,
              label:    '$k  (${buckets[k]!.length})',
              selected: false,
              onTap:    () => _onPillTapped(k, _DrillLevel.genre),
              onContextMenu: (pos) => _menuForBucket(pos, k, buckets[k]!),
            ),
        ];
      case _DrillLevel.artist:
        final buckets = _bucketByNorm(scoped, _artistKey, _artistKeyNorm);
        final keys = buckets.keys.toList()..sort(_sortAlpha);
        return [
          for (final k in keys)
            _DiscoverChip(
              icon:     Icons.person_outline,
              label:    '$k  (${buckets[k]!.length})',
              selected: false,
              onTap:    () => _onPillTapped(k, _DrillLevel.artist),
              onContextMenu: (pos) => _menuForBucket(pos, k, buckets[k]!),
            ),
        ];
      case _DrillLevel.album:
        final buckets = _bucketByNorm(scoped, _albumKey, _albumKeyNorm);
        // Albums ordered chronologically by earliest tagged year; albums
        // without any year fall to the end alphabetically.
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
            _DiscoverChip(
              icon:     Icons.album_outlined,
              label:    _formatAlbumLabel(k, buckets[k]!),
              selected: _selectedAlbum?.toLowerCase() == k.toLowerCase(),
              onTap:    () => _onPillTapped(k, _DrillLevel.album),
              onContextMenu: (pos) => _menuForBucket(pos, k, buckets[k]!),
            ),
        ];
    }
  }

  /// Group [items] by `norm(item)` and pick the most-common spelling of
  /// `display(item)` as the bucket key — so two tag variants like
  /// "FIDLAR" and "Fidlar" collapse to one chip with the spelling that
  /// appears most often in the user's library.
  Map<String, List<Song>> _bucketByNorm(
      List<Song> items,
      String Function(Song) display,
      String Function(Song) norm) {
    final normToVariants = <String, Map<String, int>>{};
    final normToTracks   = <String, List<Song>>{};
    for (final s in items) {
      final n = norm(s);
      final d = display(s);
      (normToTracks[n] ??= []).add(s);
      (normToVariants[n] ??= <String, int>{})[d] =
          (normToVariants[n]![d] ?? 0) + 1;
    }
    final out = <String, List<Song>>{};
    normToVariants.forEach((n, variants) {
      // Pick the spelling with the most occurrences; ties break in
      // favor of the longer string (so "FIDLAR" beats "fidlar"
      // when both appear once).
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

  String _formatAlbumLabel(String name, List<Song> tracks) {
    final year = _earliestYear(tracks);
    final n = tracks.length;
    if (year > 0) return '$name  ($year · $n)';
    return '$name  ($n)';
  }

  Future<void> _menuForBucket(Offset position,
                              String label,
                              List<Song> tracks) async {
    final overlay = Overlay.of(context).context.findRenderObject() as RenderBox;
    final picked = await showMenu<String>(
      context: context,
      position: RelativeRect.fromRect(
        position & const Size(40, 40),
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
          value: 'download',
          child: ListTile(
            dense: true,
            leading: Icon(Icons.download_for_offline_outlined, size: 18),
            title: Text('Download'),
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
    if (picked == 'play' && tracks.isNotEmpty) {
      final sorted = _TrackPane.sortTracks(tracks);
      _play(sorted.first, sorted, 0);
    } else if (picked == 'download') {
      DownloadProvider.instance.enqueueAlbum(label, tracks);
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
        content: Text('Queued ${tracks.length} track'
                      '${tracks.length == 1 ? "" : "s"} from $label'),
        duration: const Duration(seconds: 2),
      ));
    } else if (picked == 'dmca' && mounted) {
      Navigator.of(context).push(MaterialPageRoute(
        builder: (_) => const DmcaScreen(),
      ));
    }
  }
}

// ---- Internals ---------------------------------------------------------

enum _DrillLevel { genre, artist, album }

class _CrumbSeg {
  _CrumbSeg({required this.label, this.onTap});
  final String        label;
  final VoidCallback? onTap;
}

const double _kHandleHeight = 14;

class _ModeToolbar extends StatelessWidget {
  const _ModeToolbar({required this.mode, required this.onChange});
  final _FacetMode              mode;
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

class _DiscoverChip extends StatelessWidget {
  const _DiscoverChip({
    required this.icon,
    required this.label,
    required this.selected,
    required this.onTap,
    required this.onContextMenu,
  });
  final IconData                   icon;
  final String                     label;
  final bool                       selected;
  final VoidCallback               onTap;
  final void Function(Offset pos)  onContextMenu;

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      onSecondaryTapDown: (d) => onContextMenu(d.globalPosition),
      onLongPressStart:   (d) => onContextMenu(d.globalPosition),
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
  final String           albumName;
  final String?          artistFallback;
  final List<Song>       tracks;
  final void Function(Song song, List<Song> playlist, int idx) onPlay;
  final VoidCallback     onClose;

  static List<Song> sortTracks(List<Song> tracks) {
    final out = [...tracks];
    out.sort((a, b) {
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

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final artist = (tracks.isNotEmpty && tracks.first.artist.trim().isNotEmpty)
        ? tracks.first.artist
        : (artistFallback ?? '');
    final year = tracks
        .map((s) => s.year)
        .firstWhere((y) => y > 0, orElse: () => 0);
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
              itemBuilder: (context, i) => _SongRow(
                song:     tracks[i],
                playlist: tracks,
                index:    i,
                onPlay:   onPlay,
              ),
            ),
          ),
        ],
      ),
    );
  }
}

class _SongRow extends StatefulWidget {
  const _SongRow({
    required this.song,
    required this.playlist,
    required this.index,
    required this.onPlay,
  });
  final Song                                                  song;
  final List<Song>                                            playlist;
  final int                                                   index;
  final void Function(Song song, List<Song> playlist, int idx) onPlay;

  @override
  State<_SongRow> createState() => _SongRowState();
}

class _SongRowState extends State<_SongRow> {
  bool _localFor(String contentHash) {
    final e = LibraryService.instance.entryByHash(contentHash);
    return e != null && e.isLocal;
  }

  Future<void> _download() async {
    final messenger = ScaffoldMessenger.maybeOf(context);
    try {
      final pid = await NodeService.getRatsPeerId(
          waitFor: const Duration(seconds: 8));
      if (pid.isEmpty) {
        throw StateError('No full node discovered yet.');
      }
      final client = NodeClient(ratsPeerId: pid);

      final variants = await client
          .lookupSwarmVariants(widget.song.contentHash);
      SwarmVariant? chosen;
      if (variants.length > 1) {
        final dedup = _distinctByQuality(variants);
        if (dedup.length > 1 && mounted) {
          chosen = await _pickQualityDialog(dedup);
          if (chosen == null) return;
        } else {
          chosen = dedup.isNotEmpty ? dedup.first : null;
        }
      } else if (variants.length == 1) {
        chosen = variants.first;
      }

      DownloadProvider.instance.enqueueSong(
        widget.song,
        variant: chosen,
      );
      messenger?.showSnackBar(SnackBar(
        content: Text('Queued "${widget.song.title.isEmpty
            ? widget.song.contentHash.substring(0, 8)
            : widget.song.title}"'
            '${chosen != null ? " (${chosen.qualityLabel})" : ""}'),
        duration: const Duration(seconds: 2),
      ));
    } catch (e) {
      messenger?.showSnackBar(SnackBar(
        content: Text('Download failed: $e'),
        duration: const Duration(seconds: 4),
      ));
    }
  }

  List<SwarmVariant> _distinctByQuality(List<SwarmVariant> variants) {
    final groups = <String, Map<String, List<SwarmVariant>>>{};
    for (final v in variants) {
      groups.putIfAbsent(v.qualityKey, () => {});
      groups[v.qualityKey]!.putIfAbsent(v.contentHash, () => []).add(v);
    }
    final result = <SwarmVariant>[];
    for (final byHash in groups.values) {
      MapEntry<String, List<SwarmVariant>>? best;
      for (final entry in byHash.entries) {
        if (best == null || entry.value.length > best.value.length) {
          best = entry;
        }
      }
      if (best != null) result.add(best.value.first);
    }
    result.sort((a, b) {
      if (a.qualityBucketKbps == 0 && b.qualityBucketKbps != 0) return 1;
      if (b.qualityBucketKbps == 0 && a.qualityBucketKbps != 0) return -1;
      return a.qualityBucketKbps.compareTo(b.qualityBucketKbps);
    });
    return result;
  }

  Future<SwarmVariant?> _pickQualityDialog(List<SwarmVariant> options) {
    return showDialog<SwarmVariant>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Download quality'),
        content: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            for (final v in options)
              ListTile(
                dense: true,
                leading: const Icon(Icons.high_quality, size: 20),
                title: Text(v.qualityLabel),
                subtitle: Text(
                  'from ${v.peerId.substring(0, 12)}…',
                  style: const TextStyle(fontSize: 11),
                ),
                onTap: () => Navigator.pop(ctx, v),
              ),
          ],
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(ctx),
            child: const Text('Cancel'),
          ),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final s = widget.song;
    final isLocal = _localFor(s.contentHash);
    final dl = context.watch<DownloadProvider>();
    DownloadJob? activeJob;
    for (final j in dl.activeJobs) {
      if (j.song.contentHash == s.contentHash) {
        activeJob = j;
        break;
      }
    }

    Widget trailing;
    if (isLocal) {
      trailing = const Icon(Icons.check_circle_outline,
          size: 18, color: Colors.green);
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
      trailing = IconButton(
        tooltip: 'Download to library',
        icon: const Icon(Icons.download_for_offline_outlined, size: 18),
        onPressed: _download,
      );
    }

    final playStr = _formatPlayCount(s.playCount);
    return ListTile(
      dense: true,
      leading: SizedBox(
        width: 28,
        child: Text(
          s.trackNumber > 0 ? '${s.trackNumber}' : '${widget.index + 1}',
          textAlign: TextAlign.right,
          style: Theme.of(context).textTheme.bodySmall,
        ),
      ),
      title: Text(
        s.title.isEmpty ? '(untitled)' : s.title,
        maxLines: 1, overflow: TextOverflow.ellipsis,
      ),
      subtitle: Text(
        '${s.durationFormatted}  •  ▶ $playStr',
        style: const TextStyle(fontSize: 11),
      ),
      onTap: () => widget.onPlay(s, widget.playlist, widget.index),
      trailing: SizedBox(width: 40, child: trailing),
    );
  }

  /// Compact play-count display. Anything under 1K renders as-is; larger
  /// counts collapse to "12.3K", "4.5M" so the chip stays width-stable
  /// next to the duration even on viral tracks.
  static String _formatPlayCount(int n) {
    if (n < 1000) return '$n';
    if (n < 1000000) {
      final v = n / 1000.0;
      return v >= 10 ? '${v.toStringAsFixed(0)}K' : '${v.toStringAsFixed(1)}K';
    }
    final v = n / 1000000.0;
    return v >= 10 ? '${v.toStringAsFixed(0)}M' : '${v.toStringAsFixed(1)}M';
  }
}
