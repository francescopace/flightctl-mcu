#!/usr/bin/env bash
# Run in a Linux shell with the target Xtensa or RISC-V toolchain on PATH.
set -euo pipefail
export FC_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
profile="${BOARD:-esp32s3-n4r2}"
chip=esp32s3
arch=xtensa
board_name=esp32s3-devkit
overrides=("$FC_ROOT/port/nuttx/config.common")
[[ "$profile" != esp32s3-* ]] || overrides+=("$FC_ROOT/port/nuttx/config")
case "$profile" in
    nrf7002dk|nrf7002dk-net) chip=nrf53; arch=arm; board_name=nrf5340-dk ;;
    nucleo-f767zi|nucleo-f767zi-eth) chip=stm32f7; arch=arm; board_name=nucleo-f767zi ;;
    pico2w) chip=rp23xx; arch=arm; board_name=raspberrypi-pico-2-w ;;
    esp32s3-n4r2) ;;
    esp32s3-n8r8) overrides+=("$FC_ROOT/port/nuttx/config.n8r8") ;;
    esp32s3-n16r8) overrides+=("$FC_ROOT/port/nuttx/config.n16r8") ;;
    esp32-devkitc|esp32-devkitc-rev1)
        chip=esp32; board_name=esp32-devkitc; overrides+=("$FC_ROOT/port/nuttx/config.esp32")
        [[ "$profile" != esp32-devkitc-rev1 ]] || overrides+=("$FC_ROOT/port/nuttx/config.esp32-rev1")
        ;;
    esp32c3-devkit|esp32c5-devkitc|esp32c6-devkitc)
        chip="${profile%%-*}"; arch=risc-v; board_name="$profile"; overrides+=("$FC_ROOT/port/nuttx/config.riscv")
        [[ "$chip" != esp32c3 && "$chip" != esp32c5 ]] || overrides+=("$FC_ROOT/port/nuttx/config.esp32c3")
        ;;
    *) echo "Unknown NuttX board: $profile" >&2; exit 1 ;;
esac
if [[ "${OTA:-0}" == 1 ]]; then
    [[ "$chip" == esp32s3 ]] || { echo "$profile uses Simple Boot" >&2; exit 1; }
    overrides+=("$FC_ROOT/port/nuttx/config.ota")
    if [[ "$profile" == esp32s3-n4r2 ]]; then
        overrides+=("$FC_ROOT/port/nuttx/config.ota-n4")
    fi
fi
if [[ "$arch" == risc-v && -z "${CROSSDEV:-}" ]] && command -v riscv32-esp-elf-gcc >/dev/null; then
    export CROSSDEV=riscv32-esp-elf-
fi
default_work="$FC_ROOT/build/nuttx"
[[ "$chip" == esp32s3 ]] || default_work="$default_work-$chip"
[[ "$profile" != nrf7002dk-net ]] || default_work+="-net"
work="${NUTTX_WORK:-$default_work}"
mkdir -p "$work"
cd "$work"
work="$PWD"
for repo in nuttx apps; do
    url=nuttx
    [[ "$repo" != apps ]] || url=nuttx-apps
    if [[ "$profile" == pico2w ]]; then
        revision=d50b768840c413e0172039d2f3ae582dce7d47c1
        [[ "$repo" != apps ]] || revision=7196658dffde6638403f1c821349776f183b44a4
        if [[ ! -d "$repo/.git" ]]; then
            git init -q "$repo"
            git -C "$repo" fetch --depth=1 "https://github.com/apache/$url.git" "$revision"
            git -C "$repo" checkout -q --detach "$revision"
        fi
        [[ "$(git -C "$repo" rev-parse HEAD)" == "$revision" ]]
    else
        [[ -d "$repo" ]] || git clone --branch nuttx-13.1.0 --depth 1 "https://github.com/apache/$url.git" "$repo"
        [[ "$(git -C "$repo" rev-parse HEAD)" == "$(git -C "$repo" rev-parse nuttx-13.1.0^{commit})" ]]
    fi
done
[[ -f "$FC_ROOT/third_party/wamr/build-scripts/runtime_lib.cmake" ]] || "$FC_ROOT/third_party/fetch-wamr.sh"
export FC_MBEDTLS="$work/mbedtls-4.1.1"
if [[ ! -d "$FC_MBEDTLS" ]]; then
    curl -fLsS https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-4.1.1/mbedtls-4.1.1.tar.bz2 -o mbedtls.tar.bz2
    echo '3359a349e23db3d5536fcee032ae7b2ecbfc08972fab643089b5cbf2a375c98c  mbedtls.tar.bz2' | sha256sum -c -
    tar -xjf mbedtls.tar.bz2
fi
if [[ "$profile" == pico2w ]]; then
    exec "$FC_ROOT/port/nuttx/build-pico2w.sh"
fi
patch="$FC_ROOT/port/nuttx/ntp-after-dhcp.patch"
if ! git -C apps apply --reverse --check "$patch" 2>/dev/null; then
    git -C apps apply "$patch"
fi
mkdir -p apps/flightctl
cp "$FC_ROOT/port/nuttx/"{Makefile,Make.defs,Kconfig} apps/flightctl/
cd nuttx
patch="$FC_ROOT/port/nuttx/hardware-drivers.patch"
parts=()
if [[ "$chip" != esp32s3 ]]; then
    parts=(--include=sched/sched/sched_cpuload.c --include=mm/mm_heap/mm_mallinfo.c)
fi
if ! git apply "${parts[@]}" --reverse --check "$patch" 2>/dev/null; then
    git apply "${parts[@]}" "$patch"
fi
case "$profile" in
    nrf7002dk) exec "$FC_ROOT/port/nuttx/nrf7002dk/build.sh" app ;;
    nrf7002dk-net) exec "$FC_ROOT/port/nuttx/nrf7002dk/build.sh" net ;;
esac
if [[ "$chip" == stm32f7 ]]; then
    exec "$FC_ROOT/port/nuttx/build-nucleo.sh" "$profile"
fi
if [[ "$chip" == esp32c5 ]]; then
    export ESP_HAL_3RDPARTY_VERSION=b90b1837cb5ad24747deb4c895246037cc206ce5
    python3 "$FC_ROOT/port/nuttx/c5/prepare.py" kernel "$PWD"
    overrides+=("$FC_ROOT/port/nuttx/c5/config")
fi
board="boards/$arch/$chip/$board_name/configs"
mkdir -p "$board/flightctl"
# Merge the board's Wi-Fi defaults and our overrides before resolving dependencies.
awk '/^CONFIG_|^# CONFIG_/ { k=$0; sub(/^# /,"",k); sub(/[= ].*/,"",k); v[k]=$0 } END { for (k in v) print v[k] }' \
    "$board/wifi/defconfig" "${overrides[@]}" | sort > "$board/flightctl/defconfig"
if ! cmp -s "$board/flightctl/defconfig" .fc-defconfig; then
    if [[ -f .fc-defconfig && -f .config ]]; then
        # The Espressif HAL objects do not track changes to the PSRAM mode.
        make clean
        # Preserve upstream's generated host and application path settings.
        sed -n '/^CONFIG_HOST_\|^CONFIG_APPS_DIR=/p' .config > .fc-host-config
        cat "$board/flightctl/defconfig" .fc-host-config > .config
        make olddefconfig
    else
        tools/configure.sh -l "$board_name:flightctl"
    fi
    cp "$board/flightctl/defconfig" .fc-defconfig
fi
init_dir="boards/$arch/$chip/common/src/etc/init.d"
if [[ "$chip" == esp32s3 ]]; then
    install -m 644 "$FC_ROOT/port/nuttx/init.rc" "$init_dir/init.rc"
elif ! grep -Fxq 'flightctl &' "$init_dir/rcS"; then
    echo 'flightctl &' >> "$init_dir/rcS"
fi
# The Wi-Fi HAL defaults to libc calloc with the NuttX configuration.
# Match its deallocator before compiling the HAL in the configured tree.
make context
hal="arch/$arch/src/$chip/esp-hal-3rdparty"
patch="$FC_ROOT/port/nuttx/wifi-allocator.patch"
if ! git -C "$hal" apply --reverse --check "$patch" 2>/dev/null; then
    git -C "$hal" apply "$patch"
fi
if [[ "$chip" == esp32 ]]; then
    # The NuttX timer callback calls the scheduler from flash. Mask its IRQ during flash writes.
    patch="$FC_ROOT/port/nuttx/esp32-timer-flash.patch"
    if ! git -C "$hal" apply --reverse --check "$patch" 2>/dev/null; then
        git -C "$hal" apply "$patch"
    fi
fi
make -j"${JOBS:-6}" EXTRAFLAGS=-std=gnu17
if [[ "${OTA:-0}" == 1 ]]; then
    patch="$FC_ROOT/port/nuttx/mcuboot-build.patch"
    if ! git apply --reverse --check "$patch" 2>/dev/null; then
        git apply "$patch"
    fi
    make bootloader
    slot=$(sed -n 's/^CONFIG_ESP32S3_OTA_SLOT_SIZE=//p' .config)
    header=$(sed -n 's/^CONFIG_ESP32S3_APP_MCUBOOT_HEADER_SIZE=//p' .config)
    args=(--align 4 -v "${FW_VERSION:-0.9.7}" -H "$header" --pad-header -S "$slot" --custom-tlv 0xa0 "nuttx-$profile")
    # Only the factory installation carries a confirmed trailer. The OTA
    # stream ends after its TLVs; the running agent requests a trial boot.
    imgtool sign "${args[@]}" --pad --confirm nuttx.hex nuttx.bin
    imgtool sign "${args[@]}" nuttx.hex nuttx-ota.bin
    exit 0
fi
if [[ "$chip" == esp32c5 ]]; then
    python3 "$FC_ROOT/port/nuttx/c5/image.py" nuttx.bin
fi
size="$(stat -c %s nuttx.bin)"
app_offset=0
[[ "$chip" != esp32 ]] || app_offset=0x1000
[[ "$chip" != esp32c5 ]] || app_offset=0x2000
(( app_offset + size <= 0x2fc000 )) || { echo "Firmware overlaps the factory partition" >&2; exit 1; }
