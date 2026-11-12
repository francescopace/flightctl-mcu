#!/usr/bin/env python3
"""Writes a device's fc_creds partition: what it needs to reach Flight Control.

    tools/provision.py --port /dev/cu.usbmodem1101      write it to a board
    tools/provision.py --image flash.bin                put it into a merged flash image (emulator)
    tools/provision.py --out creds.bin                  just the partition
    tools/provision.py --format zephyr --image flash.bin    for the Zephyr build (port/zephyr/)
    tools/provision.py --format zephyr --board esp32s3_devkitc_procpu --port /dev/cu.usbmodem1101
                                                         a Zephyr build on an Espressif board

Contents (NVS namespace "fc"):
  ca, enroll_crt, enroll_key   from --secrets (default .secrets, see tools/enrollment.sh)
  signing_pub                  from --secrets/signing.pub, if there (tools/signing/keygen.sh);
                               without it the device does not check image signatures
  server_host, server_port     --server HOST[:PORT]; default: port/espidf/sdkconfig.local,
                               else agent-api.<this machine's LAN IP>.nip.io:8443
  labels                       --labels k=v,k=v; default: port/espidf/sdkconfig.local, else site=lab,app=alarm
  wifi_ssid, wifi_pass         --wifi-ssid / --wifi-password, or $WIFI_SSID / $WIFI_PASSWORD;
                               if not given, the device uses the WiFi it was built with
  prov_pop                     --pop SECRET (8+ characters): with no WiFi, the device opens a
                               provisioning access point where an installer sets it in the field
                               (esp_prov or Espressif's provisioning app); the secret proves
                               they hold the device. See docs/PROVISIONING.md.

--format zephyr writes the partition the Zephyr application reads instead: "FCC1", a length,
and a JSON object with the same fields (no WiFi yet). Its place in the flash comes from the
board's overlay, port/zephyr/boards/<--board>.overlay (default native_sim_native_64, whose flash
image is the file given to zephyr.exe --flash=).

The firmware is the same for every installation: change the credentials, not the build.
The partition is generated with ESP-IDF's NVS partition generator: the pip package
esp-idf-nvs-partition-gen when it is installed, else nvs_partition_gen.py in the espressif/idf
image ($IDF_IMAGE, default espressif/idf:v6.0.3). It holds the enrollment key: treat the output
like the key itself.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
IDF_IMAGE = os.environ.get("IDF_IMAGE", "espressif/idf:v6.0.3")


def partition(path=os.path.join(ROOT, "port", "espidf", "partitions.csv")):
    """Offset and size of fc_creds in port/espidf/partitions.csv."""
    with open(path) as f:
        for line in f:
            cols = [c.strip() for c in line.split("#")[0].split(",")]
            if cols[0] == "fc_creds":
                return int(cols[3], 0), int(cols[4], 0)
    sys.exit("no fc_creds partition in port/espidf/partitions.csv")


def sdkconfig_local(key, path=os.path.join(ROOT, "port", "espidf", "sdkconfig.local")):
    if not os.path.exists(path):
        return None
    with open(path) as f:
        for line in f:
            m = re.match(rf'CONFIG_{key}=("?)(.*)\1\s*$', line.strip())
            if m:
                return m.group(2)
    return None


def local_ip():
    out = subprocess.run(["bash", "-c", f"source '{ROOT}/deploy/local-ip.sh'; local_ip"],
                         capture_output=True, text=True)
    return out.stdout.strip()


def csv_rows(keys, port, base):
    """nvs_partition_gen CSV: namespace fc, one string entry per file in
    `base` (named after its key), and the port as an integer."""
    rows = ["key,type,encoding,value", "fc,namespace,,"]
    rows += [f"{key},file,string,{base}/{key}" for key in keys]
    rows.append(f"server_port,data,i32,{int(port)}")
    return rows


def build_partition(args, size):
    secrets = os.path.abspath(args.secrets)
    for name in ("ca.crt", "enroll.crt", "enroll.key"):
        if not os.path.exists(os.path.join(secrets, name)):
            sys.exit(f"{secrets}/{name} missing (tools/enrollment.sh creates it)")
    host, port = args.server.rsplit(":", 1) if ":" in args.server else (args.server, "8443")

    # Inside the repo, so the ESP-IDF container sees it; removed afterwards.
    os.makedirs(os.path.join(ROOT, "build"), exist_ok=True)
    work = tempfile.mkdtemp(prefix="provision-", dir=os.path.join(ROOT, "build"))
    try:
        entries = [("ca", "ca.crt"), ("enroll_crt", "enroll.crt"), ("enroll_key", "enroll.key")]
        for key, name in entries:
            shutil.copy(os.path.join(secrets, name), os.path.join(work, key))
        if os.path.exists(os.path.join(secrets, "signing.pub")):
            shutil.copy(os.path.join(secrets, "signing.pub"), os.path.join(work, "signing_pub"))
            entries.append(("signing_pub", None))
        else:
            print("warning: no signing.pub, the device will not check image signatures", file=sys.stderr)
        # Strings go in files too, so commas and quotes need no CSV escaping.
        values = {"server_host": host, "labels": args.labels}
        if args.wifi_ssid:
            values["wifi_ssid"] = args.wifi_ssid
            values["wifi_pass"] = args.wifi_password or ""
        if args.pop:
            values["prov_pop"] = args.pop
        for key, value in values.items():
            with open(os.path.join(work, key), "w") as f:
                f.write(value)
        local = subprocess.run([sys.executable, "-c", "import esp_idf_nvs_partition_gen"],
                               capture_output=True).returncode == 0
        keys = [key for key, _ in entries] + [key for key in values if values[key] != ""]  # no password: open AP
        rows = csv_rows(keys, port, work if local else "/w")
        with open(os.path.join(work, "creds.csv"), "w") as f:
            f.write("\n".join(rows) + "\n")
        if local:
            gen = subprocess.run([sys.executable, "-m", "esp_idf_nvs_partition_gen", "generate",
                                  f"{work}/creds.csv", f"{work}/creds.bin", f"{size:#x}"],
                                 capture_output=True, text=True)
        else:
            gen = subprocess.run(["docker", "run", "--rm", "-v", f"{work}:/w", IDF_IMAGE, "bash", "-c",
                                  "python $IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py "
                                  f"generate /w/creds.csv /w/creds.bin {size:#x}"],
                                 capture_output=True, text=True)
        if gen.returncode != 0:
            sys.exit(f"nvs_partition_gen failed:\n{gen.stdout}{gen.stderr}")
        with open(os.path.join(work, "creds.bin"), "rb") as f:
            data = f.read()
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print(f"fc_creds: {host}:{port}, labels {args.labels}, "
          f"signing key {'yes' if any(k == 'signing_pub' for k, _ in entries) else 'no'}, "
          f"WiFi {'provisioned' if args.wifi_ssid else 'set in the field' if args.pop else 'from the build'}")
    return data


def zephyr_partition(board):
    """Offset and size of the fc_creds partition in the board's overlay."""
    path = os.path.join(ROOT, "port", "zephyr", "boards", f"{board}.overlay")
    if not os.path.exists(path):
        sys.exit(f"no {path}")
    with open(path) as f:
        m = re.search(r'label\s*=\s*"fc_creds";\s*reg\s*=\s*<\s*(\w+)\s+(\w+)\s*>', f.read())
    if not m:
        sys.exit(f"no fc_creds partition in {path}")
    return int(m.group(1), 0), int(m.group(2), 0)


def build_zephyr_partition(args, size):
    """ "FCC1", the JSON's length (32 bits, little-endian), the JSON (port/zephyr/src/main.c)."""
    secrets = os.path.abspath(args.secrets)
    creds = {}
    for key, name in (("ca", "ca.crt"), ("enroll_crt", "enroll.crt"), ("enroll_key", "enroll.key")):
        if not os.path.exists(os.path.join(secrets, name)):
            sys.exit(f"{secrets}/{name} missing (tools/enrollment.sh creates it)")
        with open(os.path.join(secrets, name)) as f:
            creds[key] = f.read()
    if os.path.exists(os.path.join(secrets, "signing.pub")):
        with open(os.path.join(secrets, "signing.pub")) as f:
            creds["signing_pub"] = f.read()
    else:
        print("warning: no signing.pub, the device will not check image signatures", file=sys.stderr)
    host, port = args.server.rsplit(":", 1) if ":" in args.server else (args.server, "8443")
    creds["server_host"] = host
    creds["server_port"] = int(port)
    creds["labels"] = dict(kv.split("=", 1) for kv in args.labels.split(",") if "=" in kv)
    # Boards with WiFi (native_sim uses the host's network): the network given
    # here, else the one in port/espidf/sdkconfig.wifi, your desk's.
    ssid, password, where = args.wifi_ssid, args.wifi_password, "given"
    if not ssid and not args.board.startswith("native_sim"):
        wifi = os.path.join(ROOT, "port", "espidf", "sdkconfig.wifi")
        ssid, password, where = sdkconfig_local("FC_WIFI_SSID", wifi), sdkconfig_local("FC_WIFI_PASSWORD", wifi), wifi
    if ssid:
        creds["wifi_ssid"] = ssid
        creds["wifi_pass"] = password or ""
        print(f"WiFi network: {where if where != 'given' else 'from the command line'}")
    body = json.dumps(creds, separators=(",", ":")).encode()
    data = b"FCC1" + len(body).to_bytes(4, "little") + body
    if len(data) > size:
        sys.exit(f"credentials are {len(data)} bytes, the partition holds {size}")
    print(f"fc_creds (zephyr): {host}:{port}, labels {args.labels}, "
          f"signing key {'yes' if 'signing_pub' in creds else 'no'}")
    return data + b"\xff" * (size - len(data))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--format", choices=("esp-idf", "zephyr"), default="esp-idf")
    p.add_argument("--board", default="native_sim_native_64", help="Zephyr board, as in port/zephyr/boards/")
    p.add_argument("--secrets", default=os.path.join(ROOT, ".secrets"))
    p.add_argument("--server")
    p.add_argument("--labels")
    p.add_argument("--wifi-ssid", default=os.environ.get("WIFI_SSID"))
    p.add_argument("--wifi-password", default=os.environ.get("WIFI_PASSWORD"))
    p.add_argument("--pop", default=os.environ.get("PROV_POP"),
                   help="proof of possession for provisioning in the field (8+ characters)")
    dest = p.add_mutually_exclusive_group(required=True)
    dest.add_argument("--port", help="serial port of the board to write")
    dest.add_argument("--image", help="merged flash image to patch")
    dest.add_argument("--out", help="file to write the partition to")
    args = p.parse_args()
    if not args.server:
        host = sdkconfig_local("FC_SERVER_HOST") or f"agent-api.{local_ip()}.nip.io"
        args.server = f"{host}:{sdkconfig_local('FC_SERVER_PORT') or 8443}"
    if not args.labels:
        args.labels = sdkconfig_local("FC_LABELS") or "site=lab,app=alarm"
    if args.pop is not None and not 8 <= len(args.pop) <= 63:
        p.error("--pop: 8 to 63 characters (it is also the provisioning access point's password)")

    if args.format == "zephyr":
        if args.port and not args.board.startswith("esp32"):
            p.error("--format zephyr --port: Espressif boards only (esptool); for the others --image or --out")
        offset, size = zephyr_partition(args.board)
        data = build_zephyr_partition(args, size)
    else:
        offset, size = partition()
        data = build_partition(args, size)
    if args.out:
        with open(args.out, "wb") as f:
            f.write(data)
    elif args.image:
        with open(args.image, "r+b") as f:
            f.seek(0, os.SEEK_END)
            if f.tell() < offset:
                f.write(b"\xff" * (offset - f.tell()))
            f.seek(offset)
            f.write(data)
        print(f"written into {args.image} at {offset:#x}")
    else:
        with tempfile.NamedTemporaryFile(suffix=".bin") as f:
            f.write(data)
            f.flush()
            subprocess.run(["esptool", "-p", args.port, "--before", "default-reset", "--after", "watchdog-reset",
                            "write-flash", f"{offset:#x}", f.name], check=True)


if __name__ == "__main__":
    main()
