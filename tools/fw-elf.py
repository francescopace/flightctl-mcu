#!/usr/bin/env python3
"""Gets back the ELF of a firmware release, to decode a device's lastFatalError.

    tools/fw-elf.py <registry>/flightctl-mcu/fw:<tag> <platform> [out.elf]
    tools/fw-elf.py <registry>/flightctl-mcu/fw@sha256:<index digest> <platform> [out.elf]

tools/fw-push.py pushes each image's ELF, gzipped, next to the release: an
artifact whose subject is the release's index, tagged sha256-<index digest>.elf.
The platform is the one in the index (esp32c3, esp32s3-r8, esp32s3_devkitc for
Zephyr...); the digest is what the device reports as status.os.imageDigest.
When lastFatalError ends with ", firmware <version>", the crash was another
release's (a rollback came after it): take that release's ELF.

The ELF goes to build/elf/<repo>-<tag or digest>-<platform>.elf, or to out.elf,
and its path is printed. Then, with the chip's addr2line (ESP-IDF's toolchain,
the Zephyr SDK's):

    xtensa-esp32s3-elf-addr2line -pfiaC -e <elf> <pc> <addresses...>
"""
import gzip
import hashlib
import json
import os
import sys
import urllib.error

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "registry"))
import registry_client  # noqa: E402

INDEX_TYPE = "application/vnd.oci.image.index.v1+json"
MANIFEST_TYPE = "application/vnd.oci.image.manifest.v1+json"
PLATFORM_KEY = "io.flightctl-mcu.platform"


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    ref, platform = sys.argv[1], sys.argv[2]
    host, rest = ref.split("/", 1)
    if "@" in rest:
        repo, version = rest.split("@", 1)
    else:
        repo, version = rest.rsplit(":", 1)
    base = registry_client.base_url(host)
    try:
        _, _, body = registry_client.request("GET", f"{base}/v2/{repo}/manifests/{version}",
                                             headers={"Accept": INDEX_TYPE})
        index = "sha256:" + hashlib.sha256(body).hexdigest()
        _, _, body = registry_client.request("GET", f"{base}/v2/{repo}/manifests/{index.replace(':', '-')}.elf",
                                             headers={"Accept": MANIFEST_TYPE})
    except urllib.error.HTTPError as e:
        if e.code == 404:
            sys.exit(f"{ref}: no such release, or no ELF pushed with it")
        raise
    layers = json.loads(body)["layers"]
    layer = next((l for l in layers if l.get("annotations", {}).get(PLATFORM_KEY) == platform), None)
    if layer is None:
        have = ", ".join(l.get("annotations", {}).get(PLATFORM_KEY, "?") for l in layers)
        sys.exit(f"{ref}: no ELF for {platform} (there are: {have})")
    data = registry_client.request("GET", f"{base}/v2/{repo}/blobs/{layer['digest']}")[2]
    if "sha256:" + hashlib.sha256(data).hexdigest() != layer["digest"]:
        sys.exit(f"{ref}: the ELF for {platform} does not match its digest")
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = sys.argv[3] if len(sys.argv) == 4 else os.path.join(
        root, "build", "elf", f"{repo.rsplit('/', 1)[-1]}-{version.replace(':', '-')}-{platform}.elf")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as f:
        f.write(gzip.decompress(data))
    print(out)


if __name__ == "__main__":
    main()
