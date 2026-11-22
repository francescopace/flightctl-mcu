#!/usr/bin/env bash
# Called from the prepared NuttX 13.1.0 kernel checkout, once per core.
set -euo pipefail
core=${1:?select app or net}
port="$FC_ROOT/port/nuttx"
here="$port/nrf7002dk"
board=boards/arm/nrf53/nrf5340-dk
profiles="$board/configs"
apply_patch() {
    if ! git apply --reverse --check "$1" 2>/dev/null; then git apply "$1"; fi
}
apply_patch "$here/board.patch"
cp "$here/$core.c" "$board/src/fc_nrf_board.c"
configs=("$profiles/rptun_cpu$core/defconfig")
if [[ "$core" == app ]]; then
    configs+=("$port/config.common")
    apply_patch "$port/slip-tx-bounds.patch"
    apply_patch "$port/slip-rx.patch"
    apply_patch "$here/uart-tx.patch"
fi
configs+=("$here/config.$core")
mkdir -p "$profiles/flightctl"
awk '/^CONFIG_|^# CONFIG_/ { k=$0; sub(/^# /,"",k); sub(/[= ].*/,"",k); v[k]=$0 } END { for (k in v) print v[k] }' \
    "${configs[@]}" | sort > "$profiles/flightctl/defconfig"
if ! cmp -s "$profiles/flightctl/defconfig" .fc-defconfig; then
    [[ ! -f .config ]] || make distclean
    tools/configure.sh -l nrf5340-dk:flightctl
    cp "$profiles/flightctl/defconfig" .fc-defconfig
fi
grep -qx CONFIG_DEV_URANDOM_ARCH=y .config
make -j"${JOBS:-6}" EXTRAFLAGS=-std=gnu17
