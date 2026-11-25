#!/usr/bin/env bash
# Runs inside espressif/idf. /host is the repo (Windows bind mount, slow);
# /work is a Docker named volume on Linux-native FS, where the build happens.
# Only the sources go in and only the flash artifacts come out, so the
# thousands of compile steps never touch the slow mount.
set -euo pipefail
DEFAULTS="${1:-sdkconfig.defaults}"

# The repo is bind-mounted with the host user's ownership.
git config --global --add safe.directory /host 2>/dev/null || true

rm -rf /work/main /work/rcp
cp -r /host/main /host/rcp /work/
cp /host/CMakeLists.txt /host/partitions.csv /host/sdkconfig.defaults /host/dependencies.lock /work/
v=$(git -C /host rev-parse --short=7 HEAD 2>/dev/null || echo unknown); git -C /host diff --quiet HEAD 2>/dev/null || v="${v}*"; echo "$v" > /work/version.txt
if [ -f /host/sdkconfig.debug ]; then
    cp /host/sdkconfig.debug /work/
else
    # Do not let an overlay left in the persistent volume affect a later build.
    rm -f /work/sdkconfig.debug
fi

cd /work
esptool_path=$(python /host/tools/prepare_esptool.py --target /work/host-tools)
export PYTHONPATH="$esptool_path${PYTHONPATH:+:$PYTHONPATH}"
# sdkconfig only takes defaults into account when it is first generated. Stamp
# both the selected files and their contents so editing sdkconfig.defaults (or
# the debug overlay) invalidates the persistent Docker-volume configuration.
IFS=';' read -r -a default_files <<< "$DEFAULTS"
for default_file in "${default_files[@]}"; do
    if [ ! -f "$default_file" ]; then
        echo "missing sdkconfig defaults file: $default_file" >&2
        exit 1
    fi
done
defaults_hash=$(sha256sum "${default_files[@]}" | sha256sum | cut -d ' ' -f 1)
defaults_stamp="$DEFAULTS $defaults_hash"
if [ "$(cat .defaults 2>/dev/null || true)" != "$defaults_stamp" ]; then
    echo "sdkconfig defaults changed; regenerating sdkconfig"
    rm -f sdkconfig
    printf '%s\n' "$defaults_stamp" > .defaults
fi

# The ESP32-H2 radio firmware first: the S3 image packs it into rcp_fw. Its
# version string carries this timestamp, pinned to rcp/'s last commit so a
# rebuild of unchanged sources does not make the device reflash its radio.
# The cache entry makes a new commit reconfigure the reused build directory,
# since CMake fixes the timestamp when it configures (see rcp/CMakeLists.txt).
rcp_epoch=$(git -C /host log -1 --format=%ct -- rcp 2>/dev/null || true)
[ -n "$rcp_epoch" ] || rcp_epoch=$(git -C /host log -1 --format=%ct 2>/dev/null || date +%s)
SOURCE_DATE_EPOCH="$rcp_epoch" IDF_TARGET=esp32h2 \
    idf.py -C /work/rcp -B /work/build-rcp -D SDKCONFIG=/work/build-rcp/sdkconfig \
        -D RCP_SOURCE_DATE_EPOCH="$rcp_epoch" build

idf.py -DSDKCONFIG_DEFAULTS="$DEFAULTS" -DRCP_BUILD_DIR=/work/build-rcp build

mkdir -p /host/dist/bootloader /host/dist/partition_table
cp build/flash_args /host/dist/
cp build/bootloader/bootloader.bin /host/dist/bootloader/
cp build/partition_table/partition-table.bin /host/dist/partition_table/
cp build/epskc_commissioner.bin /host/dist/
cp build/rcp_fw.bin /host/dist/
echo "artifacts copied to host dist/"
