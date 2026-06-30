# bopwire — peer-to-peer music with on-chain royalties

I've been quietly building a peer-to-peer music platform for the last
few months. First public binaries are up, multi-container audio support
landed today, and the system finally feels coherent enough to write
about. Here's what it is and where it stands.

## The shape of it

Songs live in two places at once. The **chain** holds a tiny block per
song: a Chromaprint fingerprint, content hash, ID3 metadata, royalty
splits, and a few timestamps. About 3 KB per block — small enough that
a Spotify-scale 100 M-track catalog would weigh in around 310 GB on
chain, same order of magnitude as a portable backup drive.

The **audio bytes** live with players. When you scan a folder, the
player computes the fingerprint locally and either matches you into an
existing swarm or registers a new block. When you press play on
someone else's track, the chain tells you which peers are currently
serving it and your player pulls the bytes peer-to-peer over librats.

That bisection is the whole trick. The chain is the catalog and the
royalty ledger; the swarm is the CDN.

## Containers shipped today

mp3, ogg, flac, m4a, aac, opus, wav, aiff, wma, ape, mka — anything
libmpv decodes, the player scans and serves. Format detection rides on
extension, stamps into a 1-byte enum on the song block, and
round-trips back as the right file extension on download so other
players hand the right bytes to their decoder.

## Discovery, online-only

There's no central song registry. The Discover tab is generated live
from "who's connected right now and what fingerprints did they vouch
for." Close the phone app — peers see your songs vanish from their
Discover within seconds. Open it again — they reappear.

The state pipeline runs end-to-end on rats connection callbacks plus a
small delta protocol: an unchanged library re-announces itself in one
96-byte round trip — a SHA-256 digest, no list bytes. New file? Single
`swarm.add` event. Delete a file? Single `swarm.remove`. The
"flood every fingerprint on every boot" pattern that the prototype
shipped with is gone.

The mini-nodes — our VPSes — gossip routes and online-state between
themselves over a librats mesh. Stand up another bootstrap VPS, hand it
a `--peer-vps` flag pointing at any existing one, and the mesh figures
out the rest. No single point of failure on the catalog surface.

## Royalty model

Pre-10 000 plays per song (the discovery tier):

- Listener earns 1 token as discoverer
- Serving node earns 1 token
- Artist's 1 token routes to a per-artist escrow, releasable by a
  moderator. Quality gate for the first decade of plays.

Post-10 000:

- Artist and serving node each get 1 spendable token
- No discoverer credit
- Listener burns a dynamic amount that scales cubically with total
  supply between a 1 B floor and a 2 B hard cap — zero burn while the
  network is bootstrapping, exponential pressure as we approach the
  ceiling, mint refused entirely past it.

A play counts when timestamped heartbeats prove the listener consumed
at least **50 % of the song's unique timestamp range**. Replay-the-chorus-
on-loop earns nothing; only union coverage of distinct seconds counts.
Seeking around still works as expected — listening to seconds 0–60 and
then 90–150 is 120 seconds of credit, not the wall-clock duration.

## NAT and transport

librats over TCP today, with a custom keepalive (~30 s dead-socket
detection) and a duplicate-peer-eviction patch for the dial→disconnect
storm we hit when the watchdog re-dialed faster than half-open cleanup.

Cellular peers can't ICE-punch through symmetric NAT on the
US carrier we're testing on, so cellular traffic falls back to VPS
relay — TURN-shaped, with a binary forward path for audio chunks so
relayed downloads don't pay a base64 tax. Direct rats connections still
work on home wifi and most non-cellular networks.

Planned but not in this release: QUIC + libdatachannel ICE signaling
for proper hole punching on phones.

## What this isn't

Honest list:

- One moderator. The escrow-release model needs a real council before
  it scales beyond hobby use.
- No algorithmic discovery. Discover is a live chain catalog filtered
  by who's online — no recommendations, no playlists from someone
  else's taste graph.
- Mobile peer connections relay through VPS on symmetric cellular NAT.
- Search is plain-substring against title / artist / genre / album.

## Where to try it

Binaries on the latest GitHub release:

- `bopwire-player-windows-x64.zip` — Windows desktop. Unzip
  anywhere, run `bopwire_player.exe`.
- `bopwire-player.apk` — Android sideload. Needs READ_MEDIA_AUDIO
  (Android 13+) or "All files access" for folders outside `/Music`.

Workflow:

1. Create a wallet (one password, auto-loaded thereafter via the OS
   keyring).
2. Open **My Library**, tap the folder-plus icon, add a music folder.
3. Tap refresh — files get fingerprinted on the device and join the
   chain catalog. Songs that match existing chain entries swarm-join
   silently; new ones queue for the next block.
4. Switch to **Discover**. Pick an artist or genre, drill in, pick an
   album. The track list opens in the bottom pane (drag the handle to
   resize).
5. Tap a track to stream, or long-press / right-click an album to
   download the whole thing into your library.

If you find a song you want to keep, hit download — it'll pull from
the swarm and add to your local library. From there, you serve it too.
That's the entire growth model.

---

Built solo, in C++ for the home node + VPS mini-node and Flutter for
the player. Comments, issues, and "this crashed when I…" reports
welcome on the repo.
