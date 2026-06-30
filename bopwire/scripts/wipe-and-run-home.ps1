#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Stop the home node if running, wipe its chain state, and re-start it.

.DESCRIPTION
  Full reset — wipes EVERYTHING tied to the previous chain identity so a
  fresh bootstrap can pick a new founder seed:
    - $DATA_DIR/blockchain.db   (leveldb)
    - $DATA_DIR/blocks          (.blk dump tree)
    - $DATA_DIR/logs            (old log files)
    - $DATA_DIR/keys            (the founder keypair — gone)
    - $DATA_DIR/founder.seed    (the seed bytes — gone)
    - $DATA_DIR/dmca            (DMCA submissions tied to old chain)
    - $DATA_DIR/kyc             (KYC submissions tied to old chain)
    - $DATA_DIR/moderator.txt   (moderator handle cache)

  Env vars:
    DATA_DIR      Default: C:\Users\lain\Music\bopwire
    NODE_EXE      Default: build-win64\Release\bopwire-node.exe
                  (resolved relative to the bopwire repo root)
    CONFIG_PATH   Default: build-win64\Release\full-node.config.json

  Usage:
    pwsh scripts\wipe-and-run-home.ps1
#>

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot   = (Resolve-Path "$PSScriptRoot\..").Path
$DATA_DIR   = if ($env:DATA_DIR)   { $env:DATA_DIR }   else { "C:\Users\lain\Music\bopwire" }
$NODE_EXE   = if ($env:NODE_EXE)   { $env:NODE_EXE }   else { Join-Path $repoRoot "build-win64\Release\bopwire-node.exe" }
$CONFIG_PATH= if ($env:CONFIG_PATH){ $env:CONFIG_PATH }else { Join-Path $repoRoot "build-win64\Release\full-node.config.json" }

if (-not (Test-Path $NODE_EXE)) {
    Write-Error "Node binary not found: $NODE_EXE"
}

# Stop any running bopwire-node so it releases the leveldb lock.
$running = Get-Process bopwire-node -ErrorAction SilentlyContinue
if ($running) {
    Write-Host "[wipe] stopping running bopwire-node (pid $($running.Id))"
    $running | Stop-Process -Force
    Start-Sleep -Seconds 2
}

Write-Host "[wipe] DATA_DIR=$DATA_DIR"
# Directories: full recursive wipe + recreate empty.
foreach ($sub in @("blockchain.db", "blocks", "logs", "keys", "dmca", "kyc")) {
    $p = Join-Path $DATA_DIR $sub
    if (Test-Path $p) {
        Write-Host "[wipe] rm -rf $p"
        Remove-Item -Recurse -Force $p
    }
}
# Loose files at the data-dir root (founder.seed, moderator.txt, etc.).
foreach ($f in @("founder.seed", "moderator.txt")) {
    $p = Join-Path $DATA_DIR $f
    if (Test-Path $p) {
        Write-Host "[wipe] rm $p"
        Remove-Item -Force $p
    }
}
# Re-create the dirs the node expects to find on startup.
foreach ($sub in @("blockchain.db", "blocks", "logs", "keys")) {
    New-Item -ItemType Directory -Force (Join-Path $DATA_DIR $sub) | Out-Null
}

# Patch the config to point at the home data_dir (in case it was checked
# in with ./data).
if (Test-Path $CONFIG_PATH) {
    $cfg = Get-Content $CONFIG_PATH -Raw | ConvertFrom-Json
    $cfg.data_dir = $DATA_DIR
    $cfg | ConvertTo-Json -Depth 10 | Set-Content -Encoding UTF8 $CONFIG_PATH
    Write-Host "[wipe] patched $CONFIG_PATH data_dir -> $DATA_DIR"
}

Write-Host "[run] starting $NODE_EXE start --config $CONFIG_PATH"
& $NODE_EXE start --config $CONFIG_PATH
