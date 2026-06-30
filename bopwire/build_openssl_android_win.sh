#!/bin/bash
# Cross-compile quictls for Android arm64 using the WINDOWS NDK from msys2 bash.
# Output: static libcrypto.a + libssl.a under /tmp/oa64-win/install
#
# Why this needs a custom wrapper: OpenSSL's Configure android-arm64 target
# hard-codes `aarch64-linux-android-gcc` (with an optional API suffix) as
# its PATH search. The Windows NDK ships `aarch64-linux-android26-clang.cmd`
# (a batch wrapper around clang.exe) — different name, different extension.
# We synthesize a thin shell wrapper with the expected name that calls the
# real .cmd, prepend its dir to PATH, then Configure is happy.
set -e
NDK="/c/Users/lain/android-sdk/ndk/27.0.12077973"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/windows-x86_64"
BIN="$TOOLCHAIN/bin"

OUT=/c/build-oa64-win
SRC="/c/Users/lain/blockchain/bopwire/deps/msh3/msquic/submodules/quictls"

rm -rf "$OUT"
mkdir -p "$OUT/wrap"
WRAP="$OUT/wrap"

# Create the names OpenSSL's android target looks for, each delegating to
# the NDK's actual clang.cmd. clang -m32/-m64 is sorted out by --target.
echo "[oa64-win] WRAP dir: $WRAP"
ls -la "$WRAP" || { echo "WRAP dir missing"; exit 1; }

# Perl's File::Which (used by OpenSSL Configure on Windows) needs the
# wrapper to have a Windows-recognised extension (.cmd/.bat/.exe).
# Shell-script-no-extension wrappers are invisible to it.
BIN_WIN="$(cygpath -wa "$BIN" | tr '\\' '/')"
cat > "$WRAP/aarch64-linux-android-gcc.cmd"   <<EOF
@echo off
"$BIN_WIN/clang.exe" --target=aarch64-linux-android26 %*
EOF
cat > "$WRAP/aarch64-linux-android-g++.cmd"   <<EOF
@echo off
"$BIN_WIN/clang++.exe" --target=aarch64-linux-android26 %*
EOF
cat > "$WRAP/aarch64-linux-android-clang.cmd" <<EOF
@echo off
"$BIN_WIN/clang.exe" --target=aarch64-linux-android26 %*
EOF
echo "[oa64-win] wrappers:"
ls -la "$WRAP"
echo "[oa64-win] sanity-check wrapper output (-v):"
cmd.exe //c "$(cygpath -wa "$WRAP/aarch64-linux-android-gcc.cmd")" -v 2>&1 | head -3

NDK_WIN_FWDSLASH="$(cygpath -wa "$NDK" | tr '\\' '/')"
export ANDROID_NDK_ROOT="$NDK_WIN_FWDSLASH"
export ANDROID_NDK_HOME="$NDK_WIN_FWDSLASH"
echo "[oa64-win] ANDROID_NDK_ROOT=$ANDROID_NDK_ROOT"
echo "[oa64-win] perl sees: $(perl -e 'print $ENV{ANDROID_NDK_ROOT}//qq{<undef>}')"
export PATH="$WRAP:$BIN:$PATH"
export AR="$BIN/llvm-ar.exe"
export RANLIB="$BIN/llvm-ranlib.exe"

cd "$OUT"
cp -r "$SRC"/* .

echo "[oa64-win] configuring..."
./Configure android-arm64 no-shared no-tests no-asm no-dso \
  -D__ANDROID_API__=26 --prefix=$OUT/install 2>&1 | tail -8

if [ ! -f Makefile ]; then
    echo "[oa64-win] FAIL: Configure did not produce a Makefile"
    exit 1
fi

echo "[oa64-win] make build_libs..."
make -j4 build_libs 2>&1 | tail -8 || make -j4 2>&1 | tail -8

echo "[oa64-win] install..."
make install_dev 2>&1 | tail -5 || make install_sw 2>&1 | tail -5 || make install 2>&1 | tail -5

echo "[oa64-win] done. Libs:"
ls -la $OUT/install/lib*/libcrypto.a $OUT/install/lib*/libssl.a 2>&1 | head
echo "[oa64-win] include:"
ls $OUT/install/include/openssl 2>&1 | head
