// Tiny loopback HTTP server that bridges the librats binary stream into
// a URL media_kit (libmpv) can consume. The whole point is latency: with
// the file-on-disk path the player waits for the full download to finish
// before media_kit even opens; here media_kit gets the URL the moment
// the first chunk arrives, libmpv reads as bytes flow, and audio starts
// within a fraction of a second of `Play` instead of after the file is
// fully cached.
//
// Wire shape:
//   1. NodeClient.fetchAudioToCache calls AudioStreamProxy.serve(stream)
//      which registers the AudioStream under a short ticket id and
//      returns http://127.0.0.1:<port>/<ticket>.
//   2. media_kit issues GET on the URL.
//   3. The handler advertises Content-Length = stream.totalBytes, then
//      pipes chunks into the response body until the stream closes.
//   4. If media_kit disconnects (user hit Next), the response sink raises,
//      we cancel the AudioStream, and the librats download winds down.

import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'rats_client.dart';

class AudioStreamProxy {
  AudioStreamProxy._();
  static final AudioStreamProxy instance = AudioStreamProxy._();

  HttpServer? _server;
  int         _port = 0;
  int         _nextTicket = 1;
  final Map<String, AudioStream> _pending = {};

  /// The stream most recently handed to media_kit (the currently-playing
  /// track). Tracked so a track switch can cancel it synchronously BEFORE the
  /// next track's stream.open goes out — see [cancelCurrent].
  AudioStream? _current;

  /// Idempotent. Binds a random loopback port on first call.
  Future<void> ensureStarted() async {
    if (_server != null) return;
    final s = await HttpServer.bind(InternetAddress.loopbackIPv4, 0,
                                    shared: false);
    _port = s.port;
    _server = s;
    // ignore: avoid_print
    print('[stream-proxy] listening on 127.0.0.1:$_port');
    s.listen(_handle, cancelOnError: false);
  }

  /// Register [stream] under a fresh ticket and return the URL media_kit
  /// should open. The ticket is single-use: the handler pulls the stream
  /// out of `_pending` on first GET and serves it; a refresh would 404.
  String serve(AudioStream stream) {
    _current = stream;
    final id = '${DateTime.now().millisecondsSinceEpoch}-${_nextTicket++}';
    _pending[id] = stream;
    return 'http://127.0.0.1:$_port/$id';
  }

  /// Cancel the currently-serving stream (the playing track), if any. Called at
  /// the top of a new play so the previous track's serving peer is told to stop
  /// pushing (AudioStream.cancel → stream.cancel RPC) BEFORE the next track's
  /// stream.open goes out — otherwise the leaked push starves the new stream on
  /// the single relay link (the "skip → next track hangs ~30 s" bug).
  void cancelCurrent() {
    final c = _current;
    _current = null;
    if (c != null) {
      try { c.cancel(); } catch (_) {/* teardown must never throw */}
    }
  }

  Future<void> _handle(HttpRequest req) async {
    // Path is "/<ticket>" — strip the leading slash and look it up.
    final id = req.uri.pathSegments.isEmpty ? '' : req.uri.pathSegments.first;
    final stream = _pending.remove(id);
    if (stream == null) {
      req.response.statusCode = HttpStatus.notFound;
      await req.response.close();
      return;
    }

    final resp = req.response;
    resp.statusCode = HttpStatus.ok;
    // libmpv uses Content-Length to compute the track's duration before
    // any decoded frames land — without it the seek bar stays at 0:00 /
    // unknown for the first few hundred ms.
    resp.headers.contentLength = stream.totalBytes;
    // Content-Type left generic; libmpv sniffs the audio container from
    // the bytes themselves (mpv handles mp3/ogg/m4a transparently).
    resp.headers.set('Cache-Control', 'no-store');

    StreamSubscription<Uint8List>? sub;
    bool clientGone = false;
    final clientGoneCompleter = Completer<void>();

    // Detect early disconnect so we can stop pulling chunks for a song
    // the user already skipped.
    resp.done.then((_) {}, onError: (_) {
      clientGone = true;
      if (!clientGoneCompleter.isCompleted) clientGoneCompleter.complete();
    });

    sub = stream.bytes.listen(
      (chunk) {
        if (clientGone) return;
        try {
          resp.add(chunk);
        } catch (_) {
          clientGone = true;
          if (!clientGoneCompleter.isCompleted) clientGoneCompleter.complete();
        }
      },
      onError: (_) {
        if (!clientGoneCompleter.isCompleted) clientGoneCompleter.complete();
      },
      onDone: () {
        if (!clientGoneCompleter.isCompleted) clientGoneCompleter.complete();
      },
      cancelOnError: true,
    );

    await clientGoneCompleter.future;
    await sub.cancel();
    if (clientGone) stream.cancel();
    try { await resp.close(); } catch (_) {}
  }

  Future<void> dispose() async {
    final s = _server;
    _server = null;
    if (s != null) await s.close(force: true);
    for (final stream in _pending.values) {
      stream.cancel();
    }
    _pending.clear();
  }
}
