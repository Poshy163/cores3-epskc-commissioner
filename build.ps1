# Fast build: sources are copied into a Docker named volume (Linux-native FS)
# and compiled there with ccache; only the flash artifacts land back in dist/.
# A Windows bind mount makes every compile step cross Docker's file-sharing
# layer, which is what made builds take 10+ minutes.
#
# Usage:
#   .\build.ps1                 build
#   .\build.ps1 -Port COM5      build, then flash
#   .\build.ps1 -Debug          layer sdkconfig.debug on top of the defaults
param([string]$Port = "", [switch]$Debug)

$defaults = "sdkconfig.defaults"
if ($Debug) { $defaults = "sdkconfig.defaults;sdkconfig.debug" }

docker run --rm -e IDF_CCACHE_ENABLE=1 `
    -v epskc-ccache:/root/.cache/ccache `
    -v epskc-work-idf61:/work `
    -v "${PSScriptRoot}:/host" `
    espressif/idf:v6.1.1 bash /host/build.sh $defaults
if ($LASTEXITCODE -ne 0) { exit 1 }

if ($Port) {
    $toolPath = & python "$PSScriptRoot/tools/prepare_esptool.py" --target "$PSScriptRoot/build/host-tools" --standalone
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $callerPythonPath = $env:PYTHONPATH
    Push-Location -LiteralPath "$PSScriptRoot/dist"
    try {
        $env:PYTHONPATH = (@($toolPath, $callerPythonPath) | Where-Object { $_ }) -join [IO.Path]::PathSeparator
        python -m esptool --chip esp32s3 -p $Port -b 460800 --before default-reset --after hard-reset write-flash "@flash_args"
        $flashStatus = $LASTEXITCODE
    } finally {
        $env:PYTHONPATH = $callerPythonPath
        Pop-Location
    }
    exit $flashStatus
}
