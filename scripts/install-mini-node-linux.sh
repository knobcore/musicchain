#!/usr/bin/env bash
# ----------------------------------------------------------------------
# install-mini-node-linux.sh
#
# Install bopwire-mini-node as a systemd service on Linux. Run this
# AFTER ./scripts/build-mini-node-linux.sh has produced the binary at
# bopwire/build-linux-mini/bopwire-mini-node.
#
# What it does:
#   * Copies the binary to /usr/local/bin/bopwire-mini-node
#   * Creates a `bopwire` system user (no shell, no home) if missing
#   * Drops a /etc/systemd/system/bopwire-mini-node.service unit
#   * Owns /var/lib/bopwire-mini-node/ as the data dir
#   * Enables + starts the service
#
# Usage:
#   sudo ./scripts/install-mini-node-linux.sh
#   sudo ./scripts/install-mini-node-linux.sh --uninstall   # tear down
#   sudo ./scripts/install-mini-node-linux.sh --port 9335   # override port
# ----------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN_SRC="$REPO_ROOT/bopwire/build-linux-mini/bopwire-mini-node"
BIN_DST="/usr/local/bin/bopwire-mini-node"
UNIT="/etc/systemd/system/bopwire-mini-node.service"
DATA_DIR="/var/lib/bopwire-mini-node"
USER="bopwire"
PORT="9335"
UNINSTALL=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --uninstall) UNINSTALL=1; shift ;;
        --port)      PORT="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,20p' "$0"
            exit 0
            ;;
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

if [[ $EUID -ne 0 ]]; then
    echo "must be run as root (sudo)" >&2
    exit 1
fi

step() { printf '\n==> %s\n' "$*"; }

# ---- Uninstall path -------------------------------------------------

if [[ $UNINSTALL -eq 1 ]]; then
    step "stopping + disabling bopwire-mini-node.service"
    systemctl disable --now bopwire-mini-node.service 2>/dev/null || true
    rm -f "$UNIT" "$BIN_DST"
    systemctl daemon-reload
    echo "  (data dir $DATA_DIR left in place — remove manually if desired)"
    exit 0
fi

# ---- Install path ---------------------------------------------------

[[ -f "$BIN_SRC" ]] || { echo "missing $BIN_SRC — run build-mini-node-linux.sh first" >&2; exit 1; }

step "creating $USER system user (if missing)"
if ! id "$USER" >/dev/null 2>&1; then
    useradd --system --no-create-home --shell /usr/sbin/nologin "$USER"
fi

step "installing binary to $BIN_DST"
install -m 0755 "$BIN_SRC" "$BIN_DST"

step "creating data dir $DATA_DIR"
install -d -m 0750 -o "$USER" -g "$USER" "$DATA_DIR"

step "writing unit file $UNIT"
cat > "$UNIT" <<EOF
[Unit]
Description=bopwire mini-node (VPS rendezvous)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$USER
Group=$USER
WorkingDirectory=$DATA_DIR
ExecStart=$BIN_DST --port $PORT --data-dir $DATA_DIR
Restart=on-failure
RestartSec=5
# Sandbox the daemon as much as we reasonably can. The mini-node only
# binds one UDP/TCP port and writes to its own data dir, so it doesn't
# need anything else.
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=$DATA_DIR
PrivateTmp=true
PrivateDevices=true
ProtectKernelTunables=true
ProtectKernelModules=true
ProtectControlGroups=true
RestrictSUIDSGID=true
LockPersonality=true
MemoryDenyWriteExecute=true
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
EOF
chmod 0644 "$UNIT"

step "reloading systemd and enabling service"
systemctl daemon-reload
systemctl enable --now bopwire-mini-node.service

step "status"
systemctl --no-pager --full status bopwire-mini-node.service | sed -n '1,12p'
