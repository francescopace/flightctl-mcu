#!/usr/bin/env python3
"""Signs an image in the registry, in cosign's format, with an ECDSA P-256 key.

    tools/signing/sign.py <registry>/<repo>:<tag>|@sha256:... [--key FILE]

The key defaults to $SIGNING_KEY, then .secrets/signing.key. The signature is
stored like `cosign sign --key` stores it, so `cosign verify --key signing.pub`
accepts it and the device checks the same thing:

  tag sha256-<digest>.sig   manifest with one layer per signature
    layer                   simplesigning payload naming the image digest,
                            annotation dev.cosignproject.cosign/signature

What gets signed is the digest of what the reference points to (the
multi-chip index for firmware), so the signature covers every manifest and
layer below it. The registry is reached as tools/registry/registry_client.py says
(plain HTTP, or TLS with credentials); stdlib and the openssl CLI only.
"""
import base64
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "registry"))
import registry_client  # noqa: E402

MANIFEST_TYPE = "application/vnd.oci.image.manifest.v1+json"
INDEX_TYPE = "application/vnd.oci.image.index.v1+json"
PAYLOAD_TYPE = "application/vnd.dev.cosign.simplesigning.v1+json"
CONFIG_TYPE = "application/vnd.oci.image.config.v1+json"
SIG_ANNOTATION = "dev.cosignproject.cosign/signature"


def sha(data):
    return "sha256:" + hashlib.sha256(data).hexdigest()


def request(method, url, data=None, headers=None):
    return registry_client.request(method, url, data, headers)


def push_blob(base, repo, data):
    digest = sha(data)
    try:
        request("HEAD", f"{base}/v2/{repo}/blobs/{digest}")
        return digest
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


def split_ref(ref):
    host, rest = ref.split("/", 1)
    if "@" in rest:
        repo, target = rest.split("@", 1)
    else:
        repo, target = rest.rsplit(":", 1) if ":" in rest.rsplit("/", 1)[-1] else (rest, "latest")
    return host, repo, target


def resolve(base, repo, target):
    """Digest of the manifest or index that target (tag or digest) names."""
    _, _, body = request("GET", f"{base}/v2/{repo}/manifests/{target}",
                         headers={"Accept": f"{INDEX_TYPE}, {MANIFEST_TYPE}"})
    digest = sha(body)
    if target.startswith("sha256:") and target != digest:
        sys.exit(f"{target}: registry returned a manifest with digest {digest}")
    return digest


def payload_for(host, repo, digest):
    """The simplesigning payload cosign signs: it names the image digest."""
    return json.dumps({
        "critical": {
            "identity": {"docker-reference": f"{host}/{repo}"},
            "image": {"docker-manifest-digest": digest},
            "type": "cosign container image signature",
        },
        "optional": None,
    }, separators=(",", ":")).encode()


def der_signature(payload, key):
    """ECDSA over the payload's SHA-256, DER encoded, as cosign stores it."""
    with tempfile.NamedTemporaryFile() as f:
        f.write(payload)
        f.flush()
        return subprocess.run(["openssl", "dgst", "-sha256", "-sign", key, f.name],
                              check=True, capture_output=True).stdout


def config_for(payload_digest):
    return json.dumps({
        "architecture": "", "created": "0001-01-01T00:00:00Z",
        "history": [{"created": "0001-01-01T00:00:00Z"}], "os": "",
        "rootfs": {"type": "layers", "diff_ids": [payload_digest]}, "config": {},
    }, separators=(",", ":")).encode()


def signature_manifest(payload, der, config):
    return {
        "schemaVersion": 2,
        "mediaType": MANIFEST_TYPE,
        "config": {"mediaType": CONFIG_TYPE, "size": len(config), "digest": sha(config)},
        "layers": [{
            "mediaType": PAYLOAD_TYPE, "size": len(payload), "digest": sha(payload),
            "annotations": {SIG_ANNOTATION: base64.b64encode(der).decode()},
        }],
    }


def signature_tag(digest):
    return digest.replace(":", "-") + ".sig"


def sign(ref, key):
    host, repo, target = split_ref(ref)
    base = registry_client.base_url(host)
    digest = resolve(base, repo, target)
    payload = payload_for(host, repo, digest)
    der = der_signature(payload, key)
    config = config_for(push_blob(base, repo, payload))
    push_blob(base, repo, config)
    manifest = signature_manifest(payload, der, config)
    tag = signature_tag(digest)
    request("PUT", f"{base}/v2/{repo}/manifests/{tag}",
            json.dumps(manifest, separators=(",", ":")).encode(), {"Content-Type": MANIFEST_TYPE})
    print(f"signed {host}/{repo}@{digest}")


def main():
    args = sys.argv[1:]
    key = os.environ.get("SIGNING_KEY") or os.path.join(os.path.dirname(__file__), "../../.secrets/signing.key")
    if "--key" in args:
        i = args.index("--key")
        key = args[i + 1]
        del args[i:i + 2]
    if len(args) != 1:
        sys.exit(__doc__)
    if not os.path.exists(key):
        sys.exit(f"no signing key at {key} (create one with tools/signing/keygen.sh)")
    sign(args[0], key)


if __name__ == "__main__":
    main()
