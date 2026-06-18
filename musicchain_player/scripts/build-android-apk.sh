#!/usr/bin/env bash
#
# Build the musicchain player Android APK.
#
# Required env vars:
#   FLUTTER         Path to flutter binary       (no default)
#   ANDROID_SDK_ROOT Path to Android SDK         (no default)
#   ANDROID_NDK_HOME Path to Android NDK         (no default)
#   JAVA_HOME       Path to JDK 17               (no default)
#
# Optional:
#   APK_OUT=build/app/outputs/flutter-apk/app-release.apk
#   FLUTTER_TARGET=release       (release | debug | profile)
#
# Usage from musicchain_player root:
#   FLUTTER=/opt/flutter/bin/flutter \
#   ANDROID_SDK_ROOT=$HOME/Android/Sdk \
#   ANDROID_NDK_HOME=$HOME/Android/Sdk/ndk/26.1.10909125 \
#   JAVA_HOME=/usr/lib/jvm/jdk-17 \
#   ./scripts/build-android-apk.sh

set -euo pipefail

: "${FLUTTER:?FLUTTER not set — path to flutter binary}"
: "${ANDROID_SDK_ROOT:?ANDROID_SDK_ROOT not set}"
: "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME not set}"
: "${JAVA_HOME:?JAVA_HOME not set — must point at a JDK 17 install}"

FLUTTER_TARGET="${FLUTTER_TARGET:-release}"
APK_OUT="${APK_OUT:-build/app/outputs/flutter-apk/app-release.apk}"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

export ANDROID_SDK_ROOT ANDROID_NDK_HOME JAVA_HOME
export PATH="$JAVA_HOME/bin:$ANDROID_SDK_ROOT/platform-tools:$PATH"

echo "[android] flutter pub get"
"$FLUTTER" pub get

echo "[android] flutter build apk --$FLUTTER_TARGET"
"$FLUTTER" build apk "--$FLUTTER_TARGET"

if [ -f "$APK_OUT" ]; then
    echo "[done] APK at $APK_OUT ($(du -h "$APK_OUT" | cut -f1))"
else
    echo "[warn] expected APK at $APK_OUT, not found"
    find build/app/outputs -name "*.apk" -print
fi
