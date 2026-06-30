// Helpers that bundle "remove from local library" with all the side
// effects: deannouncing from the swarm so other peers stop asking us
// for the bytes, and deleting the on-disk file when it lives under our
// own downloads cache. Files in user-picked folders are left in place
// (the user added the folder expecting their files to stay where they
// were) — the row is just unhooked from the library.

import 'dart:async';
import 'dart:io';

import 'package:path_provider/path_provider.dart';

import 'library_scanner.dart';
import 'library_service.dart';

class DeleteResult {
  const DeleteResult({required this.fileDeleted});
  final bool fileDeleted;
}

class LocalLibraryActions {
  LocalLibraryActions._();
  static final LocalLibraryActions instance = LocalLibraryActions._();

  Future<Directory> _downloadsDir() async {
    final base = await getApplicationDocumentsDirectory();
    final dir  = Directory('${base.path}/downloads');
    if (!await dir.exists()) await dir.create(recursive: true);
    return dir;
  }

  /// Returns whether the on-disk file was actually deleted (true only
  /// when it lived under our downloads cache). The library row is
  /// always removed and the full node always told to drop us from the
  /// swarm so the disk-side outcome is the only thing the caller has
  /// to surface to the user.
  Future<DeleteResult> deleteEntry(LibraryEntry e) async {
    bool fileDeleted = false;
    final dlDir = await _downloadsDir();
    if (e.filePath.isNotEmpty) {
      final norm = _normalize(e.filePath);
      final dlNorm = _normalize(dlDir.path);
      if (norm.startsWith(dlNorm)) {
        try {
          final f = File(e.filePath);
          if (await f.exists()) {
            await f.delete();
            fileDeleted = true;
          }
        } catch (_) {/* best effort — if FS denies, just drop the row */}
      }
    }

    // Best-effort swarm.leave so peers stop coming to us for the bytes.
    // We send for BOTH our local content_hash AND the canonical (when
    // they differ) — the full node only ever keys on the value we sent
    // in fingerprint.submit, which was the local hash, but a parallel
    // leave for canonical is cheap and self-healing for any legacy rows.
    final hashes = <String>{e.contentHash};
    if (e.canonicalHash.isNotEmpty) hashes.add(e.canonicalHash);
    unawaited(LibraryScanner.instance.deannounce(hashes.toList()));

    await LibraryService.instance.remove(e.contentHash);
    return DeleteResult(fileDeleted: fileDeleted);
  }

  String _normalize(String p) {
    final s = p.replaceAll('\\', '/');
    if (s.length >= 2 && s[1] == ':') {
      return s[0].toLowerCase() + s.substring(1);
    }
    return s;
  }
}
