#pragma once
//
// LibraryStore — the wallet-keyed music library + (future) playlist store.
// This is "DB2": a SECOND database, separate from the chain and OUTSIDE
// consensus. Each wallet owns a set of song content-hashes; the wallet is
// the sole writer of its own record, so replication is plain eventually-
// consistent gossip (signed deltas + anti-entropy) — no block ordering, no
// double-spend, no quorum.
//
// Storage model (per node):
//   * a wallet's library is a set of song ids, held as a Roaring bitmap
//     (compact for sparse sets, mutable in place, O(1) membership, fast
//     set-algebra for "songs in common");
//   * "song id" is a purely LOCAL, first-seen intern handle for a 32-byte
//     content_hash — it never leaves the node. The wire + every public method
//     speak content HASHES, so two nodes need no agreement on id numbering.
//   * the reverse index (which wallets hold song X) is the transpose: another
//     Roaring per song over wallet-ordinals — discovery's "holders(hash)".
//
// Persistence is leveldb under the L-prefixes (separate keyspace from the
// chain's). The forward libraries are authoritative on disk; the reverse
// index is derived and rebuilt from them on attach().
//
// Phase 1 (this file) is the store + local mutate/query. The signed-delta
// gossip + anti-entropy that clones edits onto every other node layers on top
// via apply_delta() (which is already idempotent + version-gated for exactly
// that purpose) — it does not care whether a delta arrived locally or over
// the wire.

#include "../core/block.h"      // Hash256, Address

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mc { class Database; }

namespace mc::store {

class LibraryStore {
public:
    LibraryStore();
    ~LibraryStore();
    LibraryStore(const LibraryStore&) = delete;
    LibraryStore& operator=(const LibraryStore&) = delete;

    /// Wire up persistence and slurp the persisted libraries + intern tables,
    /// then rebuild the reverse index in memory. Safe to call once at startup.
    void attach(Database& db);

    // ---- mutate ------------------------------------------------------------

    /// Replace a wallet's whole library with `hashes` at `version` (a SNAPSHOT
    /// — the authoritative full set, so removals propagate). Version-gated +
    /// idempotent: a NO-OP returning false unless `version` is strictly newer
    /// than the wallet's current version. Returns true if applied.
    bool set_library(const Address& wallet,
                     const std::vector<Hash256>& hashes, uint64_t version);

    /// Apply an edit: add `add` and remove `remove` from the wallet's library.
    /// `version` is the new monotonic version carried by a (signed) delta;
    /// the call is a NO-OP and returns false if `version` is not strictly
    /// greater than the wallet's current version (idempotent under gossip
    /// re-delivery / out-of-order arrival). Returns true if applied.
    bool apply_delta(const Address& wallet,
                     const std::vector<Hash256>& add,
                     const std::vector<Hash256>& remove,
                     uint64_t version);

    // ---- query (forward: wallet -> songs) ---------------------------------

    std::vector<Hash256> library(const Address& wallet) const;
    size_t               library_size(const Address& wallet) const;
    uint64_t             library_version(const Address& wallet) const;

    // ---- query (reverse: song -> wallets) ---------------------------------

    /// Every wallet whose library currently contains `ch`. The discovery
    /// query — "who has this song?".
    std::vector<Address> holders(const Hash256& ch) const;
    size_t               holder_count(const Hash256& ch) const;

    // ---- piece manifests (Swarm Transfer v2) ------------------------------
    //
    // Per-song piece-hash manifest (JSON: the SHA-256 of each 256 KB piece),
    // keyed by content_hash under the "Lm" prefix. Stored on fingerprint.submit
    // and served by stream.open so a downloader can verify each chunk on arrival
    // (safe multi-source). Off-chain, not consensus; last-writer-wins is fine —
    // the bytes are deterministic, so honest submitters produce identical
    // manifests. Returns nullopt when no manifest is stored for `ch`.
    void put_manifest(const Hash256& ch, const std::string& manifest_json);
    std::optional<std::string> get_manifest(const Hash256& ch) const;

    /// Discovery snapshot for songs.list. Given the set of currently-LIVE
    /// wallets (presence-bound + within TTL, resolved by the caller under its
    /// own presence lock), do ONE walk under this store's lock that builds
    /// BOTH outputs at once:
    ///   * `online_hashes`: the hex content_hash of every song held by any
    ///     live wallet (the "is this song online at all?" filter), and
    ///   * `holder_counts`: hex content_hash -> number of LIVE wallets holding
    ///     it (the per-song "swarm size" the UI shows).
    /// Replaces the old O(N_songs x N_holders) per-song double-mutex loop in
    /// songs.list with a single pass. `live_wallets` is keyed by the 40-char
    /// lowercase wallet hex (matching the presence map's keys).
    void online_snapshot(
        const std::unordered_set<std::string>& live_wallets,
        std::unordered_set<std::string>& online_hashes,
        std::unordered_map<std::string, size_t>& holder_counts) const;

    // ---- playlists (ordered lists; many per wallet) -----------------------
    //
    // A playlist is a wallet-owned, ORDERED list of song hashes with a name,
    // keyed by a 16-byte playlist id. Same signed-delta + gossip plumbing as
    // libraries — but order matters, so it is NOT a Roaring set; it is stored
    // as an ordered hash list, leveldb-backed and version-gated for gossip
    // convergence. A deleted playlist is kept as a version-stamped tombstone
    // so a re-delivered older "set" can't resurrect it.
    struct Playlist {
        std::array<uint8_t, 16> id{};
        std::string             name;
        uint64_t                version = 0;
        std::vector<Hash256>    songs;          // ORDERED
        bool                    deleted = false;
    };

    /// Create/replace a playlist (whole ordered list + name). Version-gated +
    /// idempotent; returns true iff applied (strictly-newer version).
    bool set_playlist(const Address& wallet, const std::array<uint8_t, 16>& id,
                      const std::string& name,
                      const std::vector<Hash256>& songs, uint64_t version);
    /// Tombstone a playlist (version-gated). Returns true iff applied.
    bool delete_playlist(const Address& wallet,
                         const std::array<uint8_t, 16>& id, uint64_t version);
    std::optional<Playlist> get_playlist(
        const Address& wallet, const std::array<uint8_t, 16>& id) const;
    /// Every live (non-tombstoned) playlist a wallet owns.
    std::vector<Playlist> list_playlists(const Address& wallet) const;

    // ---- anti-entropy: stored signed payloads ------------------------------
    //
    // The latest signed wire payload of each library / playlist record is kept
    // so a node can RE-SEND it to a peer that missed the live flood (e.g. the
    // peer was offline during the edit). On connect, peers exchange a
    // {key -> version} summary and push the payloads the other is behind on.
    // Keyed Ls<wallet:20> / Ps<wallet:20><id:16>, value = ver(8 LE) | payload.
    void store_library_payload(const Address& wallet, uint64_t version,
                               const std::string& payload);
    void store_playlist_payload(const Address& wallet,
                                const std::array<uint8_t, 16>& id,
                                uint64_t version, const std::string& payload);
    void for_each_library_payload(
        const std::function<void(const Address&, uint64_t,
                                 const std::string&)>& cb) const;
    void for_each_playlist_payload(
        const std::function<void(const Address&, const std::array<uint8_t, 16>&,
                                 uint64_t, const std::string&)>& cb) const;

    // ---- stats -------------------------------------------------------------

    size_t wallet_count() const;   // wallets with a non-empty library
    size_t catalog_size() const;   // distinct songs interned

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace mc::store
