#!/usr/bin/env bash
# Called after build.sh has prepared the NuttX 13.1.0 kernel and apps.
set -euo pipefail
port="$FC_ROOT/port/nuttx"
profiles=boards/arm/stm32f7/nucleo-f767zi/configs
for patch in "$port/nucleo-slot.patch" "$port/stm32f7-flash-cache.patch" \
             "$port/slip-tx-bounds.patch" "$port/slip-rx.patch" "$port/nucleo-temperature.patch"; do
    if ! git apply --reverse --check "$patch" 2>/dev/null; then
        git apply "$patch"
    fi
done
overrides=("$port/config.common" "$port/config.nucleo")
if [[ "${1:-nucleo-f767zi}" == nucleo-f767zi-eth ]]; then
    overrides+=("$port/config.nucleo-eth")
fi
mkdir -p "$profiles/flightctl"
awk '/^CONFIG_|^# CONFIG_/ { k=$0; sub(/^# /,"",k); sub(/[= ].*/,"",k); v[k]=$0 } END { for (k in v) print v[k] }' \
    "$profiles/nsh/defconfig" "${overrides[@]}" | sort > "$profiles/flightctl/defconfig"
if ! cmp -s "$profiles/flightctl/defconfig" .fc-defconfig; then
    [[ ! -f .config ]] || make distclean
    tools/configure.sh -l nucleo-f767zi:flightctl
    cp "$profiles/flightctl/defconfig" .fc-defconfig
fi
make -j"${JOBS:-6}" EXTRAFLAGS=-std=gnu17
