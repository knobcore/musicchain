#!/usr/bin/env bash
# One-shot: apt-install libsnappy-dev (and anything else discovered during
# the VPS-side build), then re-run the build.

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
    "apt-get install -y --no-install-recommends libsnappy-dev && \
     cd /opt/musicchain-src && \
     git fetch --depth 1 origin main && git reset --hard FETCH_HEAD && \
     cd musicchain && bash scripts/build-node-linux.sh && \
     mkdir -p /opt/musicchain && \
     for f in musicchain-node musicchain-mini-node libmusicchain.so \
              libmc_rats.so libmc_rats.so.1 libmc_rats.so.1.0.0.0 \
              full-node.config.json mini-node.config.json; do \
       src=build-linux/Release/\$f; \
       if [ -f \"\$src\" ] || [ -L \"\$src\" ]; then \
         cp -af \"\$src\" /opt/musicchain/; \
       fi; \
     done && \
     for f in build-linux/deps/librats/lib/libmc_rats.so*; do \
       [ -f \"\$f\" ] || [ -L \"\$f\" ] && cp -af \"\$f\" /opt/musicchain/ || true; \
     done && \
     chmod +x /opt/musicchain/musicchain-node /opt/musicchain/musicchain-mini-node && \
     echo && echo '==== /opt/musicchain ====' && ls -la /opt/musicchain && \
     echo && echo '==== ldd musicchain-node ====' && \
     LD_LIBRARY_PATH=/opt/musicchain ldd /opt/musicchain/musicchain-node | grep 'not found' || echo 'all node libs resolved' && \
     echo && echo '==== ldd musicchain-mini-node ====' && \
     LD_LIBRARY_PATH=/opt/musicchain ldd /opt/musicchain/musicchain-mini-node | grep 'not found' || echo 'all mini-node libs resolved'"
rc=$?
unset SSHPASS

echo
echo "=========================================="
echo "apt + rebuild exit code: $rc"
echo "=========================================="
read -rp "press enter to close..."
