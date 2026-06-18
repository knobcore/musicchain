#!/usr/bin/env bash
#
# Install musicchain full-node build dependencies on an Arch-flavoured
# distro (Arch Linux, Manjaro, EndeavourOS, …).
#
# Same dep set as install-deps-debian.sh; only the package names differ.
# librats + libwally stay vendored — pacman doesn't need to know about
# either.
#
# Usage (run as root or with sudo):
#   sudo bash scripts/install-deps-arch.sh
#
# Optional env vars:
#   PACMAN_OPTS             extra args (e.g. --noconfirm)

set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "[install] re-running with sudo"
    exec sudo bash "$0" "$@"
fi

PACMAN_OPTS="${PACMAN_OPTS:---noconfirm --needed}"

PKGS=(
    # Build toolchain
    base-devel cmake git pkgconf
    # Cryptography + TLS (openssl)
    openssl
    # FFmpeg suite — Arch ships a single ffmpeg package that contains
    # libav{codec,format,util,filter,device} + libsw{resample,scale}.
    ffmpeg
    # Audio fingerprinting
    chromaprint
    # Ogg / Vorbis / Opus containers
    libogg libvorbis opus opusfile
    # Storage
    leveldb
    snappy
    # Network + JSON
    libuv
    nlohmann-json
    curl
    miniupnpc
    # TUI
    ncurses
    # cpp-httplib is vendored at deps/cpp-httplib — no system package
    # needed.
)

echo "[install] pacman -S ${PKGS[*]}"
pacman -S $PACMAN_OPTS "${PKGS[@]}"

echo "[install] done. Now build with:"
echo "    bash scripts/build-node-linux.sh"
