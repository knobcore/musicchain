#!/usr/bin/env bash
# Visible Linux Flutter desktop player build, launched from Windows
# Terminal so the operator can watch flutter / cmake / gcc output live.

set -e
cd /mnt/c/Users/lain/blockchain/bopwire_player

# Strip the inherited Windows PATH so flutter / cmake / gcc don't pick up
# Win32 mingw configs by accident, then add flutter's Linux SDK.
PATH="$(echo "$PATH" | tr ':' '\n' | grep -vE '^/mnt/c/' | paste -sd: -):/root/flutter/bin"
export PATH
unset CMAKE_PREFIX_PATH

export FLUTTER=/root/flutter/bin/flutter
export MC_LIB_DIR=/mnt/c/Users/lain/blockchain/bopwire/build-linux-sys/Release

# The project was scaffolded with --platforms=android,windows only — no
# linux/CMakeLists.txt to drive `flutter build linux`. Add the linux
# scaffold idempotently first; existing linux/flutter / linux/libs are
# preserved.
if [ ! -f linux/CMakeLists.txt ]; then
    echo "[player] adding Linux desktop scaffold (flutter create --platforms=linux .)"
    "$FLUTTER" create --platforms=linux .
fi

bash scripts/build-linux-player.sh
rc=$?

echo
echo "=========================================="
echo "Linux player build finished — exit code $rc"
echo "Bundle: build/linux/x64/release/bundle"
echo "=========================================="
read -rp "press enter to close..."
