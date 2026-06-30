#!/usr/bin/env bash
# ----------------------------------------------------------------------
# build-mini-node-linux.sh
#
# Clean-slate build of bopwire-mini-node for Linux x86_64. The
# mini-node is the VPS rendezvous binary — it doesn't link any media
# deps, just OpenSSL + the vendored librats. This script installs the
# minimum apt packages it needs, then cmake-builds with the
# MC_MINI_NODE_ONLY flag so we don't drag in chromaprint / ffmpeg.
#
# Usage:
#   ./scripts/build-mini-node-linux.sh                 # build
#   ./scripts/build-mini-node-linux.sh --clean         # wipe build-linux/
#   ./scripts/build-mini-node-linux.sh --output DIR    # also copy binary to DIR
# ----------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_DIR="$REPO_ROOT/bopwire"
BUILD_DIR="$SRC_DIR/build-linux-mini"
OUTPUT_DIR=""
CLEAN=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean)  CLEAN=1; shift ;;
        --output) OUTPUT_DIR="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,15p' "$0"
            exit 0
            ;;
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

step() { printf '\n==> %s\n' "$*"; }
fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

# ---- apt deps (mini-node only) --------------------------------------

step "apt-get install (mini-node deps)"
DEBIAN_FRONTEND=noninteractive
PKGS="build-essential cmake pkg-config git libssl-dev"
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

step "cmake configure (MC_MINI_NODE_ONLY=ON, Release)"
cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMC_MINI_NODE_ONLY=ON

# ---- Build ----------------------------------------------------------

step "cmake --build (target bopwire-mini-node)"
cmake --build "$BUILD_DIR" --target bopwire-mini-node -j"$(nproc)"

BIN="$BUILD_DIR/bopwire-mini-node"
[[ -f "$BIN" ]] || fail "bopwire-mini-node not produced"

step "Build artifact"
echo "  $BIN"
file "$BIN" 2>/dev/null || true

if [[ -n "$OUTPUT_DIR" ]]; then
    mkdir -p "$OUTPUT_DIR"
    cp "$BIN" "$OUTPUT_DIR/"
    step "Staged copy -> $OUTPUT_DIR/bopwire-mini-node"
fi
