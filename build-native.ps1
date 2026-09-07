# Fast local build with the ESP-IDF installed on this machine (see CLION.md).
# It reuses build/ and ccache, so an edit-compile cycle takes seconds instead
# of the minutes a Docker rebuild costs. build.ps1 remains the reproducible
# CI-equivalent path.
#
# Usage:
#   .\build-native.ps1                    build
#   .\build-native.ps1 -Port COM5         build, then write only the app partition
#                                         (keeps Wi-Fi, Thread and Matter data)
#   .\build-native.ps1 -Port COM5 -Full   also write the bootloader and partition table
#   .\build-native.ps1 -Port COM5 -Monitor  open the serial console after flashing
#   .\build-native.ps1 -Clean             discard build/ first (full rebuild)
param([string]$Port = "", [switch]$Full, [switch]$Monitor, [switch]$Clean)

$ErrorActionPreference = "Stop"
if (-not $env:IDF_PATH) {
    throw "IDF_PATH is not set. Install ESP-IDF as described in CLION.md."
}
. "$env:IDF_PATH\export.ps1" > $null
$env:IDF_CCACHE_ENABLE = "1"

# export.ps1 leaves the system Python first on PATH, and a bare `idf.py` can
# resolve to the .py file association, so name the virtualenv interpreter.
$python = Join-Path $env:IDF_PYTHON_ENV_PATH "Scripts\python.exe"
$idf = Join-Path $env:IDF_PATH "tools\idf.py"
if (-not (Test-Path $python)) {
    throw "ESP-IDF Python environment not found at $python. Run install.ps1 (see CLION.md)."
}

Push-Location $PSScriptRoot
try {
    if ($Clean) {
        & $python $idf fullclean
        if ($LASTEXITCODE -ne 0) { exit 1 }
    }
    & $python $idf build
    if ($LASTEXITCODE -ne 0) { exit 1 }

    if ($Port) {
        if ($Full) {
            & $python $idf -p $Port flash
        } else {
            & $python $idf -p $Port app-flash
        }
        if ($LASTEXITCODE -ne 0) { exit 1 }
        if ($Monitor) {
            & $python $idf -p $Port monitor
        }
    }
} finally {
    Pop-Location
}
