# Fast local build with the ESP-IDF installed on this machine (see CLION.md).
# It reuses build/idf6/ and ccache, so an edit-compile cycle takes seconds instead
# of the minutes a Docker rebuild costs. build.ps1 remains the reproducible
# CI-equivalent path.
#
# Usage:
#   .\build-native.ps1                    build the H2 radio firmware (rcp/) and the app
#   .\build-native.ps1 -Port COM5         build, then write the app and the radio
#                                         firmware image (rcp_fw); keeps Wi-Fi,
#                                         Thread and Matter data
#   .\build-native.ps1 -Port COM5 -Full   also write the bootloader and partition table
#                                         (needed once when partitions.csv changes)
#   .\build-native.ps1 -Port COM5 -Monitor  open the serial console after flashing
#   .\build-native.ps1 -Clean             discard this build's output first
#   .\build-native.ps1 -BuildDir build/test  use a separate build directory
#   .\build-native.ps1 -IdfPath D:\esp-idf-v6.1.1  use another SDK installation
param([string]$Port = "", [switch]$Full, [switch]$Monitor, [switch]$Clean,
      [string]$BuildDir = "build/idf6", [string]$IdfPath = "")

$ErrorActionPreference = "Stop"
if (-not $IdfPath) {
    $standardIdfPath = "$env:SystemDrive\Espressif\frameworks\esp-idf-v6.1.1"
    $IdfPath = if (Test-Path -LiteralPath "$standardIdfPath\export.ps1") {
        $standardIdfPath
    } else {
        $env:IDF_PATH
    }
}
if (-not $IdfPath -or -not (Test-Path -LiteralPath "$IdfPath\export.ps1")) {
    throw "ESP-IDF v6.1.1 not found. Install it as described in CLION.md or pass -IdfPath."
}
$IdfPath = (Resolve-Path -LiteralPath $IdfPath).ProviderPath

# Activation changes only this process. Restore the caller's SDK and PATH so
# building this project does not switch the environment used by other repos.
$callerEnvironment = @{}
Get-ChildItem Env: | ForEach-Object { $callerEnvironment[$_.Name] = $_.Value }
try {
    $env:IDF_PATH = $IdfPath
    # A persistent virtualenv path from another SDK overrides automatic env
    # discovery. Let this SDK select its own installed Python environment.
    Remove-Item Env:\IDF_PYTHON_ENV_PATH -ErrorAction SilentlyContinue
    . "$IdfPath\export.ps1" > $null
    $env:IDF_CCACHE_ENABLE = "1"

    # A bare idf.py can resolve to Windows' .py file association.
    $python = Join-Path $env:IDF_PYTHON_ENV_PATH "Scripts\python.exe"
    $idf = Join-Path $env:IDF_PATH "tools\idf.py"
    if (-not (Test-Path -LiteralPath $python)) {
        throw "ESP-IDF Python environment not found at $python. Run install.ps1 (see CLION.md)."
    }
    $idfVersion = (& $python $idf --version | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $idfVersion -notmatch '(?m)^ESP-IDF v6\.1\.1(?:$|-)') {
        throw "This project requires ESP-IDF v6.1.1; activated SDK reports '$idfVersion'. See CLION.md."
    }

    Push-Location $PSScriptRoot
    try {
        $toolPath = & $python tools/prepare_esptool.py --target build/host-tools
        if ($LASTEXITCODE -ne 0) { throw "Could not prepare the project's esptool." }
        $env:PYTHONPATH = (@($toolPath, $env:PYTHONPATH) | Where-Object { $_ }) -join [IO.Path]::PathSeparator
        if ($Clean) {
            & $python $idf -B $BuildDir fullclean
            if ($LASTEXITCODE -ne 0) { exit 1 }
        }
        # The ESP32-H2 radio firmware first: the app packs it into rcp_fw. Its
        # version string's timestamp is pinned to rcp/'s last commit, so
        # rebuilding unchanged sources does not make the device reflash its
        # radio. CMake fixes the timestamp when it configures; the epoch as a
        # cache entry makes a new commit reconfigure this build (see
        # rcp/CMakeLists.txt). Only this build sees SOURCE_DATE_EPOCH.
        $rcpBuildDir = (Join-Path $PSScriptRoot "build/rcp") -replace '\\', '/'
        $rcpEpoch = (git log -1 --format=%ct -- rcp | Out-String).Trim()
        if (-not $rcpEpoch) { $rcpEpoch = (git log -1 --format=%ct | Out-String).Trim() }
        $rcpArgs = @("-C", "rcp", "-B", $rcpBuildDir, "-D", "SDKCONFIG=$rcpBuildDir/sdkconfig")
        if ($rcpEpoch) {
            $env:SOURCE_DATE_EPOCH = $rcpEpoch
            $rcpArgs += @("-D", "RCP_SOURCE_DATE_EPOCH=$rcpEpoch")
        }
        & $python $idf @rcpArgs build
        $rcpStatus = $LASTEXITCODE
        Remove-Item Env:\SOURCE_DATE_EPOCH -ErrorAction SilentlyContinue
        if ($rcpStatus -ne 0) { exit 1 }

        & $python $idf -B $BuildDir build
        if ($LASTEXITCODE -ne 0) { exit 1 }

        if ($Port) {
            if ($Full) {
                & $python $idf -B $BuildDir -p $Port flash
            } else {
                & $python $idf -B $BuildDir -p $Port app-flash rcp_fw-flash
            }
            if ($LASTEXITCODE -ne 0) { exit 1 }
            if ($Monitor) {
                & $python $idf -B $BuildDir -p $Port monitor
            }
        }
    } finally {
        Pop-Location
    }
} finally {
    foreach ($entry in (Get-ChildItem Env:)) {
        if (-not $callerEnvironment.ContainsKey($entry.Name)) {
            [Environment]::SetEnvironmentVariable($entry.Name, $null, "Process")
        }
    }
    foreach ($entry in $callerEnvironment.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, "Process")
    }
}
