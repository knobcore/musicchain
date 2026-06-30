#!/bin/bash
# Cross-compile quictls (OpenSSL fork bundled in msquic) for Android arm64.
# Output: static libcrypto.a + libssl.a in /tmp/oa64/install
set -e
NDK=/mnt/c/Users/lain/android-sdk/ndk/27.0.12077973
TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64
# OpenSSL's android-arm64 Configure target hard-codes the
# `aarch64-linux-android<api>-clang` prefix in $PATH, so injecting
# the NDK toolchain bin is mandatory even though we also export CC/CXX.
export ANDROID_NDK_ROOT=$NDK
export PATH=$TOOLCHAIN/bin:$PATH
export CC=$TOOLCHAIN/bin/aarch64-linux-android26-clang
export CXX=$TOOLCHAIN/bin/aarch64-linux-android26-clang++
export AR=$TOOLCHAIN/bin/llvm-ar
export RANLIB=$TOOLCHAIN/bin/llvm-ranlib

OUT=/tmp/oa64
SRC=/mnt/c/Users/lain/blockchain/bopwire/deps/msh3/msquic/submodules/quictls

rm -rf "$OUT"
mkdir -p "$OUT"
cd "$OUT"
cp -r "$SRC"/* .
echo "[oa64] configuring..."
./Configure android-arm64 no-shared no-tests no-asm no-dso \
  -D__ANDROID_API__=26 --prefix=$OUT/install 2>&1 | tail -5
echo "[oa64] making (this is the slow bit)..."
make -j4 2>&1 | tail -8
echo "[oa64] installing headers + libs..."
make install_sw 2>&1 | tail -5
echo "[oa64] done. Libs:"
ls -la $OUT/install/lib*/libcrypto.a $OUT/install/lib*/libssl.a 2>&1 | head -3
echo "[oa64] include:"
ls $OUT/install/include/openssl | head
