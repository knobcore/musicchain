import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:media_kit/media_kit.dart';
import 'package:provider/provider.dart';

import 'src/ffi/native_library.dart';
import 'src/providers/player_provider.dart';
import 'src/providers/wallet_provider.dart';
import 'src/providers/library_provider.dart';
import 'src/providers/download_provider.dart';
import 'src/services/librats_discovery.dart';
import 'src/services/rats_client.dart';
import 'src/services/player_server.dart';
import 'src/services/swarm_registry.dart';
import 'src/services/audio_stream_proxy.dart';
import 'src/services/library_service.dart';
import 'src/services/library_scanner.dart';
import 'src/services/presence_publisher.dart';
import 'src/services/chat_service.dart';
import 'src/services/background_scanner.dart';
import 'src/services/wallet_service.dart';
import 'src/services/offline_play_log/heartbeat_capture.dart';
import 'src/services/offline_play_log/network_transition_watcher.dart';
import 'src/services/offline_play_log/sensor_capture.dart';
import 'src/services/offline_play_log/offline_submit_service.dart';
import 'src/screens/home_screen.dart';
import 'src/screens/wallet_gate.dart';
import 'src/smoke_test.dart';

void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  MediaKit.ensureInitialized();

  // Initialize native library
  await NativeLibrary.initialize();

  // Bring up the librats client — connects to the VPS rendezvous so this
  // player joins the mesh and can stream to/from other players' libraries.
  // Failures here are non-fatal (offline mode still shows the local library)
  // so we just log and continue.
  // Warm up the local library so the My Library tab can render its
  // folder list / entries on first frame even if the rats stack later
  // fails to come up.
  try {
    await LibraryService.instance.ensureLoaded();
  } catch (e) {
    // ignore: avoid_print
    print('[library] init failed: $e');
  }

  // wallet-as-id: load the wallet BEFORE rats so its librats peer id can be
  // pinned to the wallet's chain address (stable identity across restarts).
  // First run (no wallet yet) falls back to a generated id this session and
  // pins to the wallet on the next launch once the wallet gate creates one.
  String? walletAddr;
  try {
    final wi = await WalletService().tryAutoLoad();
    walletAddr = wi?.address;
  } catch (e) {
    // ignore: avoid_print
    print('[wallet] pre-rats load failed: $e');
  }

  try {
    final rats = await RatsClient.initialize(walletAddress: walletAddr);
    // ignore: avoid_print
    print('[rats] connected; own_peer_id=${rats.ownPeerId} '
          'public=${rats.publicAddress}');

    // Hook the player-side server for incoming stream.open / library.list
    // requests from other peers.
    await PlayerServer.initialize();

    // Claim RatsClient.onPush for chat so pushed chat.message / chat.kicked
    // envelopes are captured app-wide, even before the Chat tab is first
    // opened. ChatScreen also calls ensureWired() defensively on mount.
    ChatService.instance.ensureWired();

    // Offline play-proof bundle pipeline. See
    // docs/offline_play_proof.md for the threat model + wire format.
    // We bring up the capture layer here so HeartbeatService can mirror
    // every beat into the persistent log, then the transition watcher
    // and sensor capture, then the submitter (which holds its own
    // wallet handle so it can sign bundles independent of the UI's
    // WalletProvider lifecycle).
    try {
      await HeartbeatCapture.instance.init();
      await NetworkTransitionWatcher.instance.start();
      await SensorCapture.instance.start();
      // Submit service signs with the player's wallet. We instantiate a
      // dedicated WalletService and kick its keychain-cached auto-load
      // so the signing key is ready by the time the first bundle ticks.
      final submitWallet = WalletService();
      unawaited(submitWallet.tryAutoLoad());
      await OfflineSubmitService.instance.start(wallet: submitWallet);
      // #10: reuse this loaded wallet to sign relay.receipt messages.
      // Best-effort — if the key isn't loaded yet when a receipt fires,
      // _sendRelayReceipt silently skips (no reward, no crash).
      rats.wallet = submitWallet;
    } catch (e) {
      // ignore: avoid_print
      print('[offline-play-log] init failed: $e');
    }

    // Spin up the SwarmRegistry: tracks the local-library content hashes
    // we hold, announces each to the librats DHT, and resolves DHT
    // queries so PieceDownloader can find peer sources without going
    // through the VPS swarm-locate fallback.
    await SwarmRegistry.initialize();

    // Pre-bind the loopback HTTP proxy that bridges swarm streams into
    // media_kit. Doing it now avoids paying the HttpServer.bind cost on
    // the first Play tap.
    await AudioStreamProxy.instance.ensureStarted();

    // When the VPS handshake is re-established (after a VPS or full-node
    // restart), re-submit our library so the full node's in-memory swarm
    // map gets repopulated. Without this, other players asking the home
    // node "who has X?" get an empty list and the song appears unfetchable.
    rats.onVpsReconnected = () {
      // ignore: avoid_print
      print('[rats] VPS reconnected — re-announcing library to full node');
      unawaited(LibraryScanner.instance.reAnnounce());
    };

    // Keep the wallet<->peer presence binding alive with a periodic
    // presence.hello. onVpsReconnected only fires on connection transitions, so
    // a presence eviction that DIDN'T drop the VPS handshake (e.g. the node
    // restarting, or a relayed download saturating the mini-node) would leave us
    // bound-less — our 474 songs vanishing from every other player's Discover —
    // until something re-triggered reAnnounce. The heartbeat rebinds within one
    // ~25 s tick regardless. Cheap (~150 B) and a no-op until a wallet loads.
    PresencePublisher.startHeartbeat();
  } catch (e) {
    // ignore: avoid_print
    print('[rats] init failed: $e');
  }

  // Background scanning currently only has an Android workmanager backend.
  // On other platforms the package throws UnimplementedError; we'd just
  // crash startup if we let that propagate.
  if (Platform.isAndroid) {
    try {
      await BackgroundScanner.initialize();
    } catch (e) {
      // ignore: avoid_print
      print('[bgscan] init failed: $e');
    }
  }

  runApp(const MusicChainApp());

  // Headless network-stack smoke test. Compiled in; only runs with
  // --dart-define=SMOKE_TEST=true. Scheduled post-first-frame so the provider
  // tree (LibratsDiscovery, which populates the full-node readiness signal) has
  // constructed before readiness polling. runSmokeTest() ends with exit(0|1).
  if (const bool.fromEnvironment('SMOKE_TEST')) {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      unawaited(runSmokeTest());
    });
  }
}

class MusicChainApp extends StatelessWidget {
  const MusicChainApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MultiProvider(
      providers: [
        ChangeNotifierProvider(
          // Eager. WalletProvider's constructor is the only place that
          // sets the static `_active` (used by PlayerProvider's silent
          // session-complete path to refresh balance after a mint),
          // kicks `_tryAutoLoad()` (which sets
          // LibraryScanner.artistAddress so re-announces credit the
          // right publisher), and starts the 20 s defensive balance
          // refresh timer. With lazy:true, a user whose wallet auto-
          // unlocks via WalletGate goes straight to HomeScreen
          // without anything reading WalletProvider — so balance
          // stays stale, artist_address ships empty on the first
          // re-announce, and refreshNow() silently no-ops until the
          // user finally taps the Wallet tab.
          lazy: false,
          create: (_) => WalletProvider(),
        ),
        ChangeNotifierProvider(create: (_) => LibraryProvider()),
        ChangeNotifierProvider(create: (_) => PlayerProvider()),
        ChangeNotifierProvider.value(value: LibraryService.instance),
        ChangeNotifierProvider.value(value: DownloadProvider.instance),
        ChangeNotifierProvider.value(value: ChatService.instance),
        ChangeNotifierProvider(
          // Eager. Without lazy:false the provider only constructs the
          // LibratsDiscovery the first time some widget watches it. The
          // login screen (shown when the user has a saved wallet on
          // disk) doesn't watch it, so the discovery loop never starts
          // and the player sits with no full-node handshake until the
          // user navigates to a screen that DOES watch. Wallet
          // first-launch banner needs the discovery state live; that
          // alone is reason enough to construct on app start.
          lazy: false,
          create: (_) {
            final disc = LibratsDiscovery();
            // Whenever the auto-selected full node changes (first
            // discovery or it cycled to a different node), re-announce
            // our library so the full node's swarm map picks us back
            // up.
            disc.onAutoNodeChanged = (nodePid) {
              // ignore: avoid_print
              print('[rats] auto-node changed to '
                    '${nodePid.substring(0, 12)} — re-announcing library');
              unawaited(LibraryScanner.instance.reAnnounce());
            };
            return disc;
          },
        ),
      ],
      child: MaterialApp(
        title: 'MusicChain',
        theme: ThemeData(
          colorScheme: ColorScheme.fromSeed(
            seedColor: const Color(0xFF6200EE),
            brightness: Brightness.dark,
          ),
          useMaterial3: true,
        ),
        // WalletGate runs the first-launch / login decision tree
        // before handing off to HomeScreen; once the wallet is
        // unlocked it just renders the child.
        home: const WalletGate(child: HomeScreen()),
        debugShowCheckedModeBanner: false,
      ),
    );
  }
}
