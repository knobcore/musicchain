#!/usr/bin/env bash
# Build the bopwire-web-gateway on the VPS. Run from anywhere; set SRC to your
# checkout (defaults to the layout in build-deploy notes).
set -euo pipefail

SRC="${SRC:-/opt/bopwire-src/bopwire}"
BUILD="${BUILD:-$SRC/build-linux}"
JOBS="${JOBS:-3}"

echo "[build] reconfiguring $BUILD (picks up the new CMake target)…"
cmake -S "$SRC" -B "$BUILD" >/dev/null

echo "[build] compiling bopwire-web-gateway…"
cmake --build "$BUILD" --target bopwire-web-gateway --parallel "$JOBS"

echo "[build] done -> $BUILD/bopwire-web-gateway"
ldd "$BUILD/bopwire-web-gateway" | grep -i rats || true
