#!/usr/bin/env bash
# One-shot helper: ssh into the VPS, ldd both binaries, list anything
# the runtime linker can't resolve. We can decide from the output
# whether we need to apt-install a system lib, ship a .so, or rebuild.

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

sshpass -e ssh -tt \
    -o StrictHostKeyChecking=accept-new \
    -o UserKnownHostsFile=/dev/null \
    "$VPS_USER@$VPS_HOST" \
    "echo '==== /opt/musicchain ===='; ls -la /opt/musicchain; \
     echo; echo '==== ldd musicchain-mini-node ===='; \
     LD_LIBRARY_PATH=/opt/musicchain ldd /opt/musicchain/musicchain-mini-node || true; \
     echo; echo '==== ldd musicchain-node ===='; \
     LD_LIBRARY_PATH=/opt/musicchain ldd /opt/musicchain/musicchain-node || true; \
     echo; echo '==== missing system libs ===='; \
     LD_LIBRARY_PATH=/opt/musicchain ldd /opt/musicchain/musicchain-mini-node | grep 'not found' || true; \
     LD_LIBRARY_PATH=/opt/musicchain ldd /opt/musicchain/musicchain-node | grep 'not found' || true; \
     echo; echo 'done'"
unset SSHPASS

read -rp "press enter to close..."
