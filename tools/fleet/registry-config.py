#!/usr/bin/env python3
"""Adds to the fleets on stdin what their devices need to pull from REGISTRY
over TLS: its CA and the credentials, as config files, written where
containers (and the Linux agent) look for them:

    /etc/containers/certs.d/<host>/ca.crt    (for any port: Flight Control
                                             refuses ':' in a path)
    /etc/containers/auth.json

Nothing is added for a registry without a CA (plain HTTP). The CA and the
credentials are those of tools/registry/registry_client.py (.secrets/registry,
made by tools/registry/up.sh). They end up in the fleet's spec on the server:
anyone who can read the fleet can read them.

    ... | tools/fleet/registry-config.py <host:port> | flightctl apply -f -
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "registry"))
import registry_client  # noqa: E402


def files_for(registry):
    ca = registry_client.ca_for(registry)
    if not ca:
        return []
    host = registry.split(":")[0]
    files = [{"path": f"/etc/containers/certs.d/{host}/ca.crt", "content": open(ca).read()}]
    auth = registry_client.auth_file()
    if os.path.exists(auth):
        # As text: Flight Control 1.4.0 serves content given in base64
        # percent-encoded ("%3D" for the padding), which agents may not undo.
        files.append({"path": "/etc/containers/auth.json", "content": open(auth).read()})
    return files


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    files = files_for(sys.argv[1])
    if not files:
        sys.stdout.write(sys.stdin.read())  # plain HTTP: the fleet as it is
        return
    import yaml
    docs = [d for d in yaml.safe_load_all(sys.stdin) if d is not None]
    for d in docs:
        if files and d.get("kind") == "Fleet":
            spec = d["spec"]["template"].setdefault("spec", {})
            config = [c for c in spec.get("config") or [] if c.get("name") != "registry"]
            config.append({"name": "registry", "inline": files})
            spec["config"] = config
    yaml.safe_dump_all(docs, sys.stdout, sort_keys=False)


if __name__ == "__main__":
    main()
