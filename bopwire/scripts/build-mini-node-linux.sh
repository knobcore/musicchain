#!/usr/bin/env bash
#
# Build ONLY the VPS mini-node on Linux using system packages (NO vcpkg).
# The mini-node skips chromaprint / ffmpeg / leveldb / curses entirely —
# it's a librats relay.
#
# Run install-deps-debian.sh or install-deps-arch.sh first; this script
# uses what those install (openssl + base toolchain + json header).
#
# Optional env vars:
#   CMAKE=cmake
#   BUILD_DIR=build-linux-mini
#   OUTPUT_DIR=$BUILD_DIR/Release
#   CLEAN=0
#   JOBS=$(nproc)

set -euo pipefail

CMAKE="${CMAKE:-cmake}"
BUILD_DIR="${BUILD_DIR:-build-linux-mini}"
OUTPUT_DIR="${OUTPUT_DIR:-$BUILD_DIR/Release}"
CLEAN="${CLEAN:-0}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

PATH="$(echo "$PATH" | tr ':' '\n' | grep -vE '^/mnt/c/' | paste -sd: -)"
export PATH
unset CMAKE_PREFIX_PATH

if [ "$CLEAN" = "1" ]; then rm -rf "$BUILD_DIR"; fi

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    "$CMAKE" -S . -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_IGNORE_PATH=/mnt/c/msys64 \
        -DMC_MINI_NODE_ONLY=ON
fi

"$CMAKE" --build "$BUILD_DIR" --config Release --target bopwire-mini-node -j"$JOBS"

mkdir -p "$OUTPUT_DIR"
cp -f "$BUILD_DIR/bopwire-mini-node" "$OUTPUT_DIR/bopwire-mini-node"
[ -f "config/mini-node.config.json" ] && \
    cp -f "config/mini-node.config.json" "$OUTPUT_DIR/mini-node.config.json"
echo "[done] mini-node at $OUTPUT_DIR/bopwire-mini-node"
