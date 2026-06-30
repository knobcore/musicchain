#!/usr/bin/env bash
#
# Install bopwire full-node build dependencies on a Debian-flavoured
# distro (Debian, Ubuntu, Mint, Pop!_OS, Raspberry Pi OS, …).
#
# Why not vcpkg on Linux: the deps below are mature, packaged in every
# distro, and faster to apt-install than to compile from source. vcpkg
# is kept for Windows where there's no system package manager.
#
# What stays vendored:
#   - deps/librats          (our patched fork)
#   - deps/libwally-core    (vendored static link, see CMakeLists)
#
# Usage (run as root or with sudo):
#   sudo bash scripts/install-deps-debian.sh
#
# Optional env vars:
#   APT_OPTS                extra args passed to apt-get (e.g. -y --no-install-recommends)

set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "[install] re-running with sudo"
    exec sudo bash "$0" "$@"
fi

APT_OPTS="${APT_OPTS:--y}"

echo "[install] apt-get update"
apt-get update -qq

PKGS=(
    # Build toolchain
    build-essential cmake git pkg-config
    # Cryptography + TLS
    libssl-dev
    # FFmpeg suite (covers FLAC / MP3 / Opus / AAC / WAV decode)
    libavcodec-dev libavformat-dev libavutil-dev
    libswresample-dev libswscale-dev
    # Audio fingerprinting
    libchromaprint-dev
    # Ogg / Vorbis / Opus containers
    libogg-dev libvorbis-dev libopus-dev libopusfile-dev
    # Storage
    libleveldb-dev
    # leveldb's link-time dependency on snappy. The runtime libsnappy.so.1
    # comes for free when libleveldb-dev is installed, but the bare
    # libsnappy.so symlink the linker resolves -lsnappy through only
    # arrives via libsnappy-dev.
    libsnappy-dev
    # Network + JSON (cpp-httplib is vendored at deps/cpp-httplib —
    # no apt package needed)
    libuv1-dev
    nlohmann-json3-dev
    libcurl4-openssl-dev
    libminiupnpc-dev
    # TUI
    libncurses-dev
)

echo "[install] installing: ${PKGS[*]}"
apt-get install $APT_OPTS "${PKGS[@]}"

echo "[install] done. Now build with:"
echo "    bash scripts/build-node-linux.sh"
