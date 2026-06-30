import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'library_scanner.dart';
import 'node_service.dart';
import 'rats_client.dart';

/// Pure-librats peer discovery. Asks the VPS mini-node for its current
/// routing table via the `routes.get` RPC, then sorts the returned full
/// nodes by `last_seen_ms` and picks the freshest one. No HTTP probes,
/// no DHT crawl.
class LibratsDiscovery extends ChangeNotifier {
  LibratsDiscovery() {
    _instance = this;
    _startPeriodicRefresh();
  }

  /// Lazy global so providers / services that can't easily reach the
  /// widget tree (LibraryProvider's RPC retry path, for one) can still
  /// trigger a forced rediscovery when an RPC fails.
  static LibratsDiscovery? _instance;
  static LibratsDiscovery? get current => _instance;

  /// Last route list pulled from the VPS, keyed by node_id.
  final Map<String, Map<String, dynamic>> routes = {};

  /// rats_peer_id of the currently-auto-selected full node.
  String autoSelectedRatsPeerId = '';

  /// Caller invoked when the auto-selected full node identity changes
  /// (first discovery, or it cycled to a different node). The player wires
  /// this to LibraryScanner.reAnnounce so swarm membership rebuilds when
  /// the full node restarts. Without this, a home-only restart left the
  /// player serving songs nobody knew about.
  void Function(String newHomePeerId)? onAutoNodeChanged;

  String _lastNotifiedNodePid = '';

  /// Best-effort HTTP URL, kept for compatibility with screens that still
  /// display "connected to <url>" — derived from the route's api_url field.
  String autoSelectedUrl = '';

  /// How we reach the auto-selected node: 'direct' when the mini-node's
  /// inbound probe confirmed it and a public address is known, 'relay'
  /// when every RPC has to tunnel via the VPS mini-node. Useful for the
  /// settings screen to show a "via tunnel" indicator.
  String autoSelectedReachability = 'unknown';

  String  lastError       = '';
  bool    isRefreshing    = false;
  String  vpsStatus       = '';

  /// True after the user taps "go offline" (disconnect()), false again once
  /// they tap Connect (forceReconnect()). Honored at the top of refresh() so a
  /// periodic tick already in flight — or the 20 s timer if it somehow
  /// survives — can't repopulate routes and silently drag the player back
  /// online after a deliberate disconnect.
  bool    _disconnected   = false;

  Timer? _timer;

  void _startPeriodicRefresh() {
    // Don't sleep-then-poke. Wait until the VPS rats handshake actually
    // completes before the first routes.get; on slow networks the 2-second
    // sleep used to fall short and we'd time out + sit empty for 30 s.
    _runWhenVpsReady();
    // Cellular carrier NAT closes idle UDP/TCP mappings in 30-60 s. The
    // 15s socket-keepalive defends one direction; this 20s discovery
    // refresh keeps the application-layer NAT entry warm bidirectionally.
    _timer = Timer.periodic(const Duration(seconds: 20), (_) {
      refresh();
      _bidirectionalProbe();
    });
  }

  /// Paired bidirectional probe to exercise the return path on the same
  /// 20s cadence as refresh(). routes.get is a one-way poll
  /// (player -> mini-node); without a complementary call the carrier NAT
  /// entry can age out in the reverse direction even while we keep
  /// hitting the forward direction. mini.ping is a tiny no-op verb on
  /// the mini-node that replies instantly; if it isn't implemented yet,
  /// fall back to mininodes.list which definitely exists and also
  /// returns small data.
  Future<void> _bidirectionalProbe() async {
    final rats = RatsClient.instance;
    // Probe the ACTIVE relay (the load-aware best mini-node) — that's the one
    // whose return-path NAT mapping actually carries our relay.forward traffic
    // and must be kept warm. firstMiniNodePeerId could name a different,
    // idle mini-node whose mapping we don't depend on.
    final mini = rats.bestMiniNodePeerId ?? rats.firstMiniNodePeerId;
    if (mini == null) return;
    try {
      await rats.request(mini, 'mini.ping', const {},
          timeout: const Duration(seconds: 4));
    } catch (_) {
      // mini.ping not implemented yet on the mini-node — fall back to
      // mininodes.list which is guaranteed to exist and is similarly
      // small. The exact verb doesn't matter; what matters is that the
      // reply traverses the NAT in the player->mini-node return
      // direction.
      try {
        await rats.request(mini, 'mininodes.list', const {},
            timeout: const Duration(seconds: 4));
      } catch (_) { /* both probes failed; next tick will retry */ }
    }
  }

  Future<void> _runWhenVpsReady() async {
    final rats = RatsClient.instance;
    for (int i = 0; i < 30; ++i) {
      if (rats.validatedPeerIds.isNotEmpty) break;
      await Future.delayed(const Duration(seconds: 1));
    }
    await refresh();
  }

  Future<void> refresh() async {
    // Honor a deliberate disconnect: a tick already queued before the user
    // went offline (or the periodic timer racing disconnect()) must not
    // repopulate routes / re-select a node behind their back.
    if (_disconnected) return;
    if (isRefreshing) return;
    isRefreshing = true;
    lastError    = '';
    vpsStatus    = 'Asking VPS for routes...';
    notifyListeners();

    try {
      final rats = RatsClient.instance;
      // ignore: avoid_print
      print('[discovery] refresh: validatedPeers=${rats.validatedPeerIds.length} '
            'mini=${rats.firstMiniNodePeerId ?? "<none>"} '
            'targetingMiniNode=${rats.firstMiniNodePeerId != null}');
      final list = await rats.requestRoutes();
      // ignore: avoid_print
      print('[discovery] refresh: got ${list.length} routes from mini-node');
      routes
        ..clear()
        ..addEntries(list
            .where((m) => (m['node_id'] as String? ?? '').isNotEmpty
                // Dead-node GC: drop nodes the relay reported unreachable so a
                // corpse can't be re-selected; it returns when it comes back.
                && !RatsClient.instance
                    .isPeerDead(m['rats_peer_id'] as String? ?? ''))
            .map((m) => MapEntry(m['node_id'] as String, m)));

      // Wire reachability into RatsClient's relay map. Nodes the mini-
      // node tagged "relay" are unreachable from outside their NAT —
      // their RPCs must be tunneled through the mini-node. Nodes
      // tagged "direct" are reachable so we send to them straight.
      // The relay anchor MUST be the mini-node, never a random
      // validated peer (post-pivot full nodes don't handle
      // relay.forward — they'd silently drop the tunneled body). If
      // mini.hello hasn't landed yet we leave relays unset rather
      // than guess; the next refresh tick re-runs this once the
      // mini-node identity is known.
      final vpsPeer = rats.firstMiniNodePeerId ?? '';
      for (final r in routes.values) {
        final pid   = r['rats_peer_id']   as String? ?? '';
        final reach = r['reachability']   as String? ?? 'unknown';
        final pub   = r['public_address'] as String? ?? '';
        if (pid.isEmpty) continue;
        // Direct only when the mini-node's probe confirmed inbound is open
        // AND we have a public address to dial. Anything else (relay /
        // unknown / no public_address) routes through the VPS tunnel.
        final canDirect = reach == 'direct' && pub.isNotEmpty;
        if (!canDirect && vpsPeer.isNotEmpty) {
          rats.setRelayVia(pid, vpsPeer);
        } else {
          rats.setRelayVia(pid, null);
        }
      }

      if (routes.isEmpty) {
        vpsStatus = 'No full nodes registered with VPS yet';
      } else {
        // Pick the lightest reachable full node.
        //
        // Score: lower is better.
        //   score = 1 + 2 * load_score   (clamp to 1..3)
        //         * (is_busy ? 4 : 1)    (busy peers heavily penalised)
        //         * staleness_penalty    (1 if seen <120s ago, climbing
        //                                 linearly toward 10 by 30 min)
        //
        // Ties (e.g. two fresh idle nodes) break on freshness.
        //
        // Until per-peer ping plumbing lands in RatsClient we treat
        // every peer as 50 ms away. When that arrives, multiply the
        // score by (ping_ms / 50). The structure is in place.
        const fallbackPingMs = 50;
        final candidates = routes.values
            .where((m) => (m['rats_peer_id'] as String? ?? '').isNotEmpty)
            .toList();
        if (candidates.isEmpty) {
          // Every route is missing a rats_peer_id — nothing we can talk to.
          // Falling back to routes.values here used to "pick" a node with an
          // empty peer id, clobber NodeService's auto-node, and still report
          // success in vpsStatus. Treat it the same as routes.isEmpty.
          vpsStatus = 'No reachable full nodes (routes missing peer ids)';
          return;
        }

        double scoreOf(Map m) {
          final loadScore = (m['load_score'] as num?)?.toDouble() ?? 0.0;
          final isBusy    = m['is_busy'] as bool? ?? false;
          final seenMs    = (m['last_seen_ms'] as int? ?? 0);
          final ageMs     = DateTime.now().millisecondsSinceEpoch - seenMs;
          final staleness = ageMs <= 120000
              ? 1.0
              : (1.0 + (ageMs - 120000) / 200000.0).clamp(1.0, 10.0);
          final loadPart  = 1.0 + 2.0 * loadScore.clamp(0.0, 1.0);
          final busyPart  = isBusy ? 4.0 : 1.0;
          return fallbackPingMs.toDouble() * loadPart * busyPart * staleness;
        }

        candidates.sort((a, b) {
          final cmp = scoreOf(a).compareTo(scoreOf(b));
          if (cmp != 0) return cmp;
          // Tiebreak on freshness.
          return ((b['last_seen_ms'] as int? ?? 0))
              .compareTo(a['last_seen_ms'] as int? ?? 0);
        });
        var pick = candidates.first;

        // Auto-node hysteresis. Flapping between two near-equal full nodes
        // every 20 s tears down + rebuilds direct connections and re-fires
        // onAutoNodeChanged (→ a full library re-announce) for no benefit.
        // Keep the current incumbent UNLESS a challenger beats it by a real
        // margin (>25% lower score) AND the incumbent is stale (not seen in
        // >120 s) or unreachable (missing from this route set). A challenger
        // that's only marginally better — or an incumbent that's still fresh —
        // does NOT win, so the selection stays sticky.
        final incumbentId = autoSelectedRatsPeerId;
        if (incumbentId.isNotEmpty &&
            (pick['rats_peer_id'] as String? ?? '') != incumbentId) {
          Map<String, dynamic>? incumbent;
          for (final m in candidates) {
            if ((m['rats_peer_id'] as String? ?? '') == incumbentId) {
              incumbent = m;
              break;
            }
          }
          if (incumbent != null) {
            final incScore = scoreOf(incumbent);
            final pickScore = scoreOf(pick);
            final incSeenMs = (incumbent['last_seen_ms'] as int? ?? 0);
            final incAgeMs  =
                DateTime.now().millisecondsSinceEpoch - incSeenMs;
            final incStale  = incAgeMs > 120000;
            // Beats-by-margin: challenger score is <75% of incumbent's.
            final beatsByMargin = pickScore < incScore * 0.75;
            if (!(beatsByMargin && incStale)) {
              // Incumbent holds: still reachable and either fresh or the
              // challenger isn't decisively better.
              pick = incumbent;
            }
          }
          // incumbent == null → it dropped out of the route set entirely
          // (unreachable); fall through and let the challenger win.
        }
        autoSelectedRatsPeerId   = pick['rats_peer_id'] as String? ?? '';
        autoSelectedUrl          = pick['api_url']      as String? ?? '';
        // Surface the chosen node's reachability so UI can show "via tunnel"
        // when relayed. The setRelayVia loop above already configured the
        // routing in RatsClient — this is purely a display hint.
        final pickReach = pick['reachability'] as String? ?? 'unknown';
        final pickPub   = pick['public_address'] as String? ?? '';
        autoSelectedReachability =
            (pickReach == 'direct' && pickPub.isNotEmpty) ? 'direct' : 'relay';

        await NodeService.updateAutoNode(ratsPeerId: autoSelectedRatsPeerId);

        // Notify listeners (LibraryScanner.reAnnounce hook in main.dart)
        // whenever the auto-selected full node id changes — first
        // discovery, or a different node won the freshness sort. With
        // SwarmIndex persistence this fires only on actual identity
        // changes, not every 30 s refresh.
        final identityChanged = autoSelectedRatsPeerId.isNotEmpty &&
                                autoSelectedRatsPeerId != _lastNotifiedNodePid;
        if (identityChanged) {
          _lastNotifiedNodePid = autoSelectedRatsPeerId;
          try { onAutoNodeChanged?.call(autoSelectedRatsPeerId); } catch (_) {}
        }

        // NO DIRECT CONNECTS. We used to dial the home node directly here (plus
        // a "direct-when-reachable" carve-out) so its peer id landed in
        // validatedPeerIds and request() could pick a direct path. That is gone:
        // request() now relays EVERY non-mini-node peer — the home node
        // included — through the least-busy mini-node, with failover, because
        // mini-nodes are rewarded for forwarding and direct links proved flaky
        // (half-open NAT/IPv6 sockets that the keepalive flapped, stranding
        // transfers). Dialing the home node here only created an idle socket
        // that flapped every ~20 s refresh. The mini-node mesh routes our
        // relay.forward frames to the home node, so there is nothing to dial.

        // Save peer ids for offline restart (mirrors old DHT cache).
        final prefs = await SharedPreferences.getInstance();
        await prefs.setStringList(
          'discovered_rats_peers',
          routes.values
              .map((m) => m['rats_peer_id'] as String? ?? '')
              .where((s) => s.isNotEmpty)
              .toList(),
        );

        final via = autoSelectedReachability == 'direct'
                    ? 'direct'
                    : 'tunneled via VPS';
        vpsStatus = '${routes.length} full node'
                    '${routes.length == 1 ? '' : 's'} via VPS '
                    '(selected: $via)';
      }
    } catch (e) {
      // ignore: avoid_print
      print('[discovery] refresh: FAILED — $e');
      lastError = e.toString();
      vpsStatus = 'VPS error: $e';
    } finally {
      isRefreshing = false;
      notifyListeners();
    }
  }

  /// User-initiated "go offline" path. Drops every connected librats
  /// peer and clears the auto-selected node so the UI shows "No nodes —
  /// tap Connect to search". Differs from forceReconnect() in that we
  /// do NOT immediately re-dial the VPS — the user explicitly asked to
  /// stop talking to peers. The next tap on Connect re-dials.
  Future<void> disconnect() async {
    // Flip the flag + kill the periodic timer FIRST so neither the 20 s tick
    // nor an in-flight refresh() can repopulate routes after we clear them.
    _disconnected = true;
    _timer?.cancel();
    _timer = null;

    final prefs = await SharedPreferences.getInstance();
    await prefs.remove('auto_node_url');
    await prefs.remove('auto_rats_peer_id');
    await prefs.remove('discovered_rats_peers');
    routes.clear();
    autoSelectedUrl          = '';
    autoSelectedRatsPeerId   = '';
    autoSelectedReachability = 'unknown';
    vpsStatus                = 'disconnected';
    notifyListeners();

    RatsClient.instance.disconnectAll();
  }

  Future<void> forceReconnect() async {
    // User tapped Connect — undo a prior deliberate disconnect so refresh()
    // runs again, and restart the periodic timer disconnect() cancelled.
    _disconnected = false;
    _timer ??= Timer.periodic(const Duration(seconds: 20), (_) {
      refresh();
      _bidirectionalProbe();
    });

    final prefs = await SharedPreferences.getInstance();
    await prefs.remove('auto_node_url');
    await prefs.remove('auto_rats_peer_id');
    await prefs.remove('discovered_rats_peers');
    routes.clear();
    autoSelectedUrl          = '';
    autoSelectedRatsPeerId   = '';
    autoSelectedReachability = 'unknown';
    notifyListeners();

    // Kick the librats client into re-dialing the VPS rendezvous. Without
    // this, clearing the prefs only resets UI state — the underlying
    // librats socket stays dead and refresh() returns nothing.
    RatsClient.instance.connect(kVpsHost, kVpsRatsPort);
    // Give the handshake up to a few seconds to land before asking for
    // routes; otherwise refresh() racing the connect produces a brief
    // "No nodes" flash before the watchdog tick recovers.
    for (int i = 0; i < 20; ++i) {
      if (RatsClient.instance.validatedPeerIds.isNotEmpty) break;
      await Future.delayed(const Duration(milliseconds: 250));
    }
    await refresh();

    // User explicitly tapped Connect — (re)declare our wallet<->peer presence
    // binding and re-publish the library so we're discoverable immediately.
    // The node may have restarted (clearing its in-memory presence map) or
    // evicted us when a relayed download saturated the mini-node; this rebinds
    // without waiting for the periodic heartbeat. reAnnounce() fires
    // presence.hello first, then the digest-gated library.delta + playlists.
    unawaited(LibraryScanner.instance.reAnnounce());
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }
}
