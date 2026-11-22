#!/usr/bin/env bash
# Create a confirmed image for direct installation with the board's MCUboot.
set -euo pipefail
: "${MCUBOOT_SIGNING_KEY:?set MCUBOOT_SIGNING_KEY to the key trusted by the board}"
profile=${1:?usage: sign-arm.sh board path/to/nuttx.bin}
bin=${2:?missing path to nuttx.bin}
align=1
case "$profile" in
    nrf7002dk) slot=0x10000; header=0x200; size=0xec000; ram_end=0x20080000; align=4 ;;
    pico2w)       slot=0x10020000; header=0x200; size=0x170000; ram_end=0x20082000 ;;
    nucleo-f767zi|nucleo-f767zi-eth) slot=0x08040000; header=0x400; size=0xc0000; ram_end=0x20080000 ;;
    *) echo "Unknown Arm MCUboot profile: $profile" >&2; exit 1 ;;
esac
python3 - "$bin" "$slot" "$header" "$size" "$ram_end" <<'PY'
from pathlib import Path
import struct
import sys

slot, header, size, ram_end = (int(x, 0) for x in sys.argv[2:])
image = Path(sys.argv[1]).read_bytes()
if not 8 <= len(image) < size - header:
    sys.exit("image does not fit the primary slot")
stack, entry = struct.unpack_from("<II", image)
if not (0x20000000 < stack <= ram_end and entry & 1 and
        slot + header <= entry < slot + header + len(image)):
    sys.exit("image vectors do not match the application slot")
PY
options=(--key "$MCUBOOT_SIGNING_KEY" --version "${FW_VERSION:-0.9.7}" --header-size "$header"
    --pad-header --slot-size "$size" --align "$align" --security-counter 0 --custom-tlv 0xa0 "nuttx-$profile")
"${IMGTOOL:-imgtool}" sign "${options[@]}" "$bin" "${bin%.bin}.ota.bin"
"${IMGTOOL:-imgtool}" sign "${options[@]}" --pad --confirm "$bin" "${bin%.bin}.confirmed.bin"
