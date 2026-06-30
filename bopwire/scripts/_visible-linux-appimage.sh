#!/usr/bin/env bash
# Visible AppImage packaging step, launched from Windows Terminal.

set -e
cd /mnt/c/Users/lain/blockchain/bopwire_player

PATH="$(echo "$PATH" | tr ':' '\n' | grep -vE '^/mnt/c/' | paste -sd: -):/root/flutter/bin"
export PATH
unset CMAKE_PREFIX_PATH

# Fetch the static type-2 runtime once, into /root/runtime-x86_64 so the
# next run finds it cached.
RUNTIME=/root/runtime-x86_64
if [ ! -f "$RUNTIME" ]; then
    echo "[appimage] fetching static type-2 runtime"
    wget -q https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64 -O "$RUNTIME"
    chmod +x "$RUNTIME"
fi

export FLUTTER=/root/flutter/bin/flutter
export MC_LIB_DIR=/mnt/c/Users/lain/blockchain/bopwire/build-linux-sys/Release
export APPIMAGETOOL=/root/appimagetool
export LINUXDEPLOY=/root/linuxdeploy
export STATIC_RUNTIME="$RUNTIME"
export VERSION=0.7.1

bash scripts/build-linux-appimage.sh
rc=$?

echo
echo "=========================================="
echo "AppImage packaging finished — exit code $rc"
echo "Find the .AppImage in bopwire_player/build/"
echo "=========================================="
read -rp "press enter to close..."
