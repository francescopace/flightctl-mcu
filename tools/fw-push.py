#!/usr/bin/env python3
"""Pushes the firmware builds as one multi-chip OCI artifact.

    tools/fw-push.py [--variant NAME] [--delta-from TAG ...] <registry>/flightctl-mcu/fw:v2 [esp32s3 esp32c3 ...]
    tools/fw-push.py ... <registry>/flightctl-mcu/fw:z3 esp32s3_devkitc=port/zephyr/build-esp32s3/zephyr/zephyr/zephyr.signed.bin

Layout (OCI 1.1; the registry reached as tools/registry/registry_client.py says;
stdlib only):
  index (tag)      artifactType application/vnd.flightctl-mcu.firmware.v1
    manifest       platform {os: esp-idf, architecture: <chip>}
      layer        application/vnd.espressif.app-image  (build-<chip>/flightctl_mcu_fw.bin)
      layer ...    application/vnd.flightctl-mcu.firmware.delta.v1+detools  (one per --delta-from)

The device picks the manifest whose platform.architecture is its chip, streams
the layer into the inactive OTA slot and checks its sha256.
Default chips: every port/espidf/build-<chip>[-<variant>]/ with a built image,
and build-esp32s3-r8[-<variant>] (octal PSRAM) as platform esp32s3-r8.
PLATFORM=FILE instead of a chip pushes FILE for that platform: a Zephyr build
signed for MCUboot (zephyr.signed.bin), for the board whose
CONFIG_FC_FW_PLATFORM is PLATFORM. Its manifest says os "zephyr" and its layer
application/vnd.zephyr.mcuboot-image. For NuttX pass nuttx-esp32s3-<module>=nuttx-ota.bin or nuttx-<arm-board>=nuttx.ota.bin.

--delta-from TAG (a tag or digest in the same repository, repeatable): also
push, for each chip, a delta from that release's image to this one. A device
running that image downloads the delta instead and rebuilds the new image
from its own (docs/DELTA-UPDATES.md); the others, and older firmware, take the whole
image. A delta is a detools patch (sequential, heatshrink): this needs the
detools package, here or in a virtual environment made in build/ on first use.

The release's SBOM (tools/sbom.py, CycloneDX) goes to build/sbom/ and to the
registry, as an artifact whose subject is the index; to Trustify too when
TRUSTIFY_URL is set (docs/SBOM.md).

The ELF of each image (build-<chip>/flightctl_mcu_fw.elf, zephyr.elf next to
a Zephyr image), what a device's lastFatalError is decoded against with
addr2line, goes to the registry too, gzipped, in an artifact whose subject is
the index, tagged sha256-<index digest>.elf: tools/fw-elf.py gets it back.
Devices never pull it. A build without its ELF is pushed all the same.

The index is then signed with tools/signing/sign.py when a signing key is
there ($SIGNING_KEY or .secrets/signing.key); NO_SIGN=1 skips it.
"""
import glob
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import urllib.request

import sbom

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "registry"))
import registry_client  # noqa: E402

ARTIFACT_TYPE = "application/vnd.flightctl-mcu.firmware.v1"
LAYER_TYPE = "application/vnd.espressif.app-image"
ZEPHYR_LAYER_TYPE = "application/vnd.zephyr.mcuboot-image"
DELTA_TYPE = "application/vnd.flightctl-mcu.firmware.delta.v1+detools"
DELTA_FROM = "io.flightctl-mcu.delta.from"
DELTA_FROM_SIZE = "io.flightctl-mcu.delta.from-size"
DETOOLS_VERSION = "0.53.1"  # makes the deltas; the device decodes their format (core/src/fc_delta.c)
# What the device's decoder is built for (core/src/fc_delta.h): a 4 KB window.
HEATSHRINK_WINDOW, HEATSHRINK_LOOKAHEAD = 12, 8
EMPTY = b"{}"
EMPTY_TYPE = "application/vnd.oci.empty.v1+json"
SBOM_TYPE = "application/vnd.cyclonedx+json"
ELF_ARTIFACT_TYPE = "application/vnd.flightctl-mcu.firmware.elf.v1"
ELF_TYPE = "application/vnd.flightctl-mcu.firmware.elf.v1+gzip"
PLATFORM_KEY = "io.flightctl-mcu.platform"
IMAGE_KEY = "io.flightctl-mcu.image"  # the digest of the image layer the ELF was built with
MANIFEST_TYPE = "application/vnd.oci.image.manifest.v1+json"
INDEX_TYPE = "application/vnd.oci.image.index.v1+json"


def sha(data):
    return "sha256:" + hashlib.sha256(data).hexdigest()


def request(method, url, data=None, headers=None):
    return registry_client.request(method, url, data, headers)


def push_blob(base, repo, data):
    digest = sha(data)
    try:
        request("HEAD", f"{base}/v2/{repo}/blobs/{digest}")
        return digest  # already there
    except urllib.error.HTTPError as e:
        if e.code != 404:
            raise
    _, headers, _ = request("POST", f"{base}/v2/{repo}/blobs/uploads/")
    loc = headers["Location"]
    if loc.startswith("/"):
        loc = base + loc
    sep = "&" if "?" in loc else "?"
    request("PUT", f"{loc}{sep}digest={digest}", data,
            {"Content-Type": "application/octet-stream", "Content-Length": str(len(data))})
    return digest


def push_manifest(base, repo, ref, media_type, obj):
    """Pushes obj under ref (a tag), or under its own digest if ref is None."""
    body = json.dumps(obj, separators=(",", ":")).encode()
    request("PUT", f"{base}/v2/{repo}/manifests/{ref or sha(body)}", body, {"Content-Type": media_type})
    return sha(body), len(body)


def pull_image(base, repo, ref, chip):
    """The firmware image for chip under ref (its manifest's first layer), or None."""
    accept = {"Accept": f"{INDEX_TYPE}, {MANIFEST_TYPE}"}
    try:
        top = json.loads(request("GET", f"{base}/v2/{repo}/manifests/{ref}", headers=accept)[2])
        if top.get("mediaType") == INDEX_TYPE:
            digest = next((m["digest"] for m in top["manifests"]
                           if m.get("platform", {}).get("architecture") == chip), None)
            if digest is None:
                return None
            top = json.loads(request("GET", f"{base}/v2/{repo}/manifests/{digest}", headers=accept)[2])
        return request("GET", f"{base}/v2/{repo}/blobs/{top['layers'][0]['digest']}")[2]
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise


def detools_python(root):
    """A python that has detools DETOOLS_VERSION: this one, or build/detools-venv, made or
    updated on first use. Another version could make deltas the device reads differently."""
    check = f"import detools.version as v, sys; sys.exit(v.__version__ != {DETOOLS_VERSION!r})"
    if subprocess.run([sys.executable, "-c", check], capture_output=True).returncode == 0:
        return sys.executable
    venv = f"{root}/build/detools-venv"
    py = f"{venv}/bin/python"
    if not os.path.exists(py) or subprocess.run([py, "-c", check], capture_output=True).returncode != 0:
        print(f"installing detools {DETOOLS_VERSION} in {venv}")
        subprocess.run([sys.executable, "-m", "venv", venv], check=True)
        subprocess.run([py, "-m", "pip", "install", "-q", f"detools=={DETOOLS_VERSION}"], check=True)
    return py


def make_delta(py, old, new):
    """The detools patch that turns old into new."""
    with tempfile.TemporaryDirectory() as d:
        for name, data in (("old", old), ("new", new)):
            with open(f"{d}/{name}", "wb") as f:
                f.write(data)
        code = ("import sys, detools\n"
                "with open(sys.argv[1], 'rb') as a, open(sys.argv[2], 'rb') as b, open(sys.argv[3], 'wb') as p:\n"
                "    detools.create_patch(a, b, p, compression='heatshrink', patch_type='sequential',\n"
                f"        heatshrink_window_sz2={HEATSHRINK_WINDOW}, heatshrink_lookahead_sz2={HEATSHRINK_LOOKAHEAD})\n")
        subprocess.run([py, "-c", code, f"{d}/old", f"{d}/new", f"{d}/patch"], check=True)
        with open(f"{d}/patch", "rb") as f:
            return f.read()


def push_sbom(base, host, repo, tag, idigest, isize, inventory, empty, root):
    """The release's SBOM (tools/sbom.py): in build/sbom/, in the registry as an
    artifact that refers to the index (OCI 1.1 subject), and in Trustify when
    $TRUSTIFY_URL is set."""
    doc = sbom.sbom_document(f"{host}/{repo}:{tag}", idigest, *inventory)
    data = (json.dumps(doc, indent=2) + "\n").encode()
    os.makedirs(f"{root}/build/sbom", exist_ok=True)
    path = f"{root}/build/sbom/{repo.rsplit('/', 1)[-1]}-{tag}.cdx.json"
    with open(path, "wb") as f:
        f.write(data)
    manifest = {
        "schemaVersion": 2,
        "mediaType": MANIFEST_TYPE,
        "artifactType": SBOM_TYPE,
        "config": {"mediaType": EMPTY_TYPE, "digest": empty, "size": len(EMPTY)},
        "layers": [{"mediaType": SBOM_TYPE, "digest": push_blob(base, repo, data), "size": len(data),
                    "annotations": {"org.opencontainers.image.title": os.path.basename(path)}}],
        "subject": {"mediaType": INDEX_TYPE, "digest": idigest, "size": isize},
    }
    # Tagged too, as cosign tags its attachments (sha256-<index>.sbom, next to
    # the signature's .sig): registry:3 has no referrers API to find it by.
    sdigest, _ = push_manifest(base, repo, f"{idigest.replace(':', '-')}.sbom", MANIFEST_TYPE, manifest)
    print(f"SBOM {path} ({len(doc['components'])} components), in the registry as {sdigest}")
    url = os.environ.get("TRUSTIFY_URL")
    if url:
        status, body = sbom.upload(url, doc, idigest)
        print(f"SBOM in Trustify: {status} {body[:120]}")


def push_elfs(base, repo, idigest, isize, elfs, empty):
    """The ELF of each platform's image, gzipped, in an artifact that refers to
    the index (OCI 1.1 subject), tagged as the SBOM is: sha256-<index>.elf."""
    layers = []
    for platform, path, image in elfs:
        if not os.path.exists(path):
            print(f"  {platform:8} no ELF ({path}): its crashes cannot be decoded")
            continue
        with open(path, "rb") as f:
            data = gzip.compress(f.read(), mtime=0)
        layers.append({"mediaType": ELF_TYPE, "digest": push_blob(base, repo, data), "size": len(data),
                       "annotations": {"org.opencontainers.image.title": f"flightctl_mcu_fw-{platform}.elf.gz",
                                       PLATFORM_KEY: platform, IMAGE_KEY: image}})
    if not layers:
        return
    manifest = {
        "schemaVersion": 2,
        "mediaType": MANIFEST_TYPE,
        "artifactType": ELF_ARTIFACT_TYPE,
        "config": {"mediaType": EMPTY_TYPE, "digest": empty, "size": len(EMPTY)},
        "layers": layers,
        "subject": {"mediaType": INDEX_TYPE, "digest": idigest, "size": isize},
    }
    edigest, _ = push_manifest(base, repo, f"{idigest.replace(':', '-')}.elf", MANIFEST_TYPE, manifest)
    size = sum(l["size"] for l in layers)
    print(f"ELF of {len(layers)} platforms ({size} bytes gzipped), in the registry as {edigest}")


def platform_of(build_dir):
    """The platform a build's image is for, as the firmware names it
    (fc_port_fw_platform in port/espidf/fc_port_espidf.c): the chip, and
    <chip>-r8 for an S3 built for octal PSRAM (VARIANT=r8). Builds of other
    variants (secure, release, ...) are for their chip's platform and are pushed
    with --variant."""
    try:
        cfg = open(f"{build_dir}/sdkconfig").read()
    except OSError:
        return None
    target = re.search(r'^CONFIG_IDF_TARGET="(\w+)"', cfg, re.M)
    if target is None:
        return None
    return target.group(1) + ("-r8" if re.search(r"^CONFIG_SPIRAM_MODE_OCT=y", cfg, re.M) else "")


def validate_nuttx_image(chip, data, path):
    # Keep a padded bootstrap image, or one for a different PSRAM/flash
    # configuration, out of the release before contacting the registry.
    board = chip.encode("ascii")
    if len(data) < 32 or data[:4] != bytes.fromhex("3db8f396"):
        sys.exit(f"{path}: missing MCUboot header")
    header, protected, body = struct.unpack_from("<HHI", data, 8)
    start = header + body
    arm_headers = {"nuttx-nrf7002dk": 512, "nuttx-nucleo-f767zi": 1024,
                   "nuttx-nucleo-f767zi-eth": 1024, "nuttx-pico2w": 512}
    # The Arm signing profile also includes a protected security counter.
    counter = b""
    if chip in arm_headers:
        counter = data[start + 4:start + 12]
        if len(counter) != 8 or struct.unpack_from("<HH", counter) != (0x50, 4):
            sys.exit(f"{path}: missing protected security counter")
    tag = struct.pack("<HH", 0x6908, 8 + len(counter) + len(board)) + counter
    tag += struct.pack("<HH", 0xa0, len(board)) + board
    end = start + protected
    if (header != arm_headers.get(chip, 32) or not body or data[start:end] != tag or end + 4 > len(data)):
        sys.exit(f"{path}: missing protected MCUboot platform {chip}")
    magic, size = struct.unpack_from("<HH", data, end)
    if magic != 0x6907 or size < 4 or end + size != len(data):
        sys.exit(f"{path}: use the unpadded nuttx-ota.bin")


def main():
    args = sys.argv[1:]
    variant, delta_from = "", []
    while len(args) > 1 and args[0] in ("--variant", "--delta-from"):
        if args[0] == "--variant":
            variant = "-" + args[1]
        else:
            delta_from.append(args[1])
        args = args[2:]
    if not args or args[0].startswith("-"):
        sys.exit(__doc__)
    ref = args[0]
    host, rest = ref.split("/", 1)
    repo, tag = rest.rsplit(":", 1)
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    fw_dir = f"{root}/port/espidf"
    chips = args[1:] or sorted(
        name for d in glob.glob(f"{fw_dir}/build-esp32*{variant}")
        if os.path.exists(f"{d}/flightctl_mcu_fw.bin")
        and (name := os.path.basename(d)[len("build-"):len(os.path.basename(d)) - len(variant)]) == platform_of(d))
    if not chips:
        sys.exit("no firmware builds found")

    # Validate the complete inventory before the first registry request. Keep
    # the bytes checked here so a concurrent rebuild cannot change the upload.
    images, esp_idf, zephyr, nuttx = [], [], [], []
    for spec in chips:
        chip, explicit_image, path = spec.partition("=")
        if not explicit_image:
            path = f"{fw_dir}/build-{chip}{variant}/flightctl_mcu_fw.bin"
        try:
            data = Path(path).read_bytes()
        except OSError as e:
            sys.exit(str(e))
        if explicit_image and chip.startswith("nuttx-"):
            validate_nuttx_image(chip, data, path)
            nuttx.append(chip)
            images.append((chip, "nuttx", path, data))
        elif explicit_image:
            zephyr.append(chip)
            images.append((chip, "zephyr", path, data))
        else:
            esp_idf.append((chip, sbom.image_idf_version(data, path)))
            images.append((chip, "esp-idf", path, data))
    inventory = sbom.release_inventory(esp_idf, zephyr, nuttx)

    base = registry_client.base_url(host)
    empty = push_blob(base, repo, EMPTY)
    py = detools_python(root) if delta_from else None
    entries, elfs = [], []
    for chip, platform_os, path, data in images:
        layer_type = {"esp-idf": LAYER_TYPE, "zephyr": ZEPHYR_LAYER_TYPE,
                      "nuttx": "application/vnd.nuttx.mcuboot-image"}[platform_os]
        digest = push_blob(base, repo, data)
        elf = {"nuttx": "nuttx", "zephyr": "zephyr.elf", "esp-idf": "flightctl_mcu_fw.elf"}[platform_os]
        elfs.append((chip, os.path.join(os.path.dirname(path), elf), digest))
        # The whole image first: firmware that knows nothing of deltas takes
        # the first layer.
        layers = [{
            "mediaType": layer_type, "digest": digest, "size": len(data),
            "annotations": {"org.opencontainers.image.title": f"flightctl_mcu_fw-{chip}.bin"},
        }]
        print(f"  {chip:8} {len(data):8} bytes  {digest}")
        for old_ref in delta_from:
            old = pull_image(base, repo, old_ref, chip)
            if old is None:
                print(f"           no {chip} image in {old_ref}: no delta from it")
                continue
            if sha(old) == digest or any(l["annotations"].get(DELTA_FROM) == sha(old) for l in layers):
                continue
            patch = make_delta(py, old, data)
            if len(patch) >= len(data):
                print(f"           delta from {old_ref} would be {len(patch)} bytes: left out")
                continue
            layers.append({
                "mediaType": DELTA_TYPE, "digest": push_blob(base, repo, patch), "size": len(patch),
                "annotations": {"org.opencontainers.image.title": f"flightctl_mcu_fw-{chip}.delta",
                                DELTA_FROM: sha(old), DELTA_FROM_SIZE: str(len(old))},
            })
            print(f"           {len(patch):8} bytes  delta from {old_ref} ({100 * len(patch) / len(data):.1f}%)")
        manifest = {
            "schemaVersion": 2,
            "mediaType": MANIFEST_TYPE,
            "artifactType": ARTIFACT_TYPE,
            "config": {"mediaType": EMPTY_TYPE, "digest": empty, "size": len(EMPTY)},
            "layers": layers,
            "annotations": {"org.opencontainers.image.version": tag},
        }
        mdigest, msize = push_manifest(base, repo, None, MANIFEST_TYPE, manifest)
        entries.append({"mediaType": MANIFEST_TYPE, "digest": mdigest, "size": msize,
                        "artifactType": ARTIFACT_TYPE,
                        "platform": {"os": platform_os, "architecture": chip}})

    index = {"schemaVersion": 2, "mediaType": INDEX_TYPE, "artifactType": ARTIFACT_TYPE,
             "manifests": entries, "annotations": {"org.opencontainers.image.version": tag}}
    idigest, isize = push_manifest(base, repo, tag, INDEX_TYPE, index)
    print(f"{ref}@{idigest}")
    push_sbom(base, host, repo, tag, idigest, isize, inventory, empty, root)
    key = os.environ.get("SIGNING_KEY") or f"{root}/.secrets/signing.key"
    if not os.environ.get("NO_SIGN") and os.path.exists(key):
        subprocess.run([f"{root}/tools/signing/sign.py", "--key", key, f"{host}/{repo}@{idigest}"], check=True)
    else:
        print("not signed (no signing key)")
    # Last, once the release is complete and signed: the ELFs are for whoever
    # decodes a crash, and a failure here leaves the release as it is.
    try:
        push_elfs(base, repo, idigest, isize, elfs, empty)
    except (OSError, urllib.error.URLError) as e:
        print(f"warning: the ELFs were not pushed ({e}): crashes of this release cannot be decoded from the registry")


if __name__ == "__main__":
    main()
