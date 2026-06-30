# ----------------------------------------------------------------------
# build-player-android.ps1
#
# Clean-slate Flutter Android release APK build of bopwire_player.
# Gradle's external native build compiles librats + secp256k1 +
# chromaprint_jni against the Android NDK. The prebuilt OpenSSL +
# chromaprint .so files in android/app/src/main/jniLibs/arm64-v8a/
# must already be in place — those are produced by the
# build-openssl-android.sh helpers shipped alongside this script.
#
# Usage:
#   .\scripts\build-player-android.ps1
#   .\scripts\build-player-android.ps1 -Clean         # flutter clean first
#   .\scripts\build-player-android.ps1 -OutputDir x   # copy app-release.apk to x\
# ----------------------------------------------------------------------

[CmdletBinding()]
param(
    [switch]$Clean,
    [string]$OutputDir = ''
)

# Stay on 'Continue' so flutter / gradle / cmake stderr noise (NDK
# notes, deprecated-API warnings) doesn't trip PowerShell's
# ErrorRecord wrapper. We rely on $LASTEXITCODE after each native call.
$ErrorActionPreference = 'Continue'
. $PSScriptRoot\_common-windows.ps1

$repo      = Get-RepoRoot
$playerDir = Join-Path $repo 'bopwire_player'
$flutter   = Find-Flutter
$jdk       = Find-JDK17
$sdk       = Find-AndroidSdk

if (-not $flutter) { Fail 'Flutter SDK not found.' }
if (-not $jdk)     { Fail 'JDK 17 not found (winget install EclipseAdoptium.Temurin.17.JDK).' }
if (-not $sdk)     { Fail 'Android SDK not found.' }

# Confirm jniLibs prebuilts are present — Gradle won't fail loudly when
# they're missing, the APK just crashes at first librats call.
$abi = Join-Path $playerDir 'android\app\src\main\jniLibs\arm64-v8a'
foreach ($lib in 'libssl.so','libcrypto.so','libchromaprint.so') {
    if (-not (Test-Path (Join-Path $abi $lib))) {
        Fail ("missing jniLibs prebuilt: $lib (run scripts\build-openssl-android.sh and chromaprint cross-compile first)")
    }
}

$env:JAVA_HOME        = $jdk
$env:ANDROID_SDK_ROOT = $sdk

Push-Location $playerDir
try {
    & $flutter config --jdk-dir $jdk | Out-Null

    if ($Clean) {
        Write-Step 'flutter clean'
        & $flutter clean
    }

    Write-Step 'flutter pub get'
    & $flutter pub get
    if ($LASTEXITCODE -ne 0) { Fail 'flutter pub get failed' }

    Write-Step 'flutter build apk --release'
    & $flutter build apk --release
    if ($LASTEXITCODE -ne 0) { Fail 'flutter build apk failed' }
} finally {
    Pop-Location
}

$apk = Join-Path $playerDir 'build\app\outputs\flutter-apk\app-release.apk'
if (-not (Test-Path $apk)) { Fail "couldn't find $apk" }

Write-Step 'Build artifact'
Write-Host ('  ' + $apk) -ForegroundColor Green

if ($OutputDir) {
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
    Copy-Item $apk $OutputDir -Force
    Write-Step ('Staged copy -> ' + $OutputDir)
}
