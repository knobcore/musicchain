#pragma once
//
// Variant-aware swarm map. For each canonical content_hash on the chain
// (the first-registered encoding of a song), tracks every peer that has
// announced they hold the bytes for SOME encoding of that song along
// with the local content_hash of their copy, the bitrate, and the
// container. This lets two players upload the same song at different
// bitrates (e.g. Holiday@128 vs Holiday@320) without producing duplicate
// chain entries — fuzzy chromaprint match in fingerprint.submit pins
// both to the canonical hash, and stream.open returns enough metadata
// for the requester to pick which variant to fetch.
//
// Mirrored to leveldb under the "sw:" prefix so a full-node restart
// doesn't wipe every player out of the swarm.

#include "../core/block.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mc {
class Database;
}

namespace mc::store {

struct SwarmMember {
    std::string peer_id;          // 40-hex librats peer id
    Hash256     content_hash;     // hash of THIS peer's local file bytes
                                  // (may differ from canonical)
    uint32_t    bitrate    = 0;   // bits/sec, 0 = unknown
    AudioFormat audio_format = AudioFormat::OGG;
    uint64_t    last_seen_ms = 0; // wall-clock of last announce/touch
};

class SwarmIndex {
public:
    SwarmIndex() = default;

    /// Members not touched for this long are considered ghosts and are
    /// filtered out of [members]+[song_count] / pruned periodically. A
    /// player re-announces on every VPS reconnect + every 5 minutes via
    /// the heartbeat timer, so a 20-minute window lets a brief network
    /// hiccup pass without dropping anyone but still expires peers who
    /// closed the app.
    static constexpr uint64_t kStaleAfterMs = 20ULL * 60ULL * 1000ULL;

    /// Wire up persistence and slurp any entries that survived the last
    /// run. Safe to call once at startup; subsequent calls are no-ops.
    void attach(Database& db);

    /// Announce a peer for `canonical_hash`. Replaces any prior entry
    /// for that peer and refreshes last_seen_ms.
    void announce(const Hash256& canonical_hash, const SwarmMember& member);

    /// Forget a single peer for a single song.
    void drop(const Hash256& canonical_hash, const std::string& peer_id);

    /// Refresh last_seen_ms for every entry this peer holds — called
    /// on every inbound RPC (proof-of-life). This is an IN-MEMORY-ONLY
    /// update: it no longer does a synchronous per-announced-hash leveldb
    /// put, because it runs on the hot inbound-RPC path and that write
    /// amplification (one put per hash the peer holds, per request) was
    /// starving the relay flush. The peer is flagged dirty and the
    /// freshened last_seen_ms is persisted lazily from [flush_dirty],
    /// called by the periodic prune thread. last_seen_ms only matters for
    /// offline TTL pruning, so a delayed flush is harmless: an online
    /// peer is never pruned regardless of its on-disk timestamp.
    void touch_peer(const std::string& peer_id);

    /// Persist the last_seen_ms of every peer that [touch_peer] freshened
    /// since the last flush. Cheap no-op when nothing is dirty. Called
    /// from the periodic prune thread so the hot RPC path stays write-free.
    /// Returns the number of entries written.
    size_t flush_dirty();

    /// Walk every song and drop members whose last_seen_ms is older
    /// than [kStaleAfterMs]. Returns the number of entries pruned.
    /// Invoked from a periodic thread in RatsApi.
    size_t prune_stale();

    /// All currently-announced FRESH members for `canonical_hash`
    /// (stale entries are hidden). Empty if nobody is announced.
    std::vector<SwarmMember> members(const Hash256& canonical_hash) const;

    /// Total number of songs with at least one fresh announced member.
    size_t song_count() const;

    // ---- Connection-state availability (efficient swarm protocol) ----
    //
    // The swarm protocol moved from a 20-minute TTL renewed by per-track
    // re-submits to a "rats connection IS authoritative for availability"
    // model. Each peer announces its hash set ONCE per session via
    // swarm.hello, and the full node marks the peer as online while the
    // librats transport says they're connected. Disconnect → mark
    // offline (the on-disk entries stay so a reconnect can match a
    // digest instantly). The 20-min TTL stays as a safety net for
    // half-open / never-FIN'd connections.

    /// Mark a peer as online (e.g. on librats connection-callback). All
    /// of their persisted entries become visible to members() again
    /// without the player needing to re-announce. Idempotent.
    void mark_peer_online(const std::string& peer_id);

    /// Mark a peer offline. Their entries vanish from members() but
    /// stay in the in-memory map and leveldb so the next reconnect
    /// can swarm.hello_digest and skip the resync.
    void mark_peer_offline(const std::string& peer_id);

    /// True iff this peer is currently in online_peers_ (i.e. the
    /// librats transport / mini-node presence pipeline says it's
    /// connected). Used by the DB2 discovery path (RatsApi) to filter
    /// a wallet's library down to copies a live player is serving.
    bool is_online(const std::string& peer_id) const;

    /// Hard-evict a peer: remove from online_peers_, drop every
    /// (canonical_hash → SwarmMember) entry where member.peer_id ==
    /// peer_id, and persist the removals to leveldb. Use when the
    /// transport says the peer is gone for good (e.g. librats
    /// disconnect-callback) — unlike mark_peer_offline, this purges
    /// the on-disk record so stream.open won't keep returning a dead
    /// peer up to the 20-minute prune cutoff. Idempotent.
    void evict_peer(const std::string& peer_id);

    /// Drop every entry this peer owns — used by swarm.hello when the
    /// player declares a brand-new (digest-miss) hash list, replacing
    /// what we had. Returns the number of entries removed.
    size_t drop_peer(const std::string& peer_id);

    /// Atomic peer-wide replace. Drops everything the peer currently
    /// claims, then announces `members` as the new set. Returns the
    /// digest of the resulting canonical-hash set, hex-encoded.
    std::string replace_peer(
        const std::string& peer_id,
        const std::vector<std::pair<Hash256, SwarmMember>>& members);

    /// SHA-256 over the sorted-and-concatenated canonical content_hash
    /// bytes for the entries currently owned by [peer_id]. Empty hex
    /// string when the peer has no entries. Used by swarm.hello_digest
    /// for the cheap "are you still claiming the same library?" check.
    std::string peer_digest(const std::string& peer_id) const;

    /// How many canonical hashes does this peer currently claim?
    size_t peer_size(const std::string& peer_id) const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::vector<SwarmMember>> map_;
    /// Reverse index for O(1) peer-wide operations (touch/drop/digest).
    /// Stores the set of canonical_hash hex strings each peer claims.
    std::unordered_map<std::string, std::unordered_set<std::string>> peer_to_hashes_;
    /// Peers whose in-memory last_seen_ms was bumped by [touch_peer] but
    /// not yet persisted. Drained by [flush_dirty] on the prune thread.
    std::unordered_set<std::string> dirty_peers_;
    /// Set of peer_ids currently online via librats. Entries owned by
    /// peers NOT in this set are filtered out of members() / song_count.
    std::unordered_set<std::string> online_peers_;
    Database* db_ = nullptr;

    static uint64_t now_ms_();
    bool is_fresh_(const SwarmMember& m) const;
    bool peer_available_(const std::string& peer_id, const SwarmMember& m) const;
    std::string peer_digest_unlocked_(const std::string& peer_id) const;
    void index_member_(const std::string& canonical_hex,
                       const std::string& peer_id);
    void unindex_member_(const std::string& canonical_hex,
                         const std::string& peer_id);
};

} // namespace mc::store
