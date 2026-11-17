#!/usr/bin/env python3
"""Adds to the fleets on stdin the broker their apps publish to
(docs/APPS.md#mqtt): /etc/fc/mqtt.json and /etc/fc/mqtt-ca.crt, as the
config "mqtt", from the lab's broker (tools/mqtt/up.sh, .secrets/mqtt/).

Each fleet gets the device prefix's default, flightctl/{device}/{app}/, and
a fleet prefix with its own name, flightctl/fleet/<fleet>/{app}/, for
commands to all its devices. The password ends up in the fleet's spec on the
server: anyone who can read the fleet can read it (the broker limits the
user to flightctl/#).

Not part of tools/fleet/apply.sh yet; to try it:

    sed -e "s|\\${REGISTRY}|$(cat .secrets/registry/host)|g" tools/fleet/idf-lab.yaml \\
      | tools/fleet/registry-config.py "$(cat .secrets/registry/host)" \\
      | tools/fleet/mqtt-config.py > /tmp/idf-lab.yaml     # then read it before applying

Usage: tools/fleet/mqtt-config.py [secrets dir] < fleet.yaml > fleet-with-mqtt.yaml
"""
import json
import os
import sys

import yaml

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")


def files_for(fleet, secrets):
    def read(name):
        with open(os.path.join(secrets, name)) as f:
            return f.read()

    conf = {
        "broker": "mqtts://" + read("host").strip(),
        "username": "device",
        "password": read("password").strip(),
        "fleetPrefix": f"flightctl/fleet/{fleet}/{{app}}/",
    }
    return [
        {"path": "/etc/fc/mqtt.json", "content": json.dumps(conf)},
        {"path": "/etc/fc/mqtt-ca.crt", "content": read("ca.crt")},
    ]


def main():
    if len(sys.argv) > 2 or (len(sys.argv) == 2 and sys.argv[1].startswith("-")):
        sys.exit(__doc__)
    secrets = sys.argv[1] if len(sys.argv) == 2 else os.path.join(ROOT, ".secrets", "mqtt")
    if not os.path.exists(os.path.join(secrets, "host")):
        sys.exit(f"no broker in {secrets}: start it with tools/mqtt/up.sh")
    docs = [d for d in yaml.safe_load_all(sys.stdin) if d is not None]
    for d in docs:
        if d.get("kind") == "Fleet":
            spec = d["spec"]["template"].setdefault("spec", {})
            config = [c for c in spec.get("config") or [] if c.get("name") != "mqtt"]
            config.append({"name": "mqtt", "inline": files_for(d["metadata"]["name"], secrets)})
            spec["config"] = config
    yaml.safe_dump_all(docs, sys.stdout, sort_keys=False)


if __name__ == "__main__":
    main()
