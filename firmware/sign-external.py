#!/usr/bin/env python3
"""Secure Boot signatures made outside this machine: by an HSM, a signing
service, or anything that turns a SHA-256 digest into an RSA-PSS signature.

    firmware/sign-external.py BUILD --pub-key PUB.pem --sign-cmd 'COMMAND {digest} {signature}' [--bootloader-only]

BUILD is a secure build made without the private key
(SECURE_BOOT_SIGNER=external firmware/build.sh: its images are padded but not
signed). For the bootloader and the app, this writes the image's SHA-256
digest to a file, runs COMMAND with {digest} and {signature} replaced by file
paths, and appends the signature block (espsecure's sign_data with
the public key and the signature, which checks one against the other first). The images are
signed in place; merge-secure.py and fw-push.py then work as usual.

COMMAND must produce an RSA-PSS signature over the digest: SHA-256, salt
length 32. With a key file, for a dry run:

    openssl pkeyutl -sign -in {digest} -inkey key.pem -out {signature} -pkeyopt digest:sha256 -pkeyopt rsa_padding_mode:pss -pkeyopt rsa_pss_saltlen:32

with a PKCS#11 HSM, the same through its engine or provider (-engine pkcs11
-keyform engine -inkey 'pkcs11:object=...'), or pkcs11-tool -m RSA-PKCS-PSS.

Key rotation (docs/SECURITY.md): run it again with --bootloader-only for each
spare key, which appends further signatures to the bootloader.
"""
import argparse
import hashlib
import os
import shlex
import shutil
import subprocess
import sys
import tempfile

import espsecure


def sign(image, pub_key, sign_cmd, append):
    with open(image, "rb") as f:
        data = f.read()
    if append is False and data[-0x1000:-0x1000 + 1] == b"\xe7":
        sys.exit(f"{image} is already signed: build without a key (SECURE_BOOT_SIGNER=external)")
    with tempfile.TemporaryDirectory() as tmp:
        digest, signature = os.path.join(tmp, "digest"), os.path.join(tmp, "signature")
        # The digest covers the image as built (secure-padded), without the
        # 4 KB signature sector if another key signed it already.
        body = data[:-0x1000] if append and data[-0x1000] == 0xE7 else data
        with open(digest, "wb") as f:
            f.write(hashlib.sha256(body).digest())
        cmd = sign_cmd.replace("{digest}", shlex.quote(digest)).replace("{signature}", shlex.quote(signature))
        subprocess.run(cmd, shell=True, check=True)
        # espsecure's API: its command line wants a private key file
        # even with a pre-calculated signature. It checks the signature
        # against the public key before it appends the block.
        out = os.path.join(tmp, "signed.bin")
        with open(pub_key, "rb") as pk, open(signature, "rb") as sg, open(image, "rb") as df:
            espsecure.sign_data("2", [], out, append, False, None, [pk], [sg], df)
        shutil.copyfile(out, image)
    print(f"signed {image}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("build")
    p.add_argument("--pub-key", required=True)
    p.add_argument("--sign-cmd", required=True)
    p.add_argument("--bootloader-only", action="store_true", help="append a further signature to the bootloader")
    a = p.parse_args()
    if "{digest}" not in a.sign_cmd or "{signature}" not in a.sign_cmd:
        p.error("--sign-cmd needs {digest} and {signature}")
    sign(os.path.join(a.build, "bootloader", "bootloader.bin"), a.pub_key, a.sign_cmd, a.bootloader_only)
    if not a.bootloader_only:
        sign(os.path.join(a.build, "flightctl_mcu_fw.bin"), a.pub_key, a.sign_cmd, False)


if __name__ == "__main__":
    main()
