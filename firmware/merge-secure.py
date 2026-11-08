#!/usr/bin/env python3
"""Merged flash image of a secure build, bootloader included.

With Secure Boot, ESP-IDF leaves the bootloader out of flash_args and of
`idf.py merge-bin`, so that it is never flashed by accident. A factory image or
the emulator needs it: this writes <build dir>/merged-binary.bin with the
signed bootloader at its offset and everything flasher_args.json lists.

Usage: firmware/merge-secure.py firmware/build-esp32c3-secure

Flashing such an image to a real chip and booting it burns that chip's eFuses
for good (see docs/SECURITY.md).
"""
import json
import os
import re
import subprocess
import sys


def sdkconfig(build, key):
    with open(os.path.join(build, "sdkconfig")) as f:
        m = re.search(rf"^CONFIG_{key}=(.*)$", f.read(), re.M)
    return m.group(1).strip('"') if m else None


def main():
    build = sys.argv[1] if len(sys.argv) > 1 else sys.exit(__doc__)
    if sdkconfig(build, "SECURE_BOOT") != "y":
        sys.exit(f"{build} is not a Secure Boot build: use idf.py merge-bin")
    with open(os.path.join(build, "flasher_args.json")) as f:
        args = json.load(f)
    files = {int(k, 16): v for k, v in args["flash_files"].items()}
    files[int(sdkconfig(build, "BOOTLOADER_OFFSET_IN_FLASH"), 16)] = "bootloader/bootloader.bin"
    # "keep": rewriting the flash settings in an image header would change
    # the signed bytes, and the ROM would refuse the bootloader.
    cmd = [sys.executable, "-m", "esptool", "--chip", args["extra_esptool_args"]["chip"], "merge_bin",
           "-o", "merged-binary.bin", "--flash_mode", "keep", "--flash_size", "keep", "--flash_freq", "keep"]
    for off in sorted(files):
        cmd += [hex(off), files[off]]
    subprocess.run(cmd, cwd=build, check=True)


if __name__ == "__main__":
    main()
