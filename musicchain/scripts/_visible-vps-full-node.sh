#!/usr/bin/env bash
# Run the full node on the VPS in a visible WSL tab. Same pattern as the
# mini-node launcher — password entered live, logs stream here, Ctrl-C
# stops cleanly.

set -e

PATH="$(echo "$PATH" | tr ':' '\n' | grep -vE '^/mnt/c/' | paste -sd: -)"
export PATH

VPS_HOST="${VPS_HOST:-85.239.238.226}"
VPS_USER="${VPS_USER:-root}"

if [ -z "${VPS_PASS:-}" ]; then
    echo -n "SSH password for $VPS_USER@$VPS_HOST: "
    read -rs VPS_PASS
    echo
fi
export SSHPASS="$VPS_PASS"
unset VPS_PASS

echo "[full-node] running on $VPS_HOST in foreground — Ctrl-C stops it"
echo "=========================================="
# LD_LIBRARY_PATH so musicchain-node finds the libmc_rats.so + the
# vendored libmusicchain.so we scp'd next to it. --no-tui so we get
# plain stdout/stderr instead of the ncurses TUI which doesn't behave
# over a forwarded shell.
sshpass -e ssh -tt \
    -o StrictHostKeyChecking=accept-new \
    -o UserKnownHostsFile=/dev/null \
    "$VPS_USER@$VPS_HOST" \
    "cd /opt/musicchain && LD_LIBRARY_PATH=/opt/musicchain ./musicchain-node start --config /opt/musicchain/full-node.config.json --no-tui"
rc=$?
unset SSHPASS

echo
echo "=========================================="
echo "full-node exited (rc=$rc)"
read -rp "press enter to close..."
