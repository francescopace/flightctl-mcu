#!/usr/bin/env python3
"""Approves enrollment requests from devices in an allow list.

    deploy/auto-approve/auto-approve.py ALLOWLIST.yaml [--watch SECONDS] [--dry-run]

Uses the flightctl CLI, so it acts as whoever is logged in. Pending requests
are matched against the list by device name or by MAC address, and approved
with the labels the list gives them (defaults first, then the device's own).
Requests that match nothing are left for a person to look at.

Matching by name is the strong option: the name is a hash of the device's
public key, so only that device can present it. The MAC is what the device
reports about itself in the request; it is an inventory check, not proof of
identity, and assumes the enrollment certificate is only in your devices.
"""
import json
import subprocess
import sys
import time

import yaml


def load(path):
    with open(path) as f:
        doc = yaml.safe_load(f) or {}
    defaults = (doc.get("defaults") or {}).get("labels") or {}
    by_name, by_mac = {}, {}
    for d in doc.get("devices") or []:
        labels = {**defaults, **(d.get("labels") or {})}
        if d.get("name"):
            by_name[d["name"]] = labels
        if d.get("mac"):
            by_mac[d["mac"].lower()] = labels
    return by_name, by_mac


def pending():
    out = subprocess.run(["flightctl", "get", "enrollmentrequests", "-o", "json"],
                         check=True, capture_output=True, text=True).stdout
    for er in json.loads(out).get("items") or []:
        st = er.get("status") or {}
        if (st.get("approval") or {}).get("approved"):
            continue
        if any(c.get("type") == "Denied" and c.get("status") == "True" for c in st.get("conditions") or []):
            continue
        yield er


def mac_of(er):
    info = ((er.get("spec") or {}).get("deviceStatus") or {}).get("systemInfo") or {}
    return ((info.get("customInfo") or {}).get("mac") or "").lower()


def run_once(allow, dry_run, seen):
    by_name, by_mac = allow
    for er in pending():
        name = er["metadata"]["name"]
        mac = mac_of(er)
        labels = by_name.get(name) if name in by_name else by_mac.get(mac) if mac else None
        if labels is None:
            if name not in seen:
                print(f"not in the allow list: {name} (mac {mac or '-'})")
                seen.add(name)
            continue
        cmd = ["flightctl", "approve"]
        for k, v in sorted(labels.items()):
            cmd += ["-l", f"{k}={v}"]
        cmd.append(f"enrollmentrequest/{name}")
        print(("would approve: " if dry_run else "approving: ") + " ".join(cmd[2:]))
        if not dry_run:
            subprocess.run(cmd, check=True, capture_output=True)


def main():
    args = sys.argv[1:]
    watch, dry_run = 0, False
    if "--dry-run" in args:
        args.remove("--dry-run")
        dry_run = True
    if "--watch" in args:
        i = args.index("--watch")
        watch = int(args[i + 1])
        del args[i:i + 2]
    if len(args) != 1:
        sys.exit(__doc__)
    seen = set()
    while True:
        run_once(load(args[0]), dry_run, seen)  # re-read: the list can change while watching
        if not watch:
            return
        time.sleep(watch)


if __name__ == "__main__":
    main()
