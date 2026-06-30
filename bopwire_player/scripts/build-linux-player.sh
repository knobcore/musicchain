#!/usr/bin/env bash
#
# Build the bopwire Linux player (Flutter desktop).
#
# Required env vars:
#   FLUTTER                Path to flutter binary
#   MC_LIB_DIR             Folder holding libbopwire.so + libmc_rats.so
#                          to stage into the Flutter bundle.
#
# Optional:
#   BUILD_MODE=release     (release | profile | debug)
#
# Usage:
#   FLUTTER=/opt/flutter/bin/flutter \
#   MC_LIB_DIR=/opt/bopwire/Release \
#   ./scripts/build-linux-player.sh

set -euo pipefail

: "${FLUTTER:?FLUTTER not set}"
: "${MC_LIB_DIR:?MC_LIB_DIR not set — point at a dir holding libbopwire.so + libmc_rats.so}"

BUILD_MODE="${BUILD_MODE:-release}"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

# Strip Windows PATH from the inherited env. Without this, Flutter's
# Linux desktop build invokes cmake which then finds Windows mingw
# headers via /mnt/c/msys64/... and miscompiles. We want a pure-Linux
# build using WSL gcc + GTK + system libs.
PATH="$(echo "$PATH" | tr ':' '\n' | grep -vE '^/mnt/c/' | paste -sd: -)"
export PATH
unset CMAKE_PREFIX_PATH

# Pre-create native_assets dir so the configure step doesn't error.
mkdir -p build/native_assets/linux

echo "[linux-player] flutter pub get"
"$FLUTTER" pub get

echo "[linux-player] flutter build linux --$BUILD_MODE"
"$FLUTTER" build linux "--$BUILD_MODE"

# Stage shared libs next to the Linux bundle's bundled lib folder.
case "$BUILD_MODE" in
    release) BUNDLE="build/linux/x64/release/bundle" ;;
    profile) BUNDLE="build/linux/x64/profile/bundle" ;;
    debug)   BUNDLE="build/linux/x64/debug/bundle"   ;;
esac
mkdir -p "$BUNDLE/lib"
for f in libbopwire.so libmc_rats.so; do
    src="$MC_LIB_DIR/$f"
    [ -f "$src" ] && cp -f "$src" "$BUNDLE/lib/$f"
done

echo "[done] Linux player bundle in $BUNDLE"
