#!/usr/bin/env bash
# ----------------------------------------------------------------------
# build-node-linux.sh
#
# Clean-slate build of the FULL bopwire-node for Linux x86_64. The
# full node needs media deps (chromaprint + ffmpeg + ogg/vorbis/opus),
# leveldb, and ncurses for the TUI. This script installs everything via
# apt, then cmake-builds.
#
# Usage:
#   ./scripts/build-node-linux.sh                 # build
#   ./scripts/build-node-linux.sh --clean         # wipe build-linux/
#   ./scripts/build-node-linux.sh --output DIR    # also copy binary to DIR
# ----------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_DIR="$REPO_ROOT/bopwire"
BUILD_DIR="$SRC_DIR/build-linux"
OUTPUT_DIR=""
CLEAN=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean)  CLEAN=1; shift ;;
        --output) OUTPUT_DIR="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,16p' "$0"
            exit 0
            ;;
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

step() { printf '\n==> %s\n' "$*"; }
fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

# ---- apt deps (full node) -------------------------------------------

step "apt-get install (full-node deps)"
DEBIAN_FRONTEND=noninteractive
PKGS="build-essential cmake pkg-config git \
      libssl-dev libleveldb-dev libchromaprint-dev \
      libavcodec-dev libavformat-dev libavutil-dev libswresample-dev \
      libogg-dev libvorbis-dev libvorbisfile3 libopus-dev libopusfile-dev \
      libncurses-dev libcurl4-openssl-dev libminiupnpc-dev libuv1-dev \
      libsnappy-dev"
if command -v apt-get >/dev/null 2>&1; then
    sudo apt-get update -qq
    sudo apt-get install -y $PKGS
else
    echo "  apt-get not available — assuming deps are present"
fi

# ---- Configure ------------------------------------------------------

if [[ $CLEAN -eq 1 && -d "$BUILD_DIR" ]]; then
    step "wiping $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

step "cmake configure (Release)"
cmake -S "$SRC_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release

# ---- Build ----------------------------------------------------------

step "cmake --build (target bopwire-node)"
cmake --build "$BUILD_DIR" --target bopwire-node -j"$(nproc)"

BIN="$BUILD_DIR/bopwire-node"
[[ -f "$BIN" ]] || fail "bopwire-node not produced"

step "Build artifact"
echo "  $BIN"
file "$BIN" 2>/dev/null || true

if [[ -n "$OUTPUT_DIR" ]]; then
    mkdir -p "$OUTPUT_DIR"
    cp "$BIN" "$OUTPUT_DIR/"
    step "Staged copy -> $OUTPUT_DIR/bopwire-node"
fi
