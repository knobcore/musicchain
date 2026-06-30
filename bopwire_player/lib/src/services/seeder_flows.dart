// Process-global per-seeder concurrency accounting — Swarm Transfer v2 (#3).
//
// Both transfer paths register their active flows here so a single seeder never
// serves more than [kPerSeederWindow] flows AT ONCE across the WHOLE app —
// keeping each seeder to ~one 4 Mbit/s flow no matter how many transfers run
// (one stream + a background album download, etc.). The per-flow byte pacing
// lives on the seeder (player_server kFlowCapBytesPerSec); this just bounds how
// many such flows a seeder serves concurrently.
//
// STREAMING HAS PRIORITY. A live AudioStream always registers its flow (it never
// waits — first-byte latency must stay low), and the DOWNLOAD path treats a
// seeder at/over the window as unavailable (downloadHasWindow == false), so a
// background swarm.fetch download yields that seeder and fans onto a different
// one. A brief 2-flow overlap can occur if a stream opens on a seeder a download
// is mid-range on; it self-corrects within one range as the download stops
// re-selecting that seeder.
class SeederFlows {
  SeederFlows._();
  static final SeederFlows instance = SeederFlows._();

  /// Max concurrent flows per seeder (one ~4 Mbit/s flow).
  static const int kPerSeederWindow = 1;

  final Map<String, int> _flows = {};

  /// Whether the DOWNLOAD path may open another flow to [peerId]. Streaming
  /// does NOT consult this — it always proceeds (priority) and just counts.
  bool downloadHasWindow(String peerId) =>
      (_flows[peerId] ?? 0) < kPerSeederWindow;

  void acquire(String peerId) {
    if (peerId.isEmpty) return;
    _flows[peerId] = (_flows[peerId] ?? 0) + 1;
  }

  void release(String peerId) {
    if (peerId.isEmpty) return;
    final v = (_flows[peerId] ?? 1) - 1;
    if (v <= 0) {
      _flows.remove(peerId);
    } else {
      _flows[peerId] = v;
    }
  }
}
