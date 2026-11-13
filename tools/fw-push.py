#!/usr/bin/env python3
"""Pushes the firmware builds as one multi-chip OCI artifact.

    tools/fw-push.py [--variant NAME] [--delta-from TAG ...] <registry>/flightctl-mcu/fw:v2 [esp32s3 esp32c3 ...]
    tools/fw-push.py ... <registry>/flightctl-mcu/fw:v2 rpi_pico_w=port/zephyr/build-pico-mcuboot/zephyr/zephyr/zephyr.signed.bin

Layout (OCI 1.1; the registry reached as tools/registry/registry_client.py says;
stdlib only):
  index (tag)      artifactType application/vnd.flightctl-mcu.firmware.v1
    manifest       platform {os: esp-idf, architecture: <chip>}
      layer        application/vnd.espressif.app-image  (build-<chip>/flightctl_mcu_fw.bin)
      layer ...    application/vnd.flightctl-mcu.firmware.delta.v1+detools  (one per --delta-from)

The device picks the manifest whose platform.architecture is its chip, streams
the layer into the inactive OTA slot and checks its sha256.
Default chips: every port/espidf/build-<chip>[-<variant>]/ with a built image.
PLATFORM=FILE instead of a chip pushes FILE for that platform: a Zephyr build
signed for MCUboot (zephyr.signed.bin), for the board whose
CONFIG_FC_FW_PLATFORM is PLATFORM. Its manifest says os "zephyr" and its layer
application/vnd.zephyr.mcuboot-image.

--delta-from TAG (a tag or digest in the same repository, repeatable): also
push, for each chip, a delta from that release's image to this one. A device
running that image downloads the delta instead and rebuilds the new image
from its own (docs/DELTA.md); the others, and older firmware, take the whole
image. A delta is a detools patch (sequential, heatshrink): this needs the
detools package, here or in a virtual environment made in build/ on first use.

The release's SBOM (tools/sbom.py, CycloneDX) goes to build/sbom/ and to the
registry, as an artifact whose subject is the index; to Trustify too when
TRUSTIFY_URL is set (docs/SBOM.md).

The index is then signed with tools/signing/sign.py when a signing key is
there ($SIGNING_KEY or .secrets/signing.key); NO_SIGN=1 skips it.
"""
import glob
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "registry"))
import registry_client  # noqa: E402

ARTIFACT_TYPE = "application/vnd.flightctl-mcu.firmware.v1"
LAYER_TYPE = "application/vnd.espressif.app-image"
ZEPHYR_LAYER_TYPE = "application/vnd.zephyr.mcuboot-image"
DELTA_TYPE = "application/vnd.flightctl-mcu.firmware.delta.v1+detools"
DELTA_FROM = "io.flightctl-mcu.delta.from"
DELTA_FROM_SIZE = "io.flightctl-mcu.delta.from-size"
DETOOLS_VERSION = "0.53.1"  # the version third_party/fetch-detools.sh fetches for the device
# What the device's decoder is built for (core/src/fc_delta.h): a 4 KB window.
HEATSHRINK_WINDOW, HEATSHRINK_LOOKAHEAD = 12, 8
EMPTY = b"{}"
EMPTY_TYPE = "application/vnd.oci.empty.v1+json"
SBOM_TYPE = "application/vnd.cyclonedx+json"
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


def push_sbom(base, host, repo, tag, idigest, isize, entries, empty, root):
    """The release's SBOM (tools/sbom.py): in build/sbom/, in the registry as an
    artifact that refers to the index (OCI 1.1 subject), and in Trustify when
    $TRUSTIFY_URL is set."""
    sys.path.insert(0, os.path.join(root, "tools"))
    import sbom
    esp_idf = [e["platform"]["architecture"] for e in entries if e["platform"]["os"] == "esp-idf"]
    zephyr = [e["platform"]["architecture"] for e in entries if e["platform"]["os"] == "zephyr"]
    doc = sbom.make_sbom(f"{host}/{repo}:{tag}", idigest, esp_idf, zephyr)
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
    base = registry_client.base_url(host)
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    fw_dir = f"{root}/port/espidf"
    chips = args[1:] or sorted(
        os.path.basename(d)[len("build-"):len(os.path.basename(d)) - len(variant)]
        for d in glob.glob(f"{fw_dir}/build-esp32*{variant}")
        if os.path.exists(f"{d}/flightctl_mcu_fw.bin")
        and os.path.basename(d).count("-") == 1 + variant.count("-"))
    if not chips:
        sys.exit("no firmware builds found")

    empty = push_blob(base, repo, EMPTY)
    py = detools_python(root) if delta_from else None
    entries = []
    for chip in chips:
        path, platform_os, layer_type = f"{fw_dir}/build-{chip}{variant}/flightctl_mcu_fw.bin", "esp-idf", LAYER_TYPE
        if "=" in chip:
            chip, path = chip.split("=", 1)
            platform_os, layer_type = "zephyr", ZEPHYR_LAYER_TYPE
        data = open(path, "rb").read()
        digest = push_blob(base, repo, data)
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
    push_sbom(base, host, repo, tag, idigest, isize, entries, empty, root)
    key = os.environ.get("SIGNING_KEY") or f"{root}/.secrets/signing.key"
    if not os.environ.get("NO_SIGN") and os.path.exists(key):
        subprocess.run([f"{root}/tools/signing/sign.py", "--key", key, f"{host}/{repo}@{idigest}"], check=True)
    else:
        print("not signed (no signing key)")


if __name__ == "__main__":
    main()
