#!/usr/bin/env bash
# Called by build.sh in the pinned NuttX workspace.
set -euo pipefail
port="$FC_ROOT/port/nuttx"
work="$PWD"
header=cyw43439.h
if [[ ! -f "$header" ]]; then
    curl -fLsS https://raw.githubusercontent.com/georgerobotics/cyw43-driver/055d64274b014dd7b1c2fc94d26e8a18face7124/firmware/w43439A0_7_95_49_00_combined.h -o "$header"
fi
echo "120bb63db1c5cf42cd44cf9a057d6f14601038a9413badf5a3b737f6552bc9ea  $header" | sha256sum -c -
python3 - <<'PY'
from pathlib import Path
import re
array = Path("cyw43439.h").read_text().split("{", 1)[1].split("}", 1)[0]
Path("cyw43439.bin").write_bytes(bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})\b", array)))
PY
apply_patch() {
    local tree=$1 patch=$2
    shift 2
    if ! git -C "$tree" apply "$@" --reverse --check "$patch" 2>/dev/null; then
        git -C "$tree" apply "$@" "$patch"
    fi
}
apply_patch apps "$port/pico2w-ntp.patch"
apply_patch nuttx "$port/pico2w.patch"
apply_patch nuttx "$port/pico2w-ota-flash.patch" --include='boards/*'
apply_patch nuttx "$port/pico2w-ota-flash.patch" --include='arch/*'
apply_patch nuttx "$port/hardware-drivers.patch" --include=mm/mm_heap/mm_mallinfo.c
mkdir -p apps/flightctl
cp "$port"/{Makefile,Make.defs,Kconfig} apps/flightctl/
cd nuttx
profiles=boards/arm/rp23xx/raspberrypi-pico-2-w/configs
mkdir -p "$profiles/flightctl"
awk '/^CONFIG_|^# CONFIG_/ { k=$0; sub(/^# /,"",k); sub(/[= ].*/,"",k); v[k]=$0 } END { for (k in v) print v[k] }' \
    "$profiles/telnet/defconfig" "$port/config.common" "$port/config.pico2w" | sort > "$profiles/flightctl/defconfig"
echo "CONFIG_CYW43439_FIRMWARE_BIN_PATH=\"$work/cyw43439.bin\"" >> "$profiles/flightctl/defconfig"
if ! cmp -s "$profiles/flightctl/defconfig" .fc-defconfig; then
    [[ ! -f .config ]] || make distclean
    tools/configure.sh -l raspberrypi-pico-2-w:flightctl
    cp "$profiles/flightctl/defconfig" .fc-defconfig
fi
make -j"${JOBS:-6}" EXTRAFLAGS=-std=gnu17
