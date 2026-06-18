#!/usr/bin/env bash
# One-shot: scp the Debian install-deps script to the VPS and run it
# under sudo. apt-gets every runtime + dev package the full node
# dynamically links against (libchromaprint, libavcodec, libleveldb,
# libuv, libssl, libcurl, libncurses, libminiupnpc, etc.). After this,
# the musicchain-node binary's dynamic-linker resolution clears.

set -e
PATH="$(echo "$PATH" | tr ':' '\n' | grep -vE '^/mnt/c/' | paste -sd: -)"
export PATH

VPS_HOST="${VPS_HOST:-85.239.238.226}"
VPS_USER="${VPS_USER:-root}"
LOCAL_INSTALL="$(cd "$(dirname "$0")" && pwd)/install-deps-debian.sh"

if [ ! -f "$LOCAL_INSTALL" ]; then
    echo "[FATAL] $LOCAL_INSTALL not found"
    exit 1
fi

if [ -z "${VPS_PASS:-}" ]; then
    echo -n "SSH password for $VPS_USER@$VPS_HOST: "
    read -rs VPS_PASS
    echo
fi
export SSHPASS="$VPS_PASS"
unset VPS_PASS

echo "[push] scp install-deps-debian.sh → $VPS_HOST:/opt/musicchain/"
sshpass -e scp \
    -o StrictHostKeyChecking=accept-new \
    -o UserKnownHostsFile=/dev/null \
    "$LOCAL_INSTALL" \
    "$VPS_USER@$VPS_HOST:/opt/musicchain/install-deps-debian.sh"

echo "[run] apt-getting deps on $VPS_HOST (this takes a couple of minutes)"
sshpass -e ssh -tt \
    -o StrictHostKeyChecking=accept-new \
    -o UserKnownHostsFile=/dev/null \
    "$VPS_USER@$VPS_HOST" \
    "chmod +x /opt/musicchain/install-deps-debian.sh && \
     APT_OPTS='-y --no-install-recommends' \
     bash /opt/musicchain/install-deps-debian.sh"
rc=$?
unset SSHPASS

echo
echo "=========================================="
echo "install-deps exit code: $rc"
echo "If 0, both the mini-node and full-node binaries on the VPS now"
echo "have every shared library they need. ldd should clear."
echo "=========================================="
read -rp "press enter to close..."
