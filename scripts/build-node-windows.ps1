# ----------------------------------------------------------------------
# build-node-windows.ps1
#
# Clean-slate build of bopwire-node.exe for Windows x64. Configures
# vcpkg (cloning + bootstrapping it if missing), then cmake + MSVC.
#
# Usage:
#   .\scripts\build-node-windows.ps1                # build, leave under build-win64/
#   .\scripts\build-node-windows.ps1 -Clean         # wipe build-win64/ first
#   .\scripts\build-node-windows.ps1 -OutputDir x   # also copy artifacts to x\
# ----------------------------------------------------------------------

[CmdletBinding()]
param(
    [switch]$Clean,
    [string]$OutputDir = ''
)

# Stay on 'Continue' so MSVC's chatty stderr (deprecation notes etc.)
# doesn't trip PowerShell's ErrorRecord wrapper. We rely on
# $LASTEXITCODE after each native call instead.
$ErrorActionPreference = 'Continue'
. $PSScriptRoot\_common-windows.ps1

$repo     = Get-RepoRoot
$srcDir   = Join-Path $repo 'bopwire'
$buildDir = Join-Path $srcDir 'build-win64'

# ---- Prereqs ---------------------------------------------------------

$msvc  = Find-MSVC
$cmake = Find-Cmake
if (-not $msvc)  { Fail 'Visual Studio 2022 with "Desktop development with C++" not found.' }
if (-not $cmake) { Fail 'cmake not found (winget install Kitware.CMake).' }
$vcpkg = Ensure-Vcpkg

# ---- Clean -----------------------------------------------------------

if ($Clean -and (Test-Path $buildDir)) {
    Write-Step ('Wiping ' + $buildDir)
    Remove-Item $buildDir -Recurse -Force -ErrorAction SilentlyContinue
}

# ---- Configure -------------------------------------------------------

Write-Step 'cmake configure (Visual Studio 17 2022, x64)'
$toolchain = Join-Path $vcpkg 'scripts\buildsystems\vcpkg.cmake'
& $cmake -S $srcDir -B $buildDir -G 'Visual Studio 17 2022' -A x64 `
    ('-DCMAKE_TOOLCHAIN_FILE=' + $toolchain) `
    '-DVCPKG_TARGET_TRIPLET=x64-windows'
if ($LASTEXITCODE -ne 0) { Fail 'cmake configure failed' }

# ---- Build -----------------------------------------------------------

Write-Step 'cmake --build (Release, target bopwire-node)'
& $cmake --build $buildDir --config Release --target bopwire-node
if ($LASTEXITCODE -ne 0) { Fail 'build failed' }

$releaseDir = Join-Path $buildDir 'Release'
$exe        = Join-Path $releaseDir 'bopwire-node.exe'
if (-not (Test-Path $exe)) { Fail 'bopwire-node.exe not produced' }

# librats's CMakeLists.txt writes the shared library to its own
# bin\Release subdir instead of the parent project's Release dir.
# Pull it (and any other rats-side DLLs) next to bopwire-node.exe
# so the runtime loader finds it.
$librrelease = Join-Path $buildDir 'deps\librats\bin\Release'
if (Test-Path $librrelease) {
    Get-ChildItem $librrelease -Filter *.dll -ErrorAction SilentlyContinue |
        ForEach-Object { Copy-Item $_.FullName $releaseDir -Force }
}

Write-Step 'Build artifacts'
Write-Host ('  ' + $exe) -ForegroundColor Green
Get-ChildItem $releaseDir -Filter *.dll -ErrorAction SilentlyContinue |
    ForEach-Object { Write-Host ('  ' + $_.FullName) }

if ($OutputDir) {
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
    Copy-Item $exe $OutputDir -Force
    Get-ChildItem $releaseDir -Filter *.dll -ErrorAction SilentlyContinue |
        ForEach-Object { Copy-Item $_.FullName $OutputDir -Force }
    Write-Step ('Staged copy -> ' + $OutputDir)
}
