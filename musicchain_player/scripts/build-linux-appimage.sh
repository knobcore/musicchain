#!/usr/bin/env bash
#
# Package the musicchain Linux player as an AppImage.
#
# Required env vars:
#   FLUTTER                Path to flutter binary
#   MC_LIB_DIR             Folder with libmusicchain.so + libmc_rats.so
#   APPIMAGETOOL           Path to appimagetool binary (download from
#                          https://github.com/AppImage/AppImageKit/releases)
#
# Optional:
#   LINUXDEPLOY            Path to linuxdeploy (auto-includes runtime
#                          libs from /usr). If unset, we skip linuxdeploy
#                          and just bundle the absolute minimum.
#   VERSION=0.7            Version stamped into the AppImage name.
#
# Usage:
#   FLUTTER=/opt/flutter/bin/flutter \
#   MC_LIB_DIR=/opt/musicchain/Release \
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
for f in libmusicchain.so libmc_rats.so; do
    src="$MC_LIB_DIR/$f"
    [ -f "$src" ] && cp -f "$src" "$APPDIR/usr/lib/$f"
done

# AppRun wrapper.
cat > "$APPDIR/AppRun" <<'APPRUN'
#!/usr/bin/env bash
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib:$HERE/usr/bin/lib:${LD_LIBRARY_PATH:-}"
exec "$HERE/usr/bin/musicchain_player" "$@"
APPRUN
chmod +x "$APPDIR/AppRun"

# .desktop entry. Required for appimagetool.
cat > "$APPDIR/musicchain.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=musicchain
Comment=Decentralized music player
Exec=musicchain_player
Icon=musicchain
Categories=AudioVideo;Audio;Music;
Terminal=false
DESKTOP
cp "$APPDIR/musicchain.desktop" "$APPDIR/usr/share/applications/musicchain.desktop"

# Icon. Use a placeholder if the player doesn't ship one.
ICON_SRC="assets/icon.png"
if [ -f "$ICON_SRC" ]; then
    cp "$ICON_SRC" "$APPDIR/musicchain.png"
    cp "$ICON_SRC" "$APPDIR/usr/share/icons/hicolor/256x256/apps/musicchain.png"
else
    # Empty 1×1 PNG so appimagetool doesn't refuse to build.
    printf '\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x06\x00\x00\x00\x1f\x15\xc4\x89\x00\x00\x00\rIDATx\xdac\xf8\x00\x00\x00\x01\x00\x01\x5c\xcd\xff\x69\x00\x00\x00\x00IEND\xaeB`\x82' > "$APPDIR/musicchain.png"
fi

if [ -n "${LINUXDEPLOY:-}" ] && [ -x "$LINUXDEPLOY" ]; then
    echo "[appimage] running linuxdeploy to bundle runtime libs"
    "$LINUXDEPLOY" --appdir "$APPDIR" --executable "$APPDIR/usr/bin/musicchain_player"
fi

OUTPUT="build/musicchain-player-${VERSION}-x86_64.AppImage"
echo "[appimage] building $OUTPUT"
ARCH=x86_64 "$APPIMAGETOOL" "$APPDIR" "$OUTPUT"
echo "[done] $OUTPUT ($(du -h "$OUTPUT" | cut -f1))"
