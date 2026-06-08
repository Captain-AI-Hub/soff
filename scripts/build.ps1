param(
    [ValidateSet("release", "debug")]
    [string]$Mode = "release",
    [switch]$IdaPlugin,
    [string]$IdaSdk = "ida-sdk-93-main/src",
    [switch]$SkipSoff,
    [switch]$SkipDesktop,
    [string]$DesktopBundles = ""
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$DesktopDir = Join-Path $RepoRoot "desktop"
$ResourcesDir = Join-Path $DesktopDir "src-tauri/resources"

function Invoke-Checked {
    param(
        [string]$Command,
        [string[]]$Arguments,
        [string]$WorkingDirectory = $RepoRoot
    )

    Write-Host "==> $Command $($Arguments -join ' ')"
    Push-Location $WorkingDirectory
    try {
        & $Command @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw "$Command exited with code $LASTEXITCODE"
        }
    }
    finally {
        Pop-Location
    }
}

function Get-SoffFfiName {
    if ($IsWindows -or $env:OS -eq "Windows_NT") {
        return "soff_ffi.dll"
    }
    if ($IsMacOS) {
        return "libsoff_ffi.dylib"
    }
    return "libsoff_ffi.so"
}

function Copy-SoffFfiToDesktop {
    $ffiName = Get-SoffFfiName
    $ffi = Get-ChildItem -Path (Join-Path $RepoRoot "build") -Recurse -Filter $ffiName |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $ffi) {
        throw "$ffiName was not found under build/"
    }

    New-Item -ItemType Directory -Force -Path $ResourcesDir | Out-Null
    Copy-Item -LiteralPath $ffi.FullName -Destination (Join-Path $ResourcesDir $ffiName) -Force
    Write-Host "Copied $($ffi.FullName) -> $ResourcesDir"
}

if (-not $SkipSoff) {
    $configArgs = @("config", "-y", "-m", $Mode)
    if ($IdaPlugin) {
        $configArgs += @("--ida_plugin=y", "--ida_sdk=$IdaSdk")
    }
    else {
        $configArgs += "--ida_plugin=n"
    }

    Invoke-Checked "xmake" $configArgs
    Invoke-Checked "xmake" @("require", "-y")

    $targets = @("soff_cli", "soff_smoke", "soff_ffi")
    if ($IdaPlugin) {
        $targets += "soff_ida"
    }
    foreach ($target in $targets) {
        Invoke-Checked "xmake" @("build", "-y", $target)
    }
    Copy-SoffFfiToDesktop
}

if (-not $SkipDesktop) {
    if (-not (Test-Path (Join-Path $DesktopDir "node_modules"))) {
        Invoke-Checked "bun" @("install") $DesktopDir
    }
    if ($DesktopBundles) {
        Invoke-Checked "bun" @("run", "tauri", "build", "--", "--bundles", $DesktopBundles) $DesktopDir
    }
    else {
        Invoke-Checked "bun" @("run", "tauri", "build") $DesktopDir
    }
}

Write-Host "Build complete."
