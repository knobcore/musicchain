#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Build the bopwire Windows player (Flutter desktop).

.DESCRIPTION
  Required env vars (no defaults):
    FLUTTER     Path to flutter.bat / flutter binary
    MC_DLL_DIR  Where to find a freshly-built bopwire.dll +
                runtime DLLs to stage into the Flutter build.
                The Flutter Windows build picks DLLs out of this
                folder via the embedder's native_assets manifest.

  Optional:
    BUILD_MODE=release     (release | profile | debug)

  Usage:
    $env:FLUTTER='C:\flutter\bin\flutter.bat'
    $env:MC_DLL_DIR='C:\Users\lain\tracker\Release\node-windows'
    pwsh scripts\build-windows-player.ps1
#>

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$FLUTTER    = $env:FLUTTER
$MC_DLL_DIR = $env:MC_DLL_DIR
if (-not $FLUTTER)    { Write-Error "FLUTTER not set — point at flutter.bat" }
if (-not $MC_DLL_DIR) { Write-Error "MC_DLL_DIR not set — point at a folder holding bopwire.dll + mc_rats.dll" }

$BUILD_MODE = if ($env:BUILD_MODE) { $env:BUILD_MODE } else { "release" }

$repoRoot = (Resolve-Path "$PSScriptRoot\..").Path
Set-Location $repoRoot

# Pre-create the folder the Flutter Windows build expects so the INSTALL
# step doesn't error before native_assets ever runs (this is the bug
# captured in build-node-windows lore — the folder must exist even when
# empty).
$nativeAssets = "build/native_assets/windows"
if (-not (Test-Path $nativeAssets)) { New-Item -ItemType Directory -Force $nativeAssets | Out-Null }

Write-Host "[windows-player] flutter pub get"
& $FLUTTER pub get
if ($LASTEXITCODE -ne 0) { throw "flutter pub get failed" }

Write-Host "[windows-player] flutter build windows --$BUILD_MODE"
& $FLUTTER build windows "--$BUILD_MODE"
if ($LASTEXITCODE -ne 0) { throw "flutter build windows failed" }

# Stage the DLL set into the built bundle so the .exe finds them at
# runtime. Flutter's Windows desktop build copies DLLs that exist next
# to the .exe at launch time.
$bundleDir = "build/windows/x64/runner/$($BUILD_MODE.Substring(0,1).ToUpper() + $BUILD_MODE.Substring(1))"
if (-not (Test-Path $bundleDir)) {
    Write-Warning "Expected bundle dir not found: $bundleDir"
} else {
    Write-Host "[windows-player] staging DLLs from $MC_DLL_DIR -> $bundleDir"
    Get-ChildItem -Path $MC_DLL_DIR -Filter *.dll | ForEach-Object {
        Copy-Item -Force $_.FullName (Join-Path $bundleDir $_.Name)
    }
}

Write-Host "[done] Windows player bundle in $bundleDir"
