#!/usr/bin/env bash
#
# Deploy the Linux full node + mini-node + default configs to the VPS.
#
# Required env vars:
#   VPS_HOST     hostname or IP
#   VPS_USER     ssh user (root works)
#   VPS_PASS     ssh password (only set if sshpass is in PATH; for keys
#                use ssh-agent and leave VPS_PASS empty)
#
# Optional:
#   VPS_INSTALL_DIR=/opt/musicchain
#   VPS_DATA_DIR=/var/lib/musicchain
#   ARTIFACT_DIR=build-linux/Release       relative to musicchain root
#   START_NOW=0                             "1" enables + starts the
#                                           systemd unit immediately;
#                                           default leaves them disabled
#                                           so the operator decides when.
#
# Usage:
#   VPS_HOST=85.239.238.226 VPS_USER=root VPS_PASS='secret' \
#       bash scripts/deploy-vps.sh

set -euo pipefail

: "${VPS_HOST:?VPS_HOST not set}"
: "${VPS_USER:?VPS_USER not set}"
VPS_INSTALL_DIR="${VPS_INSTALL_DIR:-/opt/musicchain}"
VPS_DATA_DIR="${VPS_DATA_DIR:-/var/lib/musicchain}"
ARTIFACT_DIR="${ARTIFACT_DIR:-build-linux/Release}"
START_NOW="${START_NOW:-0}"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

if [ ! -f "$ARTIFACT_DIR/musicchain-node" ]; then
    echo "[deploy] $ARTIFACT_DIR/musicchain-node not found." >&2
    echo "        Run scripts/build-node-linux.sh first." >&2
    exit 1
fi

# SSH transport. We deliberately avoid eval'ing a quoted password into a
# shell line: passwords with @ / $ / spaces / backslashes blow that up.
# Instead, when VPS_PASS is set, we export SSHPASS and use `sshpass -e`,
# which reads the password from the environment with zero shell parsing.
SSH_OPTS=(-o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=/dev/null)
USE_SSHPASS=0
if [ -n "${VPS_PASS:-}" ]; then
    if ! command -v sshpass >/dev/null; then
        echo "[deploy] VPS_PASS set but sshpass not installed." >&2
        echo "        apt-get install sshpass, or unset VPS_PASS and use a key." >&2
        exit 1
    fi
    export SSHPASS="$VPS_PASS"
    USE_SSHPASS=1
fi

run() {
    if [ "$USE_SSHPASS" = 1 ]; then
        sshpass -e ssh "${SSH_OPTS[@]}" "$VPS_USER@$VPS_HOST" "$@"
    else
        ssh "${SSH_OPTS[@]}" "$VPS_USER@$VPS_HOST" "$@"
    fi
}
put() {
    if [ "$USE_SSHPASS" = 1 ]; then
        sshpass -e scp "${SSH_OPTS[@]}" "$1" "$VPS_USER@$VPS_HOST:$2"
    else
        scp "${SSH_OPTS[@]}" "$1" "$VPS_USER@$VPS_HOST:$2"
    fi
}

echo "[deploy] preparing $VPS_INSTALL_DIR and $VPS_DATA_DIR on $VPS_HOST"
run "mkdir -p $VPS_INSTALL_DIR $VPS_DATA_DIR/blockchain.db $VPS_DATA_DIR/blocks $VPS_DATA_DIR/keys $VPS_DATA_DIR/logs $VPS_DATA_DIR/audio"

echo "[deploy] copying binaries + configs"
put "$ARTIFACT_DIR/musicchain-node"             "$VPS_INSTALL_DIR/"
put "$ARTIFACT_DIR/musicchain-mini-node"        "$VPS_INSTALL_DIR/"
[ -f "$ARTIFACT_DIR/libmusicchain.so" ] && put "$ARTIFACT_DIR/libmusicchain.so" "$VPS_INSTALL_DIR/"
[ -f "$ARTIFACT_DIR/libmc_rats.so" ]    && put "$ARTIFACT_DIR/libmc_rats.so"    "$VPS_INSTALL_DIR/"
put "config/full-node.config.json"              "$VPS_INSTALL_DIR/"
put "config/mini-node.config.json"              "$VPS_INSTALL_DIR/"
run "chmod +x $VPS_INSTALL_DIR/musicchain-node $VPS_INSTALL_DIR/musicchain-mini-node"

# Patch full-node config so data_dir + tui_mode match the VPS layout.
run "python3 -c \"import json,sys; p='$VPS_INSTALL_DIR/full-node.config.json'; \
d=json.load(open(p)); d['data_dir']='$VPS_DATA_DIR'; d['tui_mode']=False; \
open(p,'w').write(json.dumps(d,indent=2))\""

echo "[deploy] writing systemd units"
run "cat > /etc/systemd/system/musicchain-mini-node.service <<'EOF'
[Unit]
Description=musicchain mini-node (VPS rendezvous relay)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=$VPS_INSTALL_DIR
ExecStart=$VPS_INSTALL_DIR/musicchain-mini-node --config $VPS_INSTALL_DIR/mini-node.config.json --quiet
Restart=always
RestartSec=5
LimitNOFILE=65535

[Install]
WantedBy=multi-user.target
EOF"

run "cat > /etc/systemd/system/musicchain-node.service <<'EOF'
[Unit]
Description=musicchain full node (VPS-hosted)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=$VPS_INSTALL_DIR
Environment=LD_LIBRARY_PATH=$VPS_INSTALL_DIR
ExecStart=$VPS_INSTALL_DIR/musicchain-node start --config $VPS_INSTALL_DIR/full-node.config.json --no-tui
Restart=always
RestartSec=5
LimitNOFILE=65535

[Install]
WantedBy=multi-user.target
EOF"

run "systemctl daemon-reload"

if [ "$START_NOW" = "1" ]; then
    echo "[deploy] enabling + starting both services"
    run "systemctl enable --now musicchain-mini-node.service"
    run "systemctl enable --now musicchain-node.service"
else
    echo "[deploy] services installed but NOT started."
    echo "        On the VPS:"
    echo "          systemctl enable --now musicchain-mini-node.service"
    echo "          systemctl enable --now musicchain-node.service"
    echo "        Or set START_NOW=1 on the next deploy."
fi

echo "[deploy] done"
