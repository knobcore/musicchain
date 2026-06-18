#!/usr/bin/env bash
# Visible VPS deploy launcher. Prompts for the SSH password at runtime
# so the credential never lands in a file, environment dump, or process
# argv. The deploy script reads it via $SSHPASS which sshpass consumes
# without printing.

set -e
cd /mnt/c/Users/lain/blockchain/musicchain

PATH="$(echo "$PATH" | tr ':' '\n' | grep -vE '^/mnt/c/' | paste -sd: -)"
export PATH

# Default host can stay in the script — it's already public infra. The
# password must be entered live each time.
export VPS_HOST="${VPS_HOST:-85.239.238.226}"
export VPS_USER="${VPS_USER:-root}"
export ARTIFACT_DIR="${ARTIFACT_DIR:-build-linux-sys/Release}"
export START_NOW="${START_NOW:-0}"

if [ -z "${VPS_PASS:-}" ]; then
    echo -n "SSH password for $VPS_USER@$VPS_HOST: "
    # -s = no echo. -r = no backslash escapes. The value lives only in
    # this process's memory; not exported via export -p, not written to
    # a file, not visible in /proc/$PID/cmdline.
    read -rs VPS_PASS
    echo
fi
# Export only after read; the deploy script picks it up and immediately
# rewrites it into $SSHPASS for sshpass -e.
export VPS_PASS

bash scripts/deploy-vps.sh
rc=$?

# Scrub the password from our env before we hand control back to the
# user's shell.
unset VPS_PASS SSHPASS

echo
echo "=========================================="
echo "VPS deploy finished — exit code $rc"
echo "Mini-node + full-node binaries + configs are on $VPS_HOST:/opt/musicchain"
echo "Both services installed, NOT yet enabled."
echo "=========================================="
read -rp "press enter to close..."
