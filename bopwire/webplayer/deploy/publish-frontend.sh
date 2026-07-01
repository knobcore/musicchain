#!/usr/bin/env bash
# Publish webplayer/frontend/ to the GitHub Pages repo (bopwire/bopwire.github.io)
# under the /player/ subpath, so the marketing landing page at the root stays put
# (its "Open the web player" button links to player/).
# Usage:
#   deploy/publish-frontend.sh [/path/to/bopwire.github.io]
# With no arg it clones the Pages repo into a temp dir via gh.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # webplayer/
SRC="$HERE/frontend"

if [[ $# -ge 1 ]]; then
  DEST="$1"
else
  DEST="$(mktemp -d)/bopwire.github.io"
  gh repo clone bopwire/bopwire.github.io "$DEST"
fi

echo "[publish] copying frontend -> $DEST/player/"
mkdir -p "$DEST/player/decoders"
cp "$SRC/index.html" "$SRC/dmca.html" "$SRC/styles.css" "$SRC/app.js" "$SRC/config.js" "$SRC/wasm-player.js" \
   "$SRC/logo.png" "$SRC/favicon.png" "$DEST/player/"
cp "$SRC"/decoders/*.js "$DEST/player/decoders/"

cd "$DEST"
git add player
if git diff --cached --quiet; then
  echo "[publish] no changes."
else
  git commit -m "web player: publish Discover front-end to /player/"
  git push
  echo "[publish] pushed. Live at https://bopwire.com/player/ after Pages rebuilds."
fi
