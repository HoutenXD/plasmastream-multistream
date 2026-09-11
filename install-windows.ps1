# Copy a built plugin into the folder OBS actually scans on Windows.
#
#   powershell -ExecutionPolicy Bypass -File install-windows.ps1
#
# PROGRAMDATA, not APPDATA. Every other platform puts user plugins under the
# user config directory; Windows does not. See the README for the line in OBS's
# source that decides this. Getting it wrong fails silently: OBS never scans the
# folder, so its log holds no error and no mention of the plugin at all.
#
# OBS has to be CLOSED. It loads the plugin at startup and holds the DLL open
# for as long as it runs, so a copy over a running OBS fails with a file lock.

param(
    [string]$Configuration = "RelWithDebInfo"
)

$ErrorActionPreference = "Stop"

$name = "plasmastream-multistream"
$src = Join-Path $PSScriptRoot "build_x64\rundir\$Configuration"
$dest = Join-Path $env:ProgramData "obs-studio\plugins\$name"

if (-not (Test-Path "$src\$name.dll")) {
    Write-Host "No build found at $src" -ForegroundColor Red
    Write-Host "Build it first:"
    Write-Host "  cmake --preset windows-x64"
    Write-Host "  cmake --build build_x64 --config $Configuration"
    exit 1
}

$running = Get-Process obs64 -ErrorAction SilentlyContinue

if ($running) {
    Write-Host "OBS is running (pid $($running.Id))." -ForegroundColor Yellow
    Write-Host "Close it and run this again. The DLL is locked while OBS holds it."
    exit 1
}

New-Item -ItemType Directory -Force -Path "$dest\bin\64bit" | Out-Null
New-Item -ItemType Directory -Force -Path "$dest\data\locale" | Out-Null

Copy-Item "$src\$name.dll" "$dest\bin\64bit\" -Force

# The symbols are optional and only useful for reading a crash dump, so a build
# without them is not a failed install.
if (Test-Path "$src\$name.pdb") {
    Copy-Item "$src\$name.pdb" "$dest\bin\64bit\" -Force
}

Copy-Item "$src\$name\locale\*.ini" "$dest\data\locale\" -Force

Write-Host "Installed to $dest" -ForegroundColor Green
Write-Host "Start OBS, then tick PlasmaStream Multistream in the Docks menu."
Write-Host "Docks is its own menu in the menu bar, next to View, not inside it."
