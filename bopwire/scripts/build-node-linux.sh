#!/usr/bin/env bash
#
# Build bopwire-node + mini-node + libbopwire.so on Linux using
# system packages (NO vcpkg).
#
# Run install-deps-debian.sh or install-deps-arch.sh first to apt/pacman
# the needed -dev packages. After that all this script does is invoke
# cmake — which picks up libssl, ffmpeg, chromaprint, leveldb, etc. via
# pkg-config / find_package. librats + libwally stay vendored.
#
# Optional env vars (defaults shown):
#   CMAKE=cmake
#   BUILD_DIR=build-linux
#   OUTPUT_DIR=$BUILD_DIR/Release
#   CLEAN=0                "1" wipes BUILD_DIR first
#   JOBS=$(nproc)

set -euo pipefail

CMAKE="${CMAKE:-cmake}"
BUILD_DIR="${BUILD_DIR:-build-linux}"
OUTPUT_DIR="${OUTPUT_DIR:-$BUILD_DIR/Release}"
CLEAN="${CLEAN:-0}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

# Strip /mnt/c PATH entries that WSL inherits from Windows — otherwise
# cmake's find_package can pick up Windows mingw toolchain configs
# (e.g. /mnt/c/msys64/mingw64/lib/cmake/CURL) which then drag Win32-only
# sys/cdefs.h into a Linux build. Harmless on a real Linux host where
# /mnt/c doesn't exist; necessary in WSL.
PATH="$(echo "$PATH" | tr ':' '\n' | grep -vE '^/mnt/c/' | paste -sd: -)"
export PATH
unset CMAKE_PREFIX_PATH

if [ "$CLEAN" = "1" ]; then
    echo "[clean] removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    echo "[configure] $CMAKE -S . -B $BUILD_DIR"
    "$CMAKE" -S . -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_IGNORE_PATH=/mnt/c/msys64
fi

echo "[build] $CMAKE --build $BUILD_DIR -j$JOBS"
"$CMAKE" --build "$BUILD_DIR" --config Release -j"$JOBS"

mkdir -p "$OUTPUT_DIR"
for f in bopwire-node bopwire-mini-node libbopwire.so; do
    src="$BUILD_DIR/$f"
    [ -f "$src" ] && cp -f "$src" "$OUTPUT_DIR/$f"
done
# librats shared lib
rats_so="$BUILD_DIR/deps/librats/lib/libmc_rats.so"
[ -f "$rats_so" ] && cp -f "$rats_so" "$OUTPUT_DIR/libmc_rats.so"

# Default config files shipped alongside binaries.
for cfg in full-node.config.json mini-node.config.json; do
    [ -f "config/$cfg" ] && cp -f "config/$cfg" "$OUTPUT_DIR/$cfg"
done

echo "[done] artifacts in $OUTPUT_DIR"
