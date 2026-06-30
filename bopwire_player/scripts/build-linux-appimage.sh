#!/usr/bin/env bash
#
# Package the bopwire Linux player as an AppImage.
#
# Required env vars:
#   FLUTTER                Path to flutter binary
#   MC_LIB_DIR             Folder with libbopwire.so + libmc_rats.so
#   APPIMAGETOOL           Path to appimagetool binary (download from
#                          https://github.com/AppImage/AppImageKit/releases)
#
# Optional:
#   LINUXDEPLOY            Path to linuxdeploy (auto-includes runtime
#                          libs from /usr). If unset, we skip linuxdeploy
#                          and just bundle the absolute minimum.
#   STATIC_RUNTIME         Path to a type2-runtime binary
#                          (https://github.com/AppImage/type2-runtime/releases).
#                          When set, appimagetool embeds this instead of
#                          using the default libfuse2-based runtime, so
#                          the AppImage runs on hosts that don't have
#                          libfuse2 installed. This is what the user
#                          asked for with "self-extracting (static)".
#   VERSION=0.7            Version stamped into the AppImage name.
#
# Usage:
#   FLUTTER=/opt/flutter/bin/flutter \
#   MC_LIB_DIR=/opt/bopwire/Release \
#   APPIMAGETOOL=$HOME/bin/appimagetool-x86_64.AppImage \
#   LINUXDEPLOY=$HOME/bin/linuxdeploy-x86_64.AppImage \
#   ./scripts/build-linux-appimage.sh

set -euo pipefail

: "${FLUTTER:?FLUTTER not set}"
: "${MC_LIB_DIR:?MC_LIB_DIR not set}"
: "${APPIMAGETOOL:?APPIMAGETOOL not set}"

VERSION="${VERSION:-0.7}"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

# Build the regular Linux player first.
FLUTTER="$FLUTTER" MC_LIB_DIR="$MC_LIB_DIR" BUILD_MODE=release \
    bash ./scripts/build-linux-player.sh

BUNDLE="build/linux/x64/release/bundle"
APPDIR="build/AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps"

# Copy the Flutter bundle into the AppDir.
cp -r "$BUNDLE/." "$APPDIR/usr/bin/"
# Stage native libs next to the binary so the runtime loader (with the
# AppRun wrapper below adding usr/bin to LD_LIBRARY_PATH) picks them up.
for f in libbopwire.so libmc_rats.so; do
    src="$MC_LIB_DIR/$f"
    [ -f "$src" ] && cp -f "$src" "$APPDIR/usr/lib/$f"
done

# AppRun wrapper.
cat > "$APPDIR/AppRun" <<'APPRUN'
#!/usr/bin/env bash
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib:$HERE/usr/bin/lib:${LD_LIBRARY_PATH:-}"
exec "$HERE/usr/bin/bopwire_player" "$@"
APPRUN
chmod +x "$APPDIR/AppRun"

# .desktop entry. Required for appimagetool.
cat > "$APPDIR/bopwire.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=bopwire
Comment=Decentralized music player
Exec=bopwire_player
Icon=bopwire
Categories=AudioVideo;Audio;Music;
Terminal=false
DESKTOP
cp "$APPDIR/bopwire.desktop" "$APPDIR/usr/share/applications/bopwire.desktop"

# Icon. Probe order:
#   1. assets/icon.png — preferred location once the player ships a real icon
#   2. android/app/src/main/res/mipmap-xxxhdpi/ic_launcher.png — 192×192,
#      already in the repo via Android scaffold, reused so we don't have to
#      hand-author a separate file for the Linux side.
#   3. embedded 1×1 PNG placeholder so appimagetool / linuxdeploy don't
#      refuse to build at all.
#
# Whichever wins, the file is copied to BOTH the AppDir root (where the
# .desktop file's Icon= entry resolves) AND to usr/share/icons/hicolor/...
# (which is where linuxdeploy specifically scans).
ICON_DEST_HICOLOR="$APPDIR/usr/share/icons/hicolor/256x256/apps/bopwire.png"
mkdir -p "$(dirname "$ICON_DEST_HICOLOR")"
if [ -f "assets/icon.png" ]; then
    ICON_SRC="assets/icon.png"
elif [ -f "android/app/src/main/res/mipmap-xxxhdpi/ic_launcher.png" ]; then
    ICON_SRC="android/app/src/main/res/mipmap-xxxhdpi/ic_launcher.png"
else
    ICON_SRC=""
fi
if [ -n "$ICON_SRC" ]; then
    cp "$ICON_SRC" "$APPDIR/bopwire.png"
    cp "$ICON_SRC" "$ICON_DEST_HICOLOR"
else
    printf '\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x06\x00\x00\x00\x1f\x15\xc4\x89\x00\x00\x00\rIDATx\xdac\xf8\x00\x00\x00\x01\x00\x01\x5c\xcd\xff\x69\x00\x00\x00\x00IEND\xaeB`\x82' > "$APPDIR/bopwire.png"
    cp "$APPDIR/bopwire.png" "$ICON_DEST_HICOLOR"
fi

if [ -n "${LINUXDEPLOY:-}" ] && [ -x "$LINUXDEPLOY" ]; then
    echo "[appimage] running linuxdeploy to bundle runtime libs"
    "$LINUXDEPLOY" --appdir "$APPDIR" --executable "$APPDIR/usr/bin/bopwire_player"
fi

OUTPUT="build/bopwire-player-${VERSION}-x86_64.AppImage"
echo "[appimage] building $OUTPUT"
APPIMAGETOOL_ARGS=("$APPDIR" "$OUTPUT")
if [ -n "${STATIC_RUNTIME:-}" ] && [ -f "$STATIC_RUNTIME" ]; then
    echo "[appimage] embedding static runtime $STATIC_RUNTIME (libfuse2-free)"
    APPIMAGETOOL_ARGS=(--runtime-file "$STATIC_RUNTIME" "${APPIMAGETOOL_ARGS[@]}")
fi
ARCH=x86_64 "$APPIMAGETOOL" "${APPIMAGETOOL_ARGS[@]}"
echo "[done] $OUTPUT ($(du -h "$OUTPUT" | cut -f1))"
