# ----------------------------------------------------------------------
# _common-windows.ps1
#
# Shared helpers for the per-target Windows build scripts. NOT meant to
# be run directly — every target script dot-sources this with `. $PSScriptRoot\_common-windows.ps1`.
# ----------------------------------------------------------------------

function Write-Step([string]$msg) {
    Write-Host ''
    Write-Host ('==> ' + $msg) -ForegroundColor Cyan
}

function Fail([string]$msg) {
    Write-Host ('ERROR: ' + $msg) -ForegroundColor Red
    exit 1
}

function Get-RepoRoot {
    return (Split-Path -Parent $PSScriptRoot)
}

# ---- Prereq lookups (cached) -----------------------------------------

function Find-MSVC {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return '' }
    return (& $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null)
}

function Find-Vcpkg {
    $repo = Get-RepoRoot
    $candidates = @(
        $env:VCPKG_ROOT,
        (Join-Path $repo 'bopwire\vcpkg'),
        'C:\vcpkg',
        'C:\dev\vcpkg'
    )
    foreach ($c in $candidates) {
        if ($c -and (Test-Path (Join-Path $c 'vcpkg.exe'))) { return $c }
    }
    return ''
}

function Ensure-Vcpkg {
    $found = Find-Vcpkg
    if ($found) { return $found }

    $repo = Get-RepoRoot
    $target = Join-Path $repo 'bopwire\vcpkg'
    Write-Step ('vcpkg not found — cloning into ' + $target)
    git clone --depth 1 https://github.com/microsoft/vcpkg.git $target 2>&1
    if ($LASTEXITCODE -ne 0) { Fail 'git clone of vcpkg failed' }
    & (Join-Path $target 'bootstrap-vcpkg.bat') -disableMetrics
    if ($LASTEXITCODE -ne 0) { Fail 'vcpkg bootstrap failed' }
    return $target
}

function Find-Cmake {
    $c = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    if ($c) { return $c }
    foreach ($p in 'C:\Program Files\CMake\bin\cmake.exe') {
        if (Test-Path $p) { return $p }
    }
    return ''
}

function Find-Flutter {
    $f = (Get-Command flutter -ErrorAction SilentlyContinue).Source
    if ($f) { return $f }
    foreach ($p in 'C:\flutter\bin\flutter.bat',"$env:LOCALAPPDATA\flutter\bin\flutter.bat") {
        if (Test-Path $p) { return $p }
    }
    return ''
}

function Find-JDK17 {
    if ($env:JAVA_HOME -and (Test-Path "$env:JAVA_HOME\bin\javac.exe")) {
        return $env:JAVA_HOME
    }
    foreach ($p in 'C:\Users\lain\jdk17\jdk-17.0.18+8',
                   'C:\Program Files\Eclipse Adoptium\jdk-17',
                   'C:\Program Files\Microsoft\jdk-17') {
        if (Test-Path "$p\bin\javac.exe") { return $p }
    }
    return ''
}

function Find-AndroidSdk {
    if ($env:ANDROID_SDK_ROOT -and (Test-Path "$env:ANDROID_SDK_ROOT\platform-tools\adb.exe")) {
        return $env:ANDROID_SDK_ROOT
    }
    foreach ($p in 'C:\Users\lain\android-sdk',"$env:LOCALAPPDATA\Android\Sdk") {
        if (Test-Path "$p\platform-tools\adb.exe") { return $p }
    }
    return ''
}
