#!/usr/bin/env bash
# Runs inside espressif/idf. /host is the repo (Windows bind mount, slow);
# /work is a Docker named volume on Linux-native FS, where the build happens.
# Only the sources go in and only the flash artifacts come out, so the
# thousands of compile steps never touch the slow mount.
set -e
DEFAULTS="${1:-sdkconfig.defaults}"

rm -rf /work/main
cp -r /host/main /work/
cp /host/CMakeLists.txt /host/partitions.csv /host/sdkconfig.defaults /host/dependencies.lock /work/
v=$(git -C /host rev-parse --short=7 HEAD 2>/dev/null || echo unknown); git -C /host diff --quiet 2>/dev/null || v="${v}*"; echo "$v" > /work/version.txt
if [ -f /host/sdkconfig.debug ]; then cp /host/sdkconfig.debug /work/; fi

cd /work
# sdkconfig only regenerates from the defaults when absent, so force that
# whenever the defaults selection changes (e.g. -Debug toggled).
if [ "$(cat .defaults 2>/dev/null)" != "$DEFAULTS" ]; then
    rm -f sdkconfig
    echo "$DEFAULTS" > .defaults
fi

idf.py -DSDKCONFIG_DEFAULTS="$DEFAULTS" build

mkdir -p /host/dist/bootloader /host/dist/partition_table
cp build/flash_args /host/dist/
cp build/bootloader/bootloader.bin /host/dist/bootloader/
cp build/partition_table/partition-table.bin /host/dist/partition_table/
cp build/epskc_commissioner.bin /host/dist/
echo "artifacts copied to host dist/"
