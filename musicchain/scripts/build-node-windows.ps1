#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Build musicchain-node.exe + musicchain.dll on Windows via vcpkg + CMake.

.DESCRIPTION
  All paths are env-var driven so this script ports cleanly to a new dev
  box. Required tools the user supplies (defaults marked):

    VCPKG_ROOT       Path to vcpkg checkout (no default — must be set or
                     the script aborts).
    CMAKE            cmake.exe              [default: cmake]
    BUILD_DIR        out-of-tree build dir  [default: build-win64]
    OUTPUT_DIR       where to copy final artifacts. Defaults to
                     ${BUILD_DIR}/Release.
    CLEAN            "1" to wipe build dir first.

  Run from the musicchain root:
    pwsh scripts\build-node-windows.ps1
#>

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Resolve-EnvDefault {
    param([string]$Name, [string]$Default)
    $value = [Environment]::GetEnvironmentVariable($Name)
    if ($value) { return $value }
    return $Default
}

$VCPKG_ROOT  = $env:VCPKG_ROOT
if (-not $VCPKG_ROOT) {
    Write-Error "VCPKG_ROOT not set. Point it at your vcpkg checkout."
}
$CMAKE       = Resolve-EnvDefault "CMAKE"       "cmake"
$BUILD_DIR   = Resolve-EnvDefault "BUILD_DIR"   "build-win64"
$OUTPUT_DIR  = Resolve-EnvDefault "OUTPUT_DIR"  (Join-Path $BUILD_DIR "Release")
$CLEAN       = Resolve-EnvDefault "CLEAN"       "0"

$repoRoot = (Resolve-Path "$PSScriptRoot\..").Path
Set-Location $repoRoot

if ($CLEAN -eq "1") {
    Write-Host "[clean] removing $BUILD_DIR"
    if (Test-Path $BUILD_DIR) { Remove-Item -Recurse -Force $BUILD_DIR }
}

if (-not (Test-Path "$BUILD_DIR/CMakeCache.txt")) {
    Write-Host "[configure] $CMAKE -S . -B $BUILD_DIR"
    & $CMAKE -S . -B $BUILD_DIR `
        -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
        -DVCPKG_TARGET_TRIPLET=x64-windows `
        -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
}

Write-Host "[build] $CMAKE --build $BUILD_DIR --config Release"
& $CMAKE --build $BUILD_DIR --config Release
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

# Stage the final node executable + DLL + runtime deps to OUTPUT_DIR.
$rel = Join-Path $BUILD_DIR "Release"
if (-not (Test-Path $OUTPUT_DIR)) { New-Item -ItemType Directory -Force $OUTPUT_DIR | Out-Null }
foreach ($f in @("musicchain-node.exe", "musicchain-mini-node.exe", "musicchain.dll")) {
    $src = Join-Path $rel $f
    if (Test-Path $src) { Copy-Item -Force $src (Join-Path $OUTPUT_DIR $f) }
}
# Copy the librats DLL (built into deps/librats/bin/Release).
$ratsDll = Join-Path $BUILD_DIR "deps/librats/bin/Release/mc_rats.dll"
if (Test-Path $ratsDll) { Copy-Item -Force $ratsDll (Join-Path $OUTPUT_DIR "mc_rats.dll") }

# Ship the default config files alongside the binaries so an operator
# can edit a real file rather than guessing the schema. The node binaries
# look for them in the same directory as the .exe on startup.
foreach ($cfg in @("full-node.config.json", "mini-node.config.json")) {
    $cfgSrc = Join-Path $repoRoot "config/$cfg"
    if (Test-Path $cfgSrc) { Copy-Item -Force $cfgSrc (Join-Path $OUTPUT_DIR $cfg) }
}

Write-Host "[done] artifacts in $OUTPUT_DIR"
