# ----------------------------------------------------------------------
# build-player-windows.ps1
#
# Clean-slate Flutter Windows desktop build of bopwire_player.
#
# Usage:
#   .\scripts\build-player-windows.ps1
#   .\scripts\build-player-windows.ps1 -Clean        # flutter clean first
#   .\scripts\build-player-windows.ps1 -OutputDir x  # copy Release\ into x\
# ----------------------------------------------------------------------

[CmdletBinding()]
param(
    [switch]$Clean,
    [string]$OutputDir = ''
)

# Stay on 'Continue' so flutter.bat's chatty stderr (Nuget download
# notes, CMake dev warnings) doesn't trip PowerShell's ErrorRecord
# wrapper. We rely on $LASTEXITCODE after each native call instead.
$ErrorActionPreference = 'Continue'
. $PSScriptRoot\_common-windows.ps1

$repo      = Get-RepoRoot
$playerDir = Join-Path $repo 'bopwire_player'
$flutter   = Find-Flutter
if (-not $flutter) {
    Fail 'Flutter SDK not found (download https://docs.flutter.dev/get-started/install/windows).'
}

# Flutter's Windows build needs MSVC + the Desktop workload.
$msvc = Find-MSVC
if (-not $msvc) {
    Fail 'Visual Studio 2022 with "Desktop development with C++" is required for flutter build windows.'
}

Push-Location $playerDir
try {
    if ($Clean) {
        Write-Step 'flutter clean'
        & $flutter clean
    }

    Write-Step 'flutter pub get'
    & $flutter pub get
    if ($LASTEXITCODE -ne 0) { Fail 'flutter pub get failed' }

    # Pre-create build/native_assets/windows/. Flutter's CMakeLists.txt
    # has an unconditional install rule that copies this dir; when no
    # plugin emits native assets the dir doesn't exist and CMake's
    # file(INSTALL TYPE DIRECTORY) returns a misleading "No error" —
    # MSBuild then bubbles MSB3073 even though bopwire_player.exe is
    # already in place. Pre-creating the empty dir sidesteps it.
    $nativeAssets = Join-Path $playerDir 'build\native_assets\windows'
    New-Item -ItemType Directory -Path $nativeAssets -Force | Out-Null

    Write-Step 'flutter build windows --release'
    & $flutter build windows --release
    $rc = $LASTEXITCODE
} finally {
    Pop-Location
}

# Locate the produced Release\ folder. Flutter has moved this around
# across versions; check both known layouts.
$candidates = @(
    (Join-Path $playerDir 'build\windows\x64\runner\Release'),
    (Join-Path $playerDir 'build\windows\runner\Release')
)
$releaseDir = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $releaseDir) {
    Fail "flutter build windows failed (rc=$rc): no Release\ folder produced."
}

# The build can return non-zero from the INSTALL step yet still produce
# a complete Release\ folder. Treat the presence of the .exe as the
# real success signal.
$exe = Join-Path $releaseDir 'bopwire_player.exe'
if (-not (Test-Path $exe)) {
    Fail "flutter build windows failed (rc=$rc): no bopwire_player.exe under $releaseDir"
}

Write-Step 'Build artifacts'
Write-Host ('  ' + $releaseDir) -ForegroundColor Green

if ($OutputDir) {
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
    Copy-Item -Path (Join-Path $releaseDir '*') -Destination $OutputDir -Recurse -Force
    Write-Step ('Staged copy -> ' + $OutputDir)
}
