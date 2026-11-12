#!/usr/bin/env python3
"""Soak test: samples every device's status from Flight Control into a CSV.

    tools/soak/collect.py OUT.csv [--every SECONDS] [--selector k=v]     sample, until stopped
    tools/soak/collect.py --report OUT.csv                               summary per device

Uses the flightctl CLI, as whoever is logged in. Each round appends one row
per device: its state and the metrics it reports in customInfo (heap, its
low-water mark, the largest internal block, RSSI, temperature, traffic,
uptime, firmware, reset reason), which the devices read again once an hour
(metrics_interval_s). Leave it running for weeks next to the lab
fleet; --report then tells, per device, how long it was watched, how often it
was not online, how many times it restarted and why, whether the heap's
low-water mark kept falling (a leak), and its traffic per hour.
"""
import argparse
import csv
import datetime
import json
import os
import subprocess
import sys
import time

FIELDS = ["time", "device", "status", "updated", "firmwareVersion", "uptimeSeconds", "resetReason",
          "memFreeBytes", "memMinFreeBytes", "memInternalLargestBlock", "wifiRssi", "temperatureC",
          "netTxBytes", "netRxBytes"]


def rows(devices, now):
    """One row per device of a `flightctl get devices -o json` document."""
    for d in devices.get("items") or []:
        st = d.get("status") or {}
        ci = (st.get("systemInfo") or {}).get("customInfo") or {}
        row = {"time": now, "device": d["metadata"]["name"],
               "status": (st.get("summary") or {}).get("status", ""),
               "updated": (st.get("updated") or {}).get("status", "")}
        for f in FIELDS[4:]:
            row[f] = ci.get(f, "")
        yield row


def sample(out, selector):
    cmd = ["flightctl", "get", "devices", "-o", "json"] + (["-l", selector] if selector else [])
    doc = json.loads(subprocess.run(cmd, check=True, capture_output=True, text=True).stdout)
    now = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    new = not os.path.exists(out) or os.path.getsize(out) == 0
    with open(out, "a", newline="") as f:
        w = csv.DictWriter(f, FIELDS)
        if new:
            w.writeheader()
        n = 0
        for row in rows(doc, now):
            w.writerow(row)
            n += 1
    return n


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def report(path):
    """Per device: hours watched, share of samples not Online, restarts and
    their reasons, heap low-water mark first/last/lowest, traffic per hour."""
    by = {}
    with open(path) as f:
        for r in csv.DictReader(f):
            by.setdefault(r["device"], []).append(r)
    out = []
    for dev, rs in sorted(by.items()):
        t0 = datetime.datetime.fromisoformat(rs[0]["time"].replace("Z", "+00:00"))
        t1 = datetime.datetime.fromisoformat(rs[-1]["time"].replace("Z", "+00:00"))
        hours = (t1 - t0).total_seconds() / 3600
        offline = sum(1 for r in rs if r["status"] != "Online")
        restarts, reasons, prev = 0, {}, None
        for r in rs:
            up = num(r["uptimeSeconds"])
            if up is not None and prev is not None and up < prev:
                restarts += 1
                reasons[r["resetReason"] or "?"] = reasons.get(r["resetReason"] or "?", 0) + 1
            if up is not None:
                prev = up
        mins = [m for m in (num(r["memMinFreeBytes"]) for r in rs) if m is not None]
        tx = [num(r["netTxBytes"]) for r in rs]
        tx = [t for t in tx if t is not None]
        # Counters reset on restart: sum only the increases.
        sent = sum(b - a for a, b in zip(tx, tx[1:]) if b >= a)
        out.append({
            "device": dev, "hours": round(hours, 1), "samples": len(rs),
            "notOnline": f"{100 * offline / len(rs):.1f}%",
            "restarts": restarts, "reasons": reasons,
            "memMinFree": (int(mins[0]), int(mins[-1]), int(min(mins))) if mins else None,
            "txBytesPerHour": int(sent / hours) if hours > 0 else None,
        })
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("csv")
    p.add_argument("--every", type=int, default=300)
    p.add_argument("--selector")
    p.add_argument("--report", action="store_true")
    a = p.parse_args()
    if a.report:
        for r in report(a.csv):
            print(json.dumps(r))
        return
    while True:
        try:
            print(f"{time.strftime('%H:%M:%S')} {sample(a.csv, a.selector)} devices", flush=True)
        except (subprocess.CalledProcessError, json.JSONDecodeError) as e:
            print(f"{time.strftime('%H:%M:%S')} sample failed: {e}", file=sys.stderr, flush=True)
        time.sleep(a.every)


if __name__ == "__main__":
    main()
