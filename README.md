# bopwire

A peer-to-peer music blockchain with a content-addressed audio swarm.

## Layout

| Tree | Description |
| --- | --- |
| `bopwire/` | C++ home + mini-node, leveldb-backed chain, swarm registry, fingerprint matching (chromaprint), librats peer transport. Vendored librats sits under `bopwire/deps/librats/`. |
| `bopwire_player/` | Cross-platform Flutter player. Android, Windows, Linux desktop. Streams audio from the swarm via a loopback HTTP proxy + media_kit. Foreground-service-pinned on Android so the librats client survives screen-off. |
| `scripts/` | One build script per shipping target — see below. |

## How the pieces fit

1. **Home node** (`bopwire/tools/node_main.cpp`) holds the chain in leveldb, mints blocks via a 5-min heartbeat producer, exposes RPC over librats (`bopwire.request` topic). Persists swarm membership so a restart doesn't wipe it.
2. **Mini node** (`bopwire/tools/mini_node.cpp`) — a tiny rendezvous on the VPS that relays peer addresses, forwards RPC + binary streams for peers behind symmetric NATs, and tracks player/full-node liveness.
3. **Player** scans local folders, fingerprints with chromaprint, submits fingerprint + content hash + ID3 tags to the home node. The home node runs an exact + fuzzy match (bucket-indexed chromaprint similarity) so the same song uploaded twice at different bitrates collapses to one chain entry with two SwarmMember variants. The download dialog presents one row per quality (rounded kbps + container) and picks the rip with the most peers within each quality.
4. **Swarm** is a leveldb-persisted map `canonical_content_hash → [SwarmMember{peer_id, local_content_hash, bitrate, audio_format}, …]`. `stream.open` returns the variant list; the streaming path picks lowest bitrate; the download path either takes user's pick or falls back to highest-peer-count.

## Build scripts

Every shipping binary has its own script. Each script wipes its own build dir on `--clean`, installs the deps it needs (vcpkg on Windows, apt on Linux), and prints where the artifact lands.

| Target | Script | Output |
| --- | --- | --- |
| Windows full node | `scripts\build-node-windows.ps1` | `bopwire\build-win64\Release\bopwire-node.exe` |
| Linux full node | `scripts/build-node-linux.sh` | `bopwire/build-linux/bopwire-node` |
| Linux mini node | `scripts/build-mini-node-linux.sh` | `bopwire/build-linux-mini/bopwire-mini-node` |
| Windows player | `scripts\build-player-windows.ps1` | `bopwire_player\build\windows\x64\runner\Release\` |
| Android APK | `scripts\build-player-android.ps1` | `bopwire_player\build\app\outputs\flutter-apk\app-release.apk` |

Common flags:

- `--clean` / `-Clean` — wipe the script's build dir before configuring.
- `--output DIR` / `-OutputDir DIR` — also stage a copy of the artifact under `DIR`.

### Windows prerequisites

The Windows scripts probe for the toolchain and print a clear error if anything is missing. You will need:

- Visual Studio 2022 with the **Desktop development with C++** workload (or the standalone Build Tools)
- CMake on `PATH` (`winget install Kitware.CMake`)
- Flutter SDK (for the player scripts)
- JDK 17 (for `build-player-android.ps1`) — `winget install EclipseAdoptium.Temurin.17.JDK`
- Android SDK + the NDK that `android/app/build.gradle.kts` pins (currently `27.0.12077973`)

vcpkg is auto-cloned into `bopwire\vcpkg\` if it isn't already on `VCPKG_ROOT` or one of the common locations.

### Linux prerequisites

The shell scripts run `apt-get install` for everything they need, so you just need `sudo`. Tested on Ubuntu 22.04 / Debian 12. The mini-node script installs only OpenSSL; the full-node script also installs chromaprint, ffmpeg, ogg/vorbis/opus, leveldb, and ncurses.

### Android prerequisites

`build-player-android.ps1` expects the prebuilt OpenSSL and chromaprint `.so` files to already be staged under `bopwire_player/android/app/src/main/jniLibs/arm64-v8a/`. The OpenSSL build helper is at `bopwire/build_openssl_android.sh` (Linux/WSL) or `bopwire/build_openssl_android_win.sh` (Windows MSYS2). Chromaprint cross-compile artifacts live at `chromaprint-android-arm64/` in the repo root.

## Releases

Pre-built binaries live on the GitHub Releases page:

- **Windows home node** — `bopwire-node.exe` + DLLs.
- **Linux home node** — `bopwire-node` (Ubuntu 22.04 toolchain).
- **Linux mini node** — `bopwire-mini-node` (statically linked where possible).
- **Windows player** — `bopwire_player.exe` + DLLs.
- **Android player** — `app-release.apk`.

The release artifacts are produced from the same scripts above — there's no separate CI build.
