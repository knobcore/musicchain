# bopwire

A peer-to-peer music blockchain with a content-addressed audio swarm.

## Layout

| Tree | Description |
| --- | --- |
| `bopwire/` | C++ full + mini-node, leveldb-backed chain, swarm registry, fingerprint matching (chromaprint), librats peer transport. Vendored librats sits under `bopwire/deps/librats/`. |
| `bopwire_player/` | Cross-platform Flutter player. Android, Windows, Linux desktop. Streams audio from the swarm via a loopback HTTP proxy + media_kit. Foreground-service-pinned on Android so the librats client survives screen-off. |
| `scripts/` | One build script per shipping target — see below. |

## How the pieces fit

1. **Full node** (`bopwire/tools/node_main.cpp`) holds the chain in leveldb, mints blocks, exposes RPC over librats (`bopwire.request` topic). 
2. **Mini node** (`bopwire/tools/mini_node.cpp`) — a tiny rendezvous on the VPS that relays peer addresses, forwards RPC + binary streams for peers behind symmetric NATs, and tracks player/full-node liveness.
3. **Player** scans local folders, fingerprints with chromaprint, submits fingerprint + content hash + ID3 tags to the full node. The full node runs an exact + fuzzy match (bucket-indexed chromaprint similarity) so the same song uploaded twice at different bitrates collapses to one chain entry with two SwarmMember variants. The download dialog presents one row per quality (rounded kbps + container) and picks the rip with the most peers within each quality.
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
| Linux desktop player | `bopwire_player/scripts/build-linux-player.sh` | `bopwire_player/build/linux/x64/release/bundle/` |
| Linux AppImage | `bopwire_player/scripts/build-linux-appimage.sh` | `bopwire_player/build/*.AppImage` |

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

`build-player-android.ps1` expects the prebuilt OpenSSL and chromaprint `.so` files to already be staged under `bopwire_player/android/app/src/main/jniLibs/arm64-v8a/`. The OpenSSL build helper is at `bopwire/build_openssl_android.sh` (Linux/WSL) or `bopwire/build_openssl_android_win.sh` (Windows MSYS2). Chromaprint cross-compile artifacts live at `chromaprint-android-arm64/` in the repo root — `libchromaprint.so` from there is copied into `jniLibs/arm64-v8a/` (the imported prebuilt the NDK links against).

### Linux desktop / AppImage prerequisites

Built with the Flutter **Linux** SDK + GTK3. On Windows this runs under **WSL**
(Ubuntu 22.04). Install the desktop toolchain and the same native deps the Linux
full node uses:

```
sudo apt-get install -y ninja-build libgtk-3-dev clang cmake pkg-config \
  libssl-dev libchromaprint-dev libavcodec-dev libavformat-dev libavutil-dev \
  libswresample-dev libogg-dev libvorbis-dev libopus-dev libopusfile-dev \
  libleveldb-dev libminiupnpc-dev nlohmann-json3-dev libncurses-dev
```

The player `dlopen`s the native core at runtime, so build it first, then package:

1. **Core libs** — `cmake -S bopwire -B bopwire/build-linux-sys -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build bopwire/build-linux-sys --target bopwire rats` → `libbopwire.so` + `libmc_rats.so`.
2. **Flutter bundle** — `bopwire_player/scripts/build-linux-player.sh` (`flutter build linux --release`).
3. **Package** — `bopwire_player/scripts/build-linux-appimage.sh` copies the core libs + `libchromaprint.so` + their non-system deps into the bundle and runs `appimagetool` → one relocatable `*.AppImage`. Point it at the core build with `MC_LIB_DIR`.

On Windows, `bopwire/scripts/_visible-linux-player-build.sh` then `_visible-linux-appimage.sh` run steps 2–3 from WSL in a visible terminal (they strip the Windows `PATH`, add the WSL Flutter SDK, and set `MC_LIB_DIR`/`APPIMAGETOOL`).

## Web player

`bopwire/webplayer/` is the browser client for **bopwire.com** — the Discover feed + streaming, listen-only (a web play still rewards the artist/seeder/mini, just not the listener). Two pieces deploy independently; full steps in [`bopwire/webplayer/deploy/README.md`](bopwire/webplayer/deploy/README.md):

- **Front-end** (`webplayer/frontend/`) — vanilla JS/HTML + WASM audio decoders, no build step. Publish to GitHub Pages under `/player/` with `webplayer/deploy/publish-frontend.sh`.
- **Gateway** (`webplayer/gateway/`) — a headless librats peer bridging the browser (HTTPS via Caddy auto-TLS at `api.bopwire.com`) to the swarm. Build the `bopwire-web-gateway` CMake target with `webplayer/deploy/build-gateway.sh`; run it behind the `bopwire-web-gateway.service` unit + `Caddyfile` in that dir.

## Releases

Pre-built player binaries live on the [GitHub Releases](https://github.com/knobcore/bopwire/releases/latest) page — the web player's download menu links straight at `releases/latest`:

- **Windows player** — `bopwire-windows-x64-setup.exe` (NSIS installer: Start-menu + Desktop shortcuts, uninstaller). Portable `bopwire-windows-x64.zip` also attached.
- **Android player** — `bopwire-android.apk` (arm64-v8a; sideload, enable unknown sources).
- **Linux player** — `bopwire-linux-x86_64.AppImage` (`chmod +x`, run; built on Ubuntu 22.04, needs glibc ≥ 2.35).

All artifacts are produced from the scripts above — there's no separate CI build.
