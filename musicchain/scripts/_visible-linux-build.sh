#!/usr/bin/env bash
# Wrapper invoked by Windows Terminal so the build runs visibly in WSL
# instead of being piped through Claude's tool harness. Cleans any stale
# .o files from the last attempt, runs the actual build, then pauses so
# the terminal window stays open for inspection.

set -e
cd /mnt/c/Users/lain/blockchain/musicchain

# Stale .o cleanup. multi_decoder + jsonrpc_server need fresh compiles
# against the FFmpeg-4-compat shim and the vendored httplib. libwally +
# secp256k1 also need to be rebuilt with -fPIC so they can land inside
# the shared libmusicchain.so — without -fPIC the linker errors out
# with R_X86_64_PC32 on every libwally internal helper.
rm -f build-linux-sys/CMakeFiles/musicchain.dir/src/audio/multi_decoder.cpp.o \
      build-linux-sys/CMakeFiles/musicchain_static.dir/src/audio/multi_decoder.cpp.o \
      build-linux-sys/CMakeFiles/musicchain.dir/src/api/jsonrpc_server.cpp.o \
      build-linux-sys/CMakeFiles/musicchain_static.dir/src/api/jsonrpc_server.cpp.o \
      2>/dev/null || true
rm -rf build-linux-sys/deps/libwally-core 2>/dev/null || true

BUILD_DIR=build-linux-sys \
OUTPUT_DIR=build-linux-sys/Release \
JOBS="$(nproc)" \
bash scripts/build-node-linux.sh
rc=$?

echo
echo "=========================================="
echo "Build finished — exit code $rc"
echo "Artifacts (if any): build-linux-sys/Release/"
echo "=========================================="
read -rp "press enter to close..."
