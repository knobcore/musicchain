# Bopwire Node

A blockchain where each block is a unique song, verified by audio fingerprinting.

## Quick Start

### 1. Install dependencies (Linux/Debian)

```bash
sudo apt update && sudo apt install -y \
  build-essential cmake git pkg-config \
  libssl-dev libsodium-dev libchromaprint-dev \
  ffmpeg libavcodec-dev libavformat-dev libavutil-dev libswresample-dev \
  libogg-dev libvorbis-dev libopus-dev libopusfile-dev \
  libleveldb-dev libuv1-dev libmicrohttpd-dev nlohmann-json3-dev
```

### 2. Install dependencies (Windows — MSYS2 MinGW64)

```bash
pacman -S mingw-w64-x86_64-{toolchain,cmake,pkg-config,openssl,libsodium,chromaprint,ffmpeg,libogg,libvorbis,opus,opusfile,leveldb,libuv,libmicrohttpd,nlohmann-json}
```

### 3. Build

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

Windows (MinGW):
```bash
cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release ..
mingw32-make -j%NUMBER_OF_PROCESSORS%
```

### 4. Run a node

```bash
./bopwire-node start --data-dir ./data --api-port 9334 --p2p-port 9333
```

The node listens for P2P connections on port 9333 and exposes an HTTP API on port 9334.

### 5. Verify a node is running

```bash
curl http://localhost:9334/api/v1/status
```

---

## Architecture

### Block Structure

Every block encodes one unique song plus all transactions since the previous block:

```
[ Block Header ]  (version, prev_hash, merkle_root, music_data_root, timestamp, confirmations)
[ Song Section ]  (content_hash, fingerprint, duration, title, artist, genre, royalties, chunks)
[ 8 × 0xFF     ]  separator
[ Transactions ]  (transfer txs + mint txs from play sessions)
```

Block hash = SHA256(header bytes).

### Consensus (Proof of Music)

- A new block is only produced when a **unique** song is uploaded.
- Uniqueness checked with Chromaprint fingerprinting (similarity threshold 0.90).
- Block integrity is verified by comparing a full-block SHA256 checksum with connected peers.
- A **single node can operate independently** — no minimum peer count required.
- More connected nodes increase security: each node cross-checks received blocks against all peers before accepting them.

### Token Economics

| Condition                  | Artist   | Node     | Discoverer | Player      |
|----------------------------|----------|----------|------------|-------------|
| First 50,000 plays         | 1.000000 | 1.000000 | 1.000000   | +1.000000   |
| After 50,000 plays         | 0.010000 | 0.010000 | 0.000000   | −1.000000 🔥 |

Royalty splits subdivide the artist share only.

After 50,000 plays, each additional play **burns 1.000000 token from the player's balance**.
The player must hold at least 1 token to start a session on a song past this threshold.
Artist and node continue to earn 0.01 tokens per play indefinitely.

### Play Session Protocol

1. Player → `POST /sessions/start` with `content_hash` and `player_address`
2. Player downloads and decodes audio
3. Player sends heartbeat every 30 s with position and audio checksum
4. Player → `POST /sessions/{id}/complete` when done (minimum 30 s played)
5. Node issues mint transaction; tokens distributed immediately in next block

---

## HTTP API

Base URL: `http://localhost:9334/api/v1`

| Method | Path                              | Description                    |
|--------|-----------------------------------|--------------------------------|
| GET    | /status                           | Node status                    |
| GET    | /peers                            | Connected peers                |
| GET    | /blocks/{hash}                    | Full block                     |
| GET    | /blocks/height/{n}                | Block at height n              |
| GET    | /songs                            | Paginated song list            |
| GET    | /songs/{content_hash}             | Song metadata                  |
| GET    | /songs/{content_hash}/stream      | Raw Ogg audio data             |
| POST   | /upload                           | Upload a new song              |
| GET    | /upload/{upload_id}               | Upload status                  |
| POST   | /sessions/start                   | Start play session             |
| POST   | /sessions/{id}/heartbeat          | Send heartbeat                 |
| POST   | /sessions/{id}/complete           | Complete session               |
| GET    | /balances/{address}               | Token balance                  |
| POST   | /transactions/transfer            | Submit token transfer          |

---

## Node CLI

```
bopwire-node start [--config path] [--data-dir path] [--p2p-port n] [--api-port n]
bopwire-node status
bopwire-node peers
bopwire-node verify-chain [--data-dir path]
bopwire-node rebuild-index [--data-dir path]
```

---

## Database Layout (LevelDB)

| Key prefix | Contents                          |
|------------|-----------------------------------|
| `b:`       | Full serialized block             |
| `h:`       | Block hash → height               |
| `n:`       | Height → block hash               |
| `t:tip`    | Current tip (hash + height)       |
| `f:`       | Fingerprint entry per song        |
| `i:`       | Inverted fingerprint buckets      |
| `s:`       | Song state (play count, discoverer) |
| `a:`       | Address balance                   |
| `u:`       | Used session IDs                  |
| `p:`       | Pending mempool transactions      |
| `v:`       | Validator registry                |
| `c:`       | Configuration / metadata          |

---

## Project Structure

```
bopwire/
  include/bopwire.h        Public C API (for Flutter FFI)
  src/
    core/                     Block, transaction, merkle, chain
    crypto/                   SHA256, ECDSA keys & signatures
    audio/                    Ogg validation, decoding, Chromaprint
    consensus/                Validator registry, block candidates
    tokens/                   Token ledger, mint logic
    network/                  libuv TCP P2P, message protocol
    storage/                  LevelDB wrapper
    api/                      libmicrohttpd HTTP server + routes
    capi/                     C API implementation for FFI
  tools/node_main.cpp         Node executable entry point
  CMakeLists.txt
```
