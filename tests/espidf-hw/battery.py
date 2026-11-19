"""Exercise the signed battery app on an emulated ESP32-C3 against Flight Control on kind."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import tempfile
import time
from uuid import uuid4

from run import send_input

ADC = {
    "adc": {"vbat_adc": {"pin": 3}},
    "sensor": {"battery_mv": {"source": "battery_adc", "input": "vbat_adc", "divider_num": 2, "divider_den": 1}},
}
SBS = {
    "i2c": {"gauge": {"sda": 5, "scl": 6}},
    "sensor": {
        "battery_mv": {"source": "sbs_voltage", "input": "gauge"},
        "battery_pct": {"source": "sbs_percent", "input": "gauge"},
        "battery_charging": {"source": "sbs_charging", "input": "gauge"},
    },
}
GATEWAY = "/api/v1/namespaces/flightctl/services/http:flightctl-telemetry-gateway:metrics/proxy/metrics"


def apply_fleet(name, ip, registry, board):
    files = {"/etc/fc/board.json": board, "/etc/fc-app/battery.json": {"low_mv": 3600, "sample_ms": 2000},
             "/etc/fc/metrics.json": {"endpoint": f"telemetry.{ip}.nip.io:8443", "intervalSeconds": 30}}
    app = {"name": "battery", "appType": "container", "image": f"{registry}/flightctl-mcu/battery:{name}",
           "envVars": {"FC_ALLOW": "metrics " + " ".join(board["sensor"])}}
    spec = {"applications": [app], "config": [{"name": "battery", "inline": [
        {"path": path, "content": json.dumps(content)} for path, content in files.items()]}]}
    fleet = {"apiVersion": "flightctl.io/v1beta1", "kind": "Fleet", "metadata": {"name": name},
             "spec": {"selector": {"matchLabels": {"app": name}}, "template": {"spec": spec}}}
    manifest = subprocess.check_output(["tools/fleet/registry-config.py", registry], input=json.dumps(fleet).encode())
    subprocess.run(["flightctl", "apply", "-f", "-"], input=manifest, check=True)


def wait_device(emu, log):
    deadline = time.monotonic() + 180
    while time.monotonic() < deadline and emu.poll() is None:
        # The emulator reports the public-key-derived ID after it creates its identity.
        match = re.search(r"device name: ([a-z0-9]{52})\b", log.read_text(errors="replace"))
        if match:
            return match[1]
        time.sleep(2)
    raise RuntimeError("C3 did not report its device ID")


def approve(emu, device, fleet):
    request = f"enrollmentrequest/{device}"
    deadline = time.monotonic() + 180
    while time.monotonic() < deadline and emu.poll() is None:
        if subprocess.run(["flightctl", "get", request], capture_output=True, timeout=20).returncode == 0:
            subprocess.run(["flightctl", "approve", request, "-l", f"app={fleet}"], check=True)
            return
        time.sleep(2)
    raise RuntimeError(f"C3 did not submit {request}")


def wait_metrics(emu, device, **expected):
    deadline = time.monotonic() + 240
    last = None
    while time.monotonic() < deadline and emu.poll() is None:
        scrape = subprocess.run(["kubectl", "get", "--raw", GATEWAY, "--request-timeout=10s"],
                                capture_output=True, text=True)
        values = {}
        for metric, labels, value in re.findall(r"^app_battery_(\w+)\{([^}\n]*)\}\s+([^\s]+)",
                                                scrape.stdout, re.MULTILINE):
            if f'device_id="{device}"' in labels.split(","):
                values[metric] = float(value)
        observed = values if scrape.returncode == 0 else scrape.stderr.strip()
        if observed != last:
            print(f"gateway: {observed}", flush=True)
            last = observed
        if scrape.returncode == 0 and values == expected:
            print(f"PASS {device}: {expected}", flush=True)
            return
        time.sleep(3)
    raise RuntimeError(f"expected {expected}, last gateway observation: {last}, emulator status: {emu.poll()}")


def run(args, work):
    fleet = f"battery-emu-{uuid4().hex[:12]}"
    registry = f"registry.{args.host_ip}.nip.io:5443"
    flash = work / "flash.bin"
    shutil.copyfile(args.firmware, flash)
    subprocess.run(["tools/provision.py", "--image", flash, "--secrets", args.credentials,
                    "--server", f"agent-api.{args.host_ip}.nip.io:8443", "--labels", f"app={fleet},site=emu",
                    "--wifi-ssid", "myssid", "--wifi-password", "mypassword"], check=True)
    subprocess.run(["wasm-apps/push.sh", "battery", "v1", fleet, registry], check=True,
                   env={**os.environ, "SIGNING_KEY": str((args.credentials / "signing.key").resolve(strict=True)),
                        "ORAS_ARGS": f"--annotation io.flightctl-mcu.e2e={fleet}"})
    wiring = work / "cosim.json"
    wiring.write_text(json.dumps({"parts": {"knob": {"module": str(args.potentiometer.resolve(strict=True)),
                                                      "pins": {"W": 3}}}}))
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    # SBS Read Word replies at 0x0b: 3700 mV, +14 mA and 62% relative charge.
    cmd = ["esp-emu", "--chip", "esp32c3", "--firmware", flash, "--net", "user",
           "--cosim-board", wiring, "--control-tcp", f"127.0.0.1:{port}",
           "--i2c-dev", "0:0x0b,width=1,fill=0,reg=0x09=0x74,reg=0x0a=0x0e,reg=0x0d=0x3e",
           "--timeout", "2400s"]
    device = None
    log = work / "emu.log"
    try:
        apply_fleet(fleet, args.host_ip, registry, ADC)
        with log.open("w") as console, subprocess.Popen(cmd, cwd=work, stdout=console,
                                                        stderr=subprocess.STDOUT) as emu:
            try:
                device = wait_device(emu, log)
                approve(emu, device, fleet)
                wait_metrics(emu, device, voltage_mv=3280, low_battery=1)
                send_input(port, "knob", "more")
                wait_metrics(emu, device, voltage_mv=3924, low_battery=0)
                send_input(port, "knob", "less")
                wait_metrics(emu, device, voltage_mv=3280, low_battery=1)
                apply_fleet(fleet, args.host_ip, registry, SBS)
                wait_metrics(emu, device, voltage_mv=3700, low_battery=0, charge_pct=62, charging=1)
            finally:
                emu.terminate()
                try:
                    emu.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    emu.kill()
    except Exception:
        if log.exists():
            print("\n".join(log.read_text(errors="replace").splitlines()[-40:]), flush=True)
        raise
    finally:
        resources = [f"fleet/{fleet}"]
        if device:
            resources.extend([f"device/{device}", f"enrollmentrequest/{device}"])
        for resource in resources:
            subprocess.run(["flightctl", "delete", resource], timeout=30)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("firmware", type=Path, help="CI's plain ESP32-C3 merged flash image")
    parser.add_argument("potentiometer", type=Path, help="potentiometer.wasm from the esp-emu release")
    parser.add_argument("--host-ip", required=True, help="kind host with the lab registry and gateway")
    parser.add_argument("--credentials", type=Path, default=Path("build/e2e-emu/secrets"),
                        help="enrollment and signing keys from the preceding emulator e2e")
    with tempfile.TemporaryDirectory() as temp:
        run(parser.parse_args(), Path(temp).resolve())
