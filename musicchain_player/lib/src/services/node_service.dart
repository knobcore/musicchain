import 'package:shared_preferences/shared_preferences.dart';

/// Tracks the rats peer id of the currently auto-selected full node so
/// providers spun up before [LibratsDiscovery]'s first `routes.get` cycle
/// can block on its result instead of starting empty.
///
/// The old `node_url` / `auto_node_url` fields were dropped when the home
/// node stopped serving HTTP; there's nothing for a URL to point at now.
class NodeService {
  /// Poll a shared-preferences key with an exponential backoff until it
  /// has a non-empty value or [budget] expires. We start at 50 ms (so
  /// the common-case where the value is already written returns
  /// immediately on tick 1 or 2) and double up to 800 ms — without the
  /// backoff every provider on startup hammered `prefs.reload()` at
  /// 4 Hz for 6 s, which showed up as a CPU spike at launch.
  static Future<String> _waitForPref(String key, Duration budget) async {
    final prefs    = await SharedPreferences.getInstance();
    final deadline = DateTime.now().add(budget);
    int  delayMs   = 50;
    while (true) {
      // SharedPreferences caches the snapshot taken at getInstance() time.
      // Reload picks up writes that LibratsDiscovery (same isolate,
      // different call site) made after our cached snapshot was captured.
      await prefs.reload();
      final v = prefs.getString(key) ?? '';
      if (v.isNotEmpty) return v;
      if (DateTime.now().isAfter(deadline)) return '';
      await Future.delayed(Duration(milliseconds: delayMs));
      if (delayMs < 800) delayMs = (delayMs * 2).clamp(50, 800);
    }
  }

  static Future<String> getRatsPeerId({
    Duration waitFor = const Duration(seconds: 6),
  }) =>
      _waitForPref('auto_rats_peer_id', waitFor);

  /// Called by [LibratsDiscovery] after a `routes.get` cycle. Stores the
  /// librats peer id of the selected full node, or clears the cached
  /// value when [ratsPeerId] is empty so a post-wipe / no-routes cycle
  /// doesn't leave a stale peer id behind for getRatsPeerId to return.
  static Future<void> updateAutoNode({required String ratsPeerId}) async {
    final prefs = await SharedPreferences.getInstance();
    if (ratsPeerId.isEmpty) {
      await prefs.remove('auto_rats_peer_id');
      return;
    }
    await prefs.setString('auto_rats_peer_id', ratsPeerId);
  }

  static Future<bool> hasRatsNode() async {
    final pid = await getRatsPeerId(waitFor: Duration.zero);
    return pid.isNotEmpty;
  }
}
