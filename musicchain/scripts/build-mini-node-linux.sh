#!/usr/bin/env bash
#
# Build ONLY the VPS mini-node on Linux. Lighter than build-node-linux.sh
# because it skips chromaprint / ffmpeg / libwally — the mini-node is a
# stateless relay.
#
# Required env vars:
#   VCPKG_ROOT
#
# Optional:
#   CMAKE=cmake
#   BUILD_DIR=build-linux-mini
#   OUTPUT_DIR=$BUILD_DIR/Release
#   CLEAN=0
#   JOBS=$(nproc)

set -euo pipefail

: "${VCPKG_ROOT:?VCPKG_ROOT not set}"
CMAKE="${CMAKE:-cmake}"
BUILD_DIR="${BUILD_DIR:-build-linux-mini}"
OUTPUT_DIR="${OUTPUT_DIR:-$BUILD_DIR/Release}"
CLEAN="${CLEAN:-0}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

if [ "$CLEAN" = "1" ]; then rm -rf "$BUILD_DIR"; fi

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    "$CMAKE" -S . -B "$BUILD_DIR" \
        -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
        -DVCPKG_TARGET_TRIPLET=x64-linux \
        -DCMAKE_BUILD_TYPE=Release \
        -DMC_MINI_NODE_ONLY=ON
fi

"$CMAKE" --build "$BUILD_DIR" --config Release --target musicchain-mini-node -j"$JOBS"

mkdir -p "$OUTPUT_DIR"
cp -f "$BUILD_DIR/musicchain-mini-node" "$OUTPUT_DIR/musicchain-mini-node"
[ -f "config/mini-node.config.json" ] && \
    cp -f "config/mini-node.config.json" "$OUTPUT_DIR/mini-node.config.json"
echo "[done] mini-node at $OUTPUT_DIR/musicchain-mini-node"
