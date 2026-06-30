#!/usr/bin/env bash
# Publish webplayer/frontend/ to the GitHub Pages repo (bopwire/bopwire.github.io).
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

echo "[publish] copying frontend -> $DEST"
# Copy the site files. Keep the repo's CNAME (custom domain) intact.
cp "$SRC/index.html" "$SRC/styles.css" "$SRC/app.js" "$SRC/config.js" \
   "$SRC/logo.png" "$SRC/favicon.png" "$DEST/"

# Ensure the custom domain file survives (GitHub Pages needs it).
[[ -f "$DEST/CNAME" ]] || echo "bopwire.com" > "$DEST/CNAME"

cd "$DEST"
git add index.html styles.css app.js config.js logo.png favicon.png CNAME
if git diff --cached --quiet; then
  echo "[publish] no changes."
else
  git commit -m "web player: publish Discover front-end"
  git push
  echo "[publish] pushed. Live at https://bopwire.com after Pages rebuilds."
fi
