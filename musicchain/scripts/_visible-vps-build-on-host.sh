#!/usr/bin/env bash
# Clone (or fast-pull) the repo on the VPS and build musicchain-node +
# musicchain-mini-node in place. Avoids cross-distro library mismatches
# (Ubuntu 22.04 FFmpeg 4 vs Debian 12 FFmpeg 5, etc.) and gives us
# librats's SONAME-correct symlinks right where the binaries expect
# them. Run this after install-deps-debian.sh has populated the dev
# packages.

set -e
PATH="$(echo "$PATH" | tr ':' '\n' | grep -vE '^/mnt/c/' | paste -sd: -)"
export PATH

VPS_HOST="${VPS_HOST:-85.239.238.226}"
VPS_USER="${VPS_USER:-root}"
REPO_URL="${REPO_URL:-https://github.com/knobcore/musicchain.git}"
REPO_REF="${REPO_REF:-main}"

if [ -z "${VPS_PASS:-}" ]; then
    echo -n "SSH password for $VPS_USER@$VPS_HOST: "
    read -rs VPS_PASS
    echo
fi
export SSHPASS="$VPS_PASS"
unset VPS_PASS

REMOTE_SCRIPT=$(cat <<EOS
set -e
cd /opt
if [ ! -d musicchain-src ]; then
    git clone --depth 1 --branch '$REPO_REF' '$REPO_URL' musicchain-src
else
    cd musicchain-src
    git fetch --depth 1 origin '$REPO_REF'
    git reset --hard FETCH_HEAD
    cd /opt
fi
cd musicchain-src/musicchain
bash scripts/build-node-linux.sh
# Stage what we just built into the deploy directory. The build script
# already drops the .so files and configs into build-linux/Release;
# we just copy them across so the systemd / launcher paths don't change.
mkdir -p /opt/musicchain
for f in musicchain-node musicchain-mini-node \
         libmusicchain.so libmc_rats.so libmc_rats.so.1 \
         libmc_rats.so.1.0.0.0 \
         full-node.config.json mini-node.config.json; do
    src=build-linux/Release/\$f
    if [ -f "\$src" ] || [ -L "\$src" ]; then
        cp -af "\$src" /opt/musicchain/
    fi
done
# librats also drops its real .so under deps/librats/lib in some builds.
for f in build-linux/deps/librats/lib/libmc_rats.so*; do
    [ -f "\$f" ] || [ -L "\$f" ] && cp -af "\$f" /opt/musicchain/ || true
done
chmod +x /opt/musicchain/musicchain-node /opt/musicchain/musicchain-mini-node
echo
echo '==== /opt/musicchain after build ===='
ls -la /opt/musicchain
echo
echo '==== ldd musicchain-node ===='
LD_LIBRARY_PATH=/opt/musicchain ldd /opt/musicchain/musicchain-node | grep 'not found' || echo 'all node libs resolved'
echo
echo '==== ldd musicchain-mini-node ===='
LD_LIBRARY_PATH=/opt/musicchain ldd /opt/musicchain/musicchain-mini-node | grep 'not found' || echo 'all mini-node libs resolved'
EOS
)

sshpass -e ssh -tt \
    -o StrictHostKeyChecking=accept-new \
    -o UserKnownHostsFile=/dev/null \
    "$VPS_USER@$VPS_HOST" \
    "$REMOTE_SCRIPT"
rc=$?
unset SSHPASS

echo
echo "=========================================="
echo "remote build exit code: $rc"
echo "=========================================="
read -rp "press enter to close..."
