#!/usr/bin/env bash
# Run the mini-node on the VPS in a visible WSL tab. Logs stream to this
# terminal; Ctrl-C stops it cleanly. Credentials are entered at runtime,
# never stored.

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

echo "[mini-node] running on $VPS_HOST in foreground — Ctrl-C stops it"
echo "=========================================="
# -t -t forces a PTY so signals (Ctrl-C) make it across.
# WorkingDirectory ensures mini-node.seed lives next to the binary so it
# persists across runs; the operator just relaunches and the same wallet
# loads automatically.
sshpass -e ssh -tt \
    -o StrictHostKeyChecking=accept-new \
    -o UserKnownHostsFile=/dev/null \
    "$VPS_USER@$VPS_HOST" \
    "cd /opt/musicchain && LD_LIBRARY_PATH=/opt/musicchain ./musicchain-mini-node --config /opt/musicchain/mini-node.config.json"
rc=$?
unset SSHPASS

echo
echo "=========================================="
echo "mini-node exited (rc=$rc)"
read -rp "press enter to close..."
