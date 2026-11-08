"""Tests for the deploy scripts' logic (no registry, cluster or board needed).

    python3 -m unittest discover tests/python
"""
import base64
import hashlib
import importlib.util
import json
import os
import subprocess
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, path))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


sign = load("sign", "deploy/signing/sign.py")
provision = load("provision", "deploy/provision.py")
approve = load("auto_approve", "deploy/auto-approve/auto-approve.py")


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
        self.assertEqual(provision.partition(), (0x12000, 0x6000))

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
