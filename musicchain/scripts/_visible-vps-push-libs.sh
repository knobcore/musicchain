#!/usr/bin/env bash
# Push the vendored shared libraries (libmc_rats.so + libmusicchain.so)
# to /opt/musicchain on the VPS, then ldd both binaries with the
# LD_LIBRARY_PATH the launcher uses so we can confirm everything resolves
# before re-running the nodes.

set -e
PATH="$(echo "$PATH" | tr ':' '\n' | grep -vE '^/mnt/c/' | paste -sd: -)"
export PATH

VPS_HOST="${VPS_HOST:-85.239.238.226}"
VPS_USER="${VPS_USER:-root}"
ARTIFACTS=/mnt/c/Users/lain/blockchain/musicchain/build-linux-sys/Release

if [ -z "${VPS_PASS:-}" ]; then
    echo -n "SSH password for $VPS_USER@$VPS_HOST: "
    read -rs VPS_PASS
    echo
fi
export SSHPASS="$VPS_PASS"
unset VPS_PASS

for f in libmc_rats.so libmusicchain.so; do
    if [ ! -f "$ARTIFACTS/$f" ]; then
        echo "[skip] $ARTIFACTS/$f not built — re-run the Linux build first"
        continue
    fi
    echo "[push] $f -> $VPS_HOST:/opt/musicchain/"
    sshpass -e scp \
        -o StrictHostKeyChecking=accept-new \
        -o UserKnownHostsFile=/dev/null \
        "$ARTIFACTS/$f" \
        "$VPS_USER@$VPS_HOST:/opt/musicchain/"
done

echo
echo "[verify] running ldd against both binaries with LD_LIBRARY_PATH set"
sshpass -e ssh -tt \
    -o StrictHostKeyChecking=accept-new \
    -o UserKnownHostsFile=/dev/null \
    "$VPS_USER@$VPS_HOST" \
    "chmod +x /opt/musicchain/musicchain-node /opt/musicchain/musicchain-mini-node; \
     echo '==== /opt/musicchain ===='; ls -la /opt/musicchain | head -30; \
     echo; echo '==== ldd musicchain-mini-node ===='; \
     LD_LIBRARY_PATH=/opt/musicchain ldd /opt/musicchain/musicchain-mini-node; \
     echo; echo '==== ldd musicchain-node ===='; \
     LD_LIBRARY_PATH=/opt/musicchain ldd /opt/musicchain/musicchain-node; \
     echo; echo '==== unresolved libs (should be empty) ===='; \
     LD_LIBRARY_PATH=/opt/musicchain ldd /opt/musicchain/musicchain-mini-node /opt/musicchain/musicchain-node 2>&1 | grep 'not found' || echo 'all libs resolved'"
unset SSHPASS

echo
read -rp "press enter to close..."
