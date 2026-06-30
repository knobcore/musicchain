#!/bin/bash
NDKBIN=/mnt/c/Users/lain/android-sdk/ndk/27.0.12077973/toolchains/llvm/prebuilt/linux-x86_64/bin
echo "-- gcc-named tools in NDK bin --"
ls $NDKBIN | grep gcc || echo "(none — NDK ships only clang)"
echo "-- aarch64 clang variants --"
ls $NDKBIN | grep aarch64
