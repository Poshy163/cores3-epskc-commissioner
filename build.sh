#!/usr/bin/env bash
# Runs inside espressif/idf. /host is the repo (Windows bind mount, slow);
# /work is a Docker named volume on Linux-native FS, where the build happens.
# Only the sources go in and only the flash artifacts come out, so the
# thousands of compile steps never touch the slow mount.
set -euo pipefail
DEFAULTS="${1:-sdkconfig.defaults}"

rm -rf /work/main
cp -r /host/main /work/
cp /host/CMakeLists.txt /host/partitions.csv /host/sdkconfig.defaults /host/dependencies.lock /work/
v=$(git -C /host rev-parse --short=7 HEAD 2>/dev/null || echo unknown); git -C /host diff --quiet 2>/dev/null || v="${v}*"; echo "$v" > /work/version.txt
if [ -f /host/sdkconfig.debug ]; then
    cp /host/sdkconfig.debug /work/
else
    # Do not let an overlay left in the persistent volume affect a later build.
    rm -f /work/sdkconfig.debug
fi

cd /work
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

idf.py -DSDKCONFIG_DEFAULTS="$DEFAULTS" build

mkdir -p /host/dist/bootloader /host/dist/partition_table
cp build/flash_args /host/dist/
cp build/bootloader/bootloader.bin /host/dist/bootloader/
cp build/partition_table/partition-table.bin /host/dist/partition_table/
cp build/epskc_commissioner.bin /host/dist/
echo "artifacts copied to host dist/"
