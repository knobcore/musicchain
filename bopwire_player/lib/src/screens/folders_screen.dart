// Folders screen: pushed from the My Library tab when the user taps
// the "+ folder" action. Lists the music folders the scanner is
// watching, lets the user add or remove them, and pops back to the
// library when closed.

import 'dart:async';
import 'dart:io' show Platform;

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:provider/provider.dart';

import '../services/library_scanner.dart';
import '../services/library_service.dart';

class FoldersScreen extends StatelessWidget {
  const FoldersScreen({super.key});

  Future<void> _pickFolder(BuildContext context) async {
    // Without storage permissions the picker opens but every browse
    // attempt returns "empty folder". Prompt first so the user only
    // sees the picker once they've granted access.
    final messenger = ScaffoldMessenger.maybeOf(context);
    await LibraryScanner.instance.ensureStoragePermissions();
    // ensureStoragePermissions returns void — it doesn't tell us whether
    // the user actually granted access. On Android, verify the request
    // succeeded; otherwise the picker would open, the user would pick a
    // folder, addFolder would persist it, and the scanner would silently
    // find zero files because File(...).list() returns empty without
    // READ_MEDIA_AUDIO / MANAGE_EXTERNAL_STORAGE.
    if (Platform.isAndroid) {
      final audio = await Permission.audio.status;
      final mgr   = await Permission.manageExternalStorage.status;
      if (!audio.isGranted && !mgr.isGranted) {
        messenger?.showSnackBar(const SnackBar(
          content: Text('Storage permission denied — grant '
              '"Music & audio" or "All files access" in system '
              'settings, then try adding a folder again.'),
          duration: Duration(seconds: 4),
        ));
        return;
      }
    }
    final path = await FilePicker.platform.getDirectoryPath();
    if (path == null) return;
    await LibraryService.instance.addFolder(path);
  }

  Future<void> _confirmRemove(BuildContext context, String folder) async {
    final affected = LibraryService.instance.entriesUnder(folder);
    final count = affected.length;
    final messenger = ScaffoldMessenger.maybeOf(context);
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Remove folder?'),
        content: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(folder, style: const TextStyle(
                fontFamily: 'monospace', fontSize: 12)),
            const SizedBox(height: 12),
            Text(
              count == 0
                  ? 'No songs in your local library came from this folder.'
                  : '$count song${count == 1 ? "" : "s"} from this folder '
                    'will be removed from your library and the full node '
                    'will be told you no longer have ${count == 1 ? "it" : "them"}. '
                    'The audio files on disk are NOT deleted. The chain '
                    'fingerprints stay registered — if anyone else '
                    'has the song, the swarm keeps working.',
            ),
          ],
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
            child: const Text('Remove'),
          ),
        ],
      ),
    );
    if (ok != true) return;

    final dropped = await LibraryService.instance.purgeFolder(folder);
    final hashes = dropped.map((e) => e.contentHash).toList();
    if (hashes.isNotEmpty) {
      unawaited(LibraryScanner.instance.deannounce(hashes));
    }
    messenger?.showSnackBar(SnackBar(
      content: Text(count == 0
          ? 'Folder removed.'
          : 'Folder removed; $count song'
            '${count == 1 ? "" : "s"} dropped from library.'),
      duration: const Duration(seconds: 2),
    ));
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Folders'),
        actions: [
          IconButton(
            tooltip: 'Add folder',
            icon: const Icon(Icons.create_new_folder_outlined),
            onPressed: () => _pickFolder(context),
          ),
        ],
      ),
      // Watch LibraryService so a successful addFolder / purgeFolder
      // re-renders this screen without the user having to pop and re-push.
      body: Consumer<LibraryService>(
        builder: (context, lib, _) {
          final folders = lib.folders;
          if (folders.isEmpty) {
            return Center(
              child: Padding(
                padding: const EdgeInsets.all(32),
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Icon(Icons.folder_off_outlined,
                         size: 64,
                         color: Theme.of(context)
                             .colorScheme.onSurfaceVariant),
                    const SizedBox(height: 16),
                    const Text(
                      'No folders added yet. Tap the folder-plus icon '
                      'above to pick a music folder. Files in every added '
                      'folder are fingerprinted in the background.',
                      textAlign: TextAlign.center,
                      style: TextStyle(color: Colors.grey),
                    ),
                  ],
                ),
              ),
            );
          }
          return ListView.separated(
            itemCount: folders.length,
            separatorBuilder: (_, __) => const Divider(height: 0),
            itemBuilder: (context, i) {
              final p = folders[i];
              return ListTile(
                leading: const Icon(Icons.folder_outlined),
                title: Text(p,
                            maxLines: 2,
                            overflow: TextOverflow.ellipsis,
                            style: const TextStyle(fontSize: 13)),
                trailing: IconButton(
                  icon: const Icon(Icons.delete_outline),
                  tooltip: 'Remove folder',
                  onPressed: () => _confirmRemove(context, p),
                ),
              );
            },
          );
        },
      ),
    );
  }
}
