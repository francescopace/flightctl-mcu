#!/usr/bin/env python3
"""Merged flash image of a secure build, bootloader included.

With Secure Boot, ESP-IDF leaves the bootloader out of flash_args and of
`idf.py merge-bin`, so that it is never flashed by accident. A factory image or
the emulator needs it: this writes <build dir>/merged-binary.bin with the
signed bootloader at its offset and everything flasher_args.json lists.

Usage: firmware/merge-secure.py [--extra-key KEY.pem ...] firmware/build-esp32c3-secure

--extra-key (up to two): more Secure Boot keys to sign the bootloader with.
The first boot trusts every key that signs the bootloader, in order (key 0
is the build's, then these), and revokes the slots left empty. Releases can
then move to the next key, which retires the previous one on the devices
(see docs/SECURITY.md, "Key rotation").

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
    args, extra = sys.argv[1:], []
    while args[:1] == ["--extra-key"] and len(args) > 1:
        extra.append(os.path.abspath(args[1]))
        args = args[2:]
    build = args[0] if len(args) == 1 and len(extra) <= 2 else sys.exit(__doc__)
    if sdkconfig(build, "SECURE_BOOT") != "y":
        sys.exit(f"{build} is not a Secure Boot build: use idf.py merge-bin")
    with open(os.path.join(build, "flasher_args.json")) as f:
        args = json.load(f)
    files = {int(k, 16): v for k, v in args["flash_files"].items()}
    bootloader = "bootloader/bootloader.bin"
    if extra:
        multi = "bootloader/bootloader-multikey.bin"
        for key in extra:
            # espsecure does not sign a file in place: through a second file
            signed = multi + ".next"
            subprocess.run([sys.executable, "-m", "espsecure", "sign-data", "--version", "2", "--append-signatures",
                            "--keyfile", key, "--output", signed, bootloader],
                           cwd=build, check=True, stdout=subprocess.DEVNULL)
            os.replace(os.path.join(build, signed), os.path.join(build, multi))
            bootloader = multi
    files[int(sdkconfig(build, "BOOTLOADER_OFFSET_IN_FLASH"), 16)] = bootloader
    # "keep": rewriting the flash settings in an image header would change
    # the signed bytes, and the ROM would refuse the bootloader.
    cmd = [sys.executable, "-m", "esptool", "--chip", args["extra_esptool_args"]["chip"], "merge-bin",
           "-o", "merged-binary.bin", "--flash-mode", "keep", "--flash-size", "keep", "--flash-freq", "keep"]
    for off in sorted(files):
        cmd += [hex(off), files[off]]
    subprocess.run(cmd, cwd=build, check=True)


if __name__ == "__main__":
    main()
