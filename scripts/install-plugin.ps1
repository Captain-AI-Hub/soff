param(
    [string]$IdaDir = "D:\IDAPro9.3",
    [string]$BuildDir = "build"
)

$plugin = Get-ChildItem -Path $BuildDir -Recurse -Filter soff.dll | Select-Object -First 1
if (-not $plugin) {
    throw "soff.dll was not found under $BuildDir"
}

Copy-Item -LiteralPath $plugin.FullName -Destination (Join-Path $IdaDir "plugins\soff.dll") -Force
Write-Host "Installed $($plugin.FullName) to $IdaDir\plugins"
