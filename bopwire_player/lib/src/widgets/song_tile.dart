import 'package:flutter/material.dart';
import '../models/song.dart';

class SongTile extends StatelessWidget {
  final Song song;
  final VoidCallback onTap;

  const SongTile({super.key, required this.song, required this.onTap});

  @override
  Widget build(BuildContext context) {
    return ListTile(
      leading: const CircleAvatar(child: Icon(Icons.music_note)),
      title:    Text(song.title.isEmpty ? '(untitled)' : song.title),
      subtitle: Text('${song.artist} · ${song.genre} · ${song.durationFormatted}'),
      trailing: Column(
        mainAxisAlignment: MainAxisAlignment.center,
        crossAxisAlignment: CrossAxisAlignment.end,
        children: [
          Text('${song.playCount} plays', style: const TextStyle(fontSize: 11)),
          Icon(
            song.rewardTierFull ? Icons.star : Icons.star_border,
            size: 16,
            color: song.rewardTierFull ? Colors.amber : Colors.grey,
          ),
        ],
      ),
      onTap: onTap,
    );
  }
}
