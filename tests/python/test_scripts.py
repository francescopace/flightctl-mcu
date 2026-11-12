"""Tests for the tools' logic (no registry, cluster or board needed).

    python3 -m unittest discover tests/python
"""
import base64
import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, path))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


sign = load("sign", "tools/signing/sign.py")
provision = load("provision", "tools/provision.py")
soak = load("soak", "tools/soak/collect.py")
approve = load("auto_approve", "tools/auto-approve/auto-approve.py")


class SignTest(unittest.TestCase):
    def test_references(self):
        self.assertEqual(sign.split_ref("reg:5001/team/app:v1"), ("reg:5001", "team/app", "v1"))
        self.assertEqual(sign.split_ref("reg:5001/app@sha256:ab"), ("reg:5001", "app", "sha256:ab"))
        self.assertEqual(sign.split_ref("reg/app"), ("reg", "app", "latest"))

    def test_payload_names_the_digest(self):
        p = json.loads(sign.payload_for("reg:5001", "app", "sha256:" + "a" * 64))
        self.assertEqual(p["critical"]["image"]["docker-manifest-digest"], "sha256:" + "a" * 64)
        self.assertEqual(p["critical"]["identity"]["docker-reference"], "reg:5001/app")
        self.assertEqual(p["critical"]["type"], "cosign container image signature")

    def test_signature_verifies_with_the_public_key(self):
        with tempfile.TemporaryDirectory() as d:
            key, pub = os.path.join(d, "k"), os.path.join(d, "p")
            subprocess.run(f"openssl ecparam -name prime256v1 -genkey -noout | openssl pkcs8 -topk8 -nocrypt -out {key}",
                           shell=True, check=True, capture_output=True)
            subprocess.run(["openssl", "ec", "-in", key, "-pubout", "-out", pub], check=True, capture_output=True)
            payload = sign.payload_for("reg", "app", "sha256:" + "b" * 64)
            der = sign.der_signature(payload, key)
            manifest = sign.signature_manifest(payload, der, sign.config_for(sign.sha(payload)))
            layer = manifest["layers"][0]
            self.assertEqual(layer["digest"], "sha256:" + hashlib.sha256(payload).hexdigest())
            self.assertEqual(layer["size"], len(payload))
            sig = base64.b64decode(layer["annotations"][sign.SIG_ANNOTATION])
            with open(os.path.join(d, "payload"), "wb") as f:
                f.write(payload)
            with open(os.path.join(d, "sig"), "wb") as f:
                f.write(sig)
            ok = subprocess.run(["openssl", "dgst", "-sha256", "-verify", pub, "-signature",
                                 os.path.join(d, "sig"), os.path.join(d, "payload")], capture_output=True)
            self.assertEqual(ok.returncode, 0, ok.stdout + ok.stderr)

    def test_signature_tag(self):
        self.assertEqual(sign.signature_tag("sha256:abc"), "sha256-abc.sig")


class ProvisionTest(unittest.TestCase):
    def test_partition_from_the_table(self):
        self.assertEqual(provision.partition(), (0x1a000, 0x6000))

    def test_csv_rows(self):
        rows = provision.csv_rows(["ca", "labels"], "8443", "/w")
        self.assertEqual(rows[:2], ["key,type,encoding,value", "fc,namespace,,"])
        self.assertIn("ca,file,string,/w/ca", rows)
        self.assertIn("labels,file,string,/w/labels", rows)
        self.assertEqual(rows[-1], "server_port,data,i32,8443")

    def test_sdkconfig_local_values(self):
        with tempfile.NamedTemporaryFile("w", suffix=".local", delete=False) as f:
            f.write('CONFIG_FC_SERVER_HOST="agent-api.example"\nCONFIG_FC_SERVER_PORT=9443\n# CONFIG_X is not set\n')
        try:
            self.assertEqual(provision.sdkconfig_local("FC_SERVER_HOST", f.name), "agent-api.example")
            self.assertEqual(provision.sdkconfig_local("FC_SERVER_PORT", f.name), "9443")
            self.assertIsNone(provision.sdkconfig_local("FC_LABELS", f.name))
        finally:
            os.unlink(f.name)
        self.assertIsNone(provision.sdkconfig_local("FC_SERVER_HOST", "/nonexistent"))

    def test_pop_length(self):
        # It is also the provisioning access point's WPA2 password: 8 to 63.
        for pop, code in (("short", 2), ("x" * 64, 2)):
            r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "provision.py"), "--out", os.devnull,
                                "--server", "h:1", "--labels", "a=b", "--pop", pop],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, code, r.stderr)
            self.assertIn("--pop", r.stderr)


class SoakTest(unittest.TestCase):
    @staticmethod
    def device(name, status, uptime, mem_min, tx, reason="poweron"):
        return {"metadata": {"name": name},
                "status": {"summary": {"status": status}, "updated": {"status": "UpToDate"},
                           "systemInfo": {"customInfo": {"uptimeSeconds": str(uptime), "memMinFreeBytes": str(mem_min),
                                                         "netTxBytes": str(tx), "resetReason": reason}}}}

    def test_report(self):
        samples = [  # one device sampled hourly: a restart (task_wdt) between the 2nd and 3rd sample
            ("2026-11-01T00:00:00Z", self.device("d1", "Online", 100, 50000, 1000)),
            ("2026-11-01T01:00:00Z", self.device("d1", "Online", 3700, 49000, 3000)),
            ("2026-11-01T02:00:00Z", self.device("d1", "Unknown", 50, 51000, 500, "task_wdt")),
            ("2026-11-01T03:00:00Z", self.device("d1", "Online", 3650, 48000, 2500, "task_wdt")),
        ]
        with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False, newline="") as f:
            w = soak.csv.DictWriter(f, soak.FIELDS)
            w.writeheader()
            for t, d in samples:
                for row in soak.rows({"items": [d]}, t):
                    w.writerow(row)
        try:
            [r] = soak.report(f.name)
        finally:
            os.unlink(f.name)
        self.assertEqual(r["hours"], 3.0)
        self.assertEqual(r["notOnline"], "25.0%")
        self.assertEqual(r["restarts"], 1)
        self.assertEqual(r["reasons"], {"task_wdt": 1})
        self.assertEqual(r["memMinFree"], (50000, 48000, 48000))
        self.assertEqual(r["txBytesPerHour"], (2000 + 2000) // 3)  # the drop at the restart is not counted


class AutoApproveTest(unittest.TestCase):
    def allow(self, text):
        with tempfile.NamedTemporaryFile("w", suffix=".yaml", delete=False) as f:
            f.write(text)
        try:
            return approve.load(f.name)
        finally:
            os.unlink(f.name)

    @staticmethod
    def request(name, mac=None):
        info = {"customInfo": {"mac": mac}} if mac else {}
        return {"metadata": {"name": name}, "spec": {"deviceStatus": {"systemInfo": info}}}

    def test_by_name_and_by_mac_with_defaults(self):
        allow = self.allow(
            "defaults:\n  labels:\n    site: lab\n    app: alarm\n"
            "devices:\n"
            "  - name: dev1\n    labels:\n      stage: canary\n"
            "  - mac: \"AC:EB:E6:4A:E7:08\"\n    labels:\n      stage: prod\n      app: other\n")
        self.assertEqual(approve.labels_for(self.request("dev1"), allow),
                         {"site": "lab", "app": "alarm", "stage": "canary"})
        # MACs match case-insensitively; device labels override the defaults.
        self.assertEqual(approve.labels_for(self.request("x", "ac:eb:e6:4a:e7:08"), allow),
                         {"site": "lab", "app": "other", "stage": "prod"})
        self.assertIsNone(approve.labels_for(self.request("stranger", "00:11:22:33:44:55"), allow))
        self.assertIsNone(approve.labels_for(self.request("no-mac"), allow))

    def test_empty_list(self):
        allow = self.allow("")
        self.assertIsNone(approve.labels_for(self.request("dev1"), allow))


if __name__ == "__main__":
    unittest.main()
