#!/usr/bin/env python3
"""Writes fleet manifests ready to be kept in Git and imported by Flight
Control (a ResourceSync, or Import Fleet in the UI), which applies them as
they are:

  - ${REGISTRY} is spelled out: the server fills in device labels in a
    template, not this;
  - the registry's CA and credentials are not in the files: the fleet
    mounts them from Kubernetes secrets (config of type secretRef), which
    tools/fleet/registry-secrets.sh makes. They are the same files that
    tools/fleet/apply.sh puts inline (docs/REGISTRY.md).

    tools/fleet/git-export.py <host:port> <out dir> <fleet.yaml>...

The secrets are named fc-registry-ca and fc-registry-auth, in the namespace
$FLEET_SECRETS_NAMESPACE (default flightctl). Comments in the manifests are
not kept. Needs PyYAML.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "registry"))
import registry_client  # noqa: E402

NAMESPACE = os.environ.get("FLEET_SECRETS_NAMESPACE", "flightctl")
CA_SECRET = "fc-registry-ca"
AUTH_SECRET = "fc-registry-auth"


def providers_for(registry):
    """The config providers that mount the registry's CA and credentials."""
    if not registry_client.ca_for(registry):
        return []  # plain HTTP: nothing to mount
    host = registry.split(":")[0]
    out = [{"name": "registry-ca",
            "secretRef": {"name": CA_SECRET, "namespace": NAMESPACE,
                          "mountPath": f"/etc/containers/certs.d/{host}"}}]
    if os.path.exists(registry_client.auth_file()):
        out.append({"name": "registry-auth",
                    "secretRef": {"name": AUTH_SECRET, "namespace": NAMESPACE,
                                  "mountPath": "/etc/containers"}})
    return out


def export(text, registry):
    import yaml
    docs = [d for d in yaml.safe_load_all(text.replace("${REGISTRY}", registry)) if d is not None]
    providers = providers_for(registry)
    for d in docs:
        if d.get("kind") != "Fleet":
            continue
        spec = d["spec"]["template"].setdefault("spec", {})
        names = {"registry", "registry-ca", "registry-auth"}
        config = [c for c in spec.get("config") or [] if c.get("name") not in names]
        spec["config"] = config + providers
    return yaml.safe_dump_all(docs, sort_keys=False, width=1 << 20)


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    registry, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for path in sys.argv[3:]:
        with open(path) as f:
            text = export(f.read(), registry)
        dest = os.path.join(out, os.path.basename(path))
        with open(dest, "w") as f:
            f.write(text)
        print(dest)


if __name__ == "__main__":
    main()
