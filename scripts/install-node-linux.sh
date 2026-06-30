#!/usr/bin/env bash
# ----------------------------------------------------------------------
# install-node-linux.sh
#
# Install the FULL bopwire-node as a systemd service on Linux. Run
# this AFTER ./scripts/build-node-linux.sh has produced the binary at
# bopwire/build-linux/bopwire-node.
#
# What it does:
#   * Copies the binary to /usr/local/bin/bopwire-node
#   * Creates a `bopwire` system user (no shell, no home) if missing
#   * Owns /var/lib/bopwire/{blockchain.db,blocks,keys,logs,audio,dmca,kyc}
#   * Drops a /etc/systemd/system/bopwire-node.service unit
#   * Enables + starts the service in --no-tui mode (daemon)
#
# Usage:
#   sudo ./scripts/install-node-linux.sh
#   sudo ./scripts/install-node-linux.sh --uninstall  # tear down
#   sudo ./scripts/install-node-linux.sh --data-dir /custom/path
# ----------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN_SRC="$REPO_ROOT/bopwire/build-linux/bopwire-node"
BIN_DST="/usr/local/bin/bopwire-node"
UNIT="/etc/systemd/system/bopwire-node.service"
DATA_DIR="/var/lib/bopwire"
USER="bopwire"
UNINSTALL=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --uninstall) UNINSTALL=1; shift ;;
        --data-dir)  DATA_DIR="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,21p' "$0"
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
    step "stopping + disabling bopwire-node.service"
    systemctl disable --now bopwire-node.service 2>/dev/null || true
    rm -f "$UNIT" "$BIN_DST"
    systemctl daemon-reload
    echo "  (chain data at $DATA_DIR left in place — remove manually if you want to wipe it)"
    exit 0
fi

# ---- Install path ---------------------------------------------------

[[ -f "$BIN_SRC" ]] || { echo "missing $BIN_SRC — run build-node-linux.sh first" >&2; exit 1; }

step "creating $USER system user (if missing)"
if ! id "$USER" >/dev/null 2>&1; then
    useradd --system --no-create-home --shell /usr/sbin/nologin "$USER"
fi

step "installing binary to $BIN_DST"
install -m 0755 "$BIN_SRC" "$BIN_DST"

step "creating data subdirs under $DATA_DIR"
for sub in . blockchain.db blocks keys logs audio dmca kyc; do
    install -d -m 0750 -o "$USER" -g "$USER" "$DATA_DIR/$sub"
done

step "writing unit file $UNIT"
cat > "$UNIT" <<EOF
[Unit]
Description=bopwire full home node (chain + swarm + RPC)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$USER
Group=$USER
WorkingDirectory=$DATA_DIR
# --no-tui (alias for --daemon) so systemd doesn't get a raw-mode TTY.
# The TUI is for foreground use only.
ExecStart=$BIN_DST start --data-dir $DATA_DIR --no-tui
Restart=on-failure
RestartSec=10
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
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
EOF
chmod 0644 "$UNIT"

step "reloading systemd and enabling service"
systemctl daemon-reload
systemctl enable --now bopwire-node.service

step "status"
systemctl --no-pager --full status bopwire-node.service | sed -n '1,15p'
