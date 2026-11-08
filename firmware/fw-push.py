#!/usr/bin/env python3
"""Pushes the firmware builds as one multi-chip OCI artifact.

    firmware/fw-push.py [--variant NAME] <registry>/flightctl-mcu/fw:v2 [esp32s3 esp32c3 ...]

Layout (OCI 1.1, plain HTTP registry, stdlib only):
  index (tag)      artifactType application/vnd.flightctl-mcu.firmware.v1
    manifest       platform {os: esp-idf, architecture: <chip>}
      layer        application/vnd.espressif.app-image  (build-<chip>/flightctl_mcu_fw.bin)

The device picks the manifest whose platform.architecture is its chip, streams
the layer into the inactive OTA slot and checks its sha256.
Default chips: every firmware/build-<chip>[-<variant>]/ with a built image.

The index is then signed with deploy/signing/sign.py when a signing key is
there ($SIGNING_KEY or .secrets/signing.key); NO_SIGN=1 skips it.
"""
import glob
import hashlib
import json
import os
import subprocess
import sys
import urllib.request

ARTIFACT_TYPE = "application/vnd.flightctl-mcu.firmware.v1"
LAYER_TYPE = "application/vnd.espressif.app-image"
EMPTY = b"{}"
EMPTY_TYPE = "application/vnd.oci.empty.v1+json"
MANIFEST_TYPE = "application/vnd.oci.image.manifest.v1+json"
INDEX_TYPE = "application/vnd.oci.image.index.v1+json"


def sha(data):
    return "sha256:" + hashlib.sha256(data).hexdigest()


def request(method, url, data=None, headers=None):
    req = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    with urllib.request.urlopen(req) as r:
        return r.status, r.headers, r.read()


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


def main():
    args = sys.argv[1:]
    variant = ""
    if args[:1] == ["--variant"] and len(args) > 1:
        variant, args = "-" + args[1], args[2:]
    if not args:
        sys.exit(__doc__)
    ref = args[0]
    host, rest = ref.split("/", 1)
    repo, tag = rest.rsplit(":", 1)
    base = f"http://{host}"
    fw_dir = os.path.dirname(os.path.abspath(__file__))
    chips = args[1:] or sorted(
        os.path.basename(d)[len("build-"):len(os.path.basename(d)) - len(variant)]
        for d in glob.glob(f"{fw_dir}/build-esp32*{variant}")
        if os.path.exists(f"{d}/flightctl_mcu_fw.bin")
        and os.path.basename(d).count("-") == 1 + variant.count("-"))
    if not chips:
        sys.exit("no firmware builds found")

    empty = push_blob(base, repo, EMPTY)
    entries = []
    for chip in chips:
        data = open(f"{fw_dir}/build-{chip}{variant}/flightctl_mcu_fw.bin", "rb").read()
        digest = push_blob(base, repo, data)
        manifest = {
            "schemaVersion": 2,
            "mediaType": MANIFEST_TYPE,
            "artifactType": ARTIFACT_TYPE,
            "config": {"mediaType": EMPTY_TYPE, "digest": empty, "size": len(EMPTY)},
            "layers": [{
                "mediaType": LAYER_TYPE, "digest": digest, "size": len(data),
                "annotations": {"org.opencontainers.image.title": f"flightctl_mcu_fw-{chip}.bin"},
            }],
            "annotations": {"org.opencontainers.image.version": tag},
        }
        mdigest, msize = push_manifest(base, repo, None, MANIFEST_TYPE, manifest)
        entries.append({"mediaType": MANIFEST_TYPE, "digest": mdigest, "size": msize,
                        "artifactType": ARTIFACT_TYPE,
                        "platform": {"os": "esp-idf", "architecture": chip}})
        print(f"  {chip:8} {len(data):8} bytes  {digest}")

    index = {"schemaVersion": 2, "mediaType": INDEX_TYPE, "artifactType": ARTIFACT_TYPE,
             "manifests": entries, "annotations": {"org.opencontainers.image.version": tag}}
    idigest, _ = push_manifest(base, repo, tag, INDEX_TYPE, index)
    print(f"{ref}@{idigest}")
    root = os.path.dirname(fw_dir)
    key = os.environ.get("SIGNING_KEY") or f"{root}/.secrets/signing.key"
    if not os.environ.get("NO_SIGN") and os.path.exists(key):
        subprocess.run([f"{root}/deploy/signing/sign.py", "--key", key, f"{host}/{repo}@{idigest}"], check=True)
    else:
        print("not signed (no signing key)")


if __name__ == "__main__":
    main()
