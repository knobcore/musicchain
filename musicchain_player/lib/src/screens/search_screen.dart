import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../providers/library_provider.dart';
import '../providers/player_provider.dart';
import '../providers/wallet_provider.dart';
import '../widgets/song_tile.dart';

enum _SearchType { all, artist, genre }

class SearchScreen extends StatefulWidget {
  const SearchScreen({super.key});

  @override
  State<SearchScreen> createState() => _SearchScreenState();
}

class _SearchScreenState extends State<SearchScreen> {
  final _controller = TextEditingController();
  _SearchType _searchType = _SearchType.all;

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  void _doSearch() {
    final q = _controller.text.trim();
    if (q.isEmpty) return;
    final lib = context.read<LibraryProvider>();
    switch (_searchType) {
      case _SearchType.all:    lib.search(q);
      case _SearchType.artist: lib.searchByArtist(q);
      case _SearchType.genre:  lib.searchByGenre(q);
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: TextField(
          controller: _controller,
          decoration: const InputDecoration(
            hintText: 'Search songs...',
            border: InputBorder.none,
          ),
          onSubmitted: (_) => _doSearch(),
        ),
        actions: [
          IconButton(
            icon: const Icon(Icons.search),
            onPressed: _doSearch,
          ),
        ],
      ),
      body: Column(
        children: [
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 4),
            child: Wrap(
              spacing: 8,
              children: [
                for (final t in _SearchType.values)
                  ChoiceChip(
                    label: Text(
                      t == _SearchType.all
                          ? 'All'
                          : t == _SearchType.artist
                              ? 'Artist'
                              : 'Genre',
                    ),
                    selected: _searchType == t,
                    onSelected: (_) => setState(() => _searchType = t),
                  ),
              ],
            ),
          ),
          Expanded(
            child: Consumer<LibraryProvider>(
              builder: (context, lib, _) {
                if (lib.loading) {
                  return const Center(child: CircularProgressIndicator());
                }
                if (lib.songs.isEmpty) {
                  return const Center(child: Text('Search for a song above.'));
                }
                return ListView.builder(
                  itemCount: lib.songs.length,
                  itemBuilder: (context, i) {
                    final song = lib.songs[i];
                    return SongTile(
                      song: song,
                      onTap: () {
                        final wallet = context.read<WalletProvider>().info;
                        if (wallet == null || wallet.address.isEmpty) return;
                        // Playback state lives in the bottom MiniPlayer —
                        // no separate now-playing screen to push.
                        context.read<PlayerProvider>().play(song, wallet.address);
                      },
                    );
                  },
                );
              },
            ),
          ),
        ],
      ),
    );
  }
}
