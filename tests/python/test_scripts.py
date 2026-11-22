"""Tests for the tools' logic (no registry, cluster or board needed).

    python3 -m unittest discover tests/python
"""
import base64
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

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
registry = load("registry_client", "tools/registry/registry_client.py")
sbom = load("sbom", "tools/sbom.py")
advisories = load("advisories", "tools/advisories.py")


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


class C5SimpleBootTest(unittest.TestCase):
    def test_rom_digest_preserves_xip_offset(self):
        image_tool = load("c5_image", "port/nuttx/c5/image.py")
        image = bytearray(48)
        image[0:2] = bytes((0xe9, 1))
        image[12] = 23
        struct.pack_into("<II", image, 24, 0x40800000, 4)
        image[47] = 0xef
        image += struct.pack("<II", 0, 64) + bytes(64)
        xip = struct.pack("<II", 0x42000000, 4) + b"code"
        image += xip
        result = image_tool.add_digest(image)
        self.assertEqual(result[23], 1)
        self.assertEqual(result[48:80], hashlib.sha256(result[:48]).digest())
        self.assertEqual(struct.unpack_from("<II", result, 80), (0, 32))
        self.assertEqual(result[120:], xip)
        self.assertEqual(image_tool.add_digest(bytearray(result)), result)


class ProvisionTest(unittest.TestCase):
    def test_partition_from_the_table(self):
        self.assertEqual(provision.partition(), (0x1a000, 0x6000))

    def test_c5_factory_partition_matches_flash_capacity(self):
        self.assertEqual(provision.zephyr_partition("esp32c5_mini"), (0x3b0000, 0x4000))
        self.assertEqual(provision.zephyr_partition("esp32c5_devkitc_hpcore"), (0x7b0000, 0x4000))

    def test_nuttx_factory_layout(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d, "image")
            for name in ("ca.crt", "enroll.crt", "enroll.key", "signing.pub"):
                Path(d, name).write_text(name)
            for board in (None, "esp32-devkitc", "esp32-devkitc-rev1", "esp32c3-devkit", "esp32c5-devkitc", "esp32c6-devkitc",
                          "esp32s3-n4r2", "esp32s3-n8r8", "esp32s3-n16r8", "pico2w", "nucleo-f767zi", "nucleo-f767zi-eth", "nrf7002dk"):
                board_args = ["--board", board] if board else []
                wifi_args = [] if board in ("nucleo-f767zi", "nucleo-f767zi-eth", "nrf7002dk") else ["--wifi-ssid", "test"]
                with self.subTest(board=board_args):
                    out.write_bytes(b"firmware")
                    subprocess.run([sys.executable, provision.__file__, "--format", "nuttx",
                                    *board_args, "--server", "agent.example:9443", "--labels", "site=test",
                                    *wifi_args, "--secrets", d, "--image", str(out)],
                                   capture_output=True, check=True)
                    image = out.read_bytes()
                    offset = {"pico2w": 0x300000, "nucleo-f767zi": 0x20000,
                              "nucleo-f767zi-eth": 0x20000, "nrf7002dk": 0xfc000}.get(board, 0x2fc000)
                    self.assertEqual(len(image), offset + 0x4000)
                    blob = image[offset:]
                    self.assertEqual(blob[:4], b"FCC1")
                    n = int.from_bytes(blob[4:8], "little")
                    creds = json.loads(blob[8:8+n])
                    self.assertEqual(creds["signing_pub"], "signing.pub")
                    self.assertEqual(creds["server_port"], 9443)
                    if not wifi_args:
                        self.assertNotIn("wifi_ssid", creds)
                        self.assertNotIn("wifi_pass", creds)
                        if board in ("nucleo-f767zi", "nucleo-f767zi-eth"):
                            self.assertEqual(creds["storage_format"], "nuttx-nvs")
                    else:
                        self.assertEqual(creds["wifi_ssid"], "test")
                    self.assertEqual(blob[8+n:], b"\xff" * (len(blob) - 8 - n))

    def test_partition_and_image_are_private_when_replacing_a_shared_file(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d, "flash.bin")
            for offset, expected in ((None, b"abcd"), (2, b"01abcd67"), (10, b"01234567\xff\xffabcd")):
                with self.subTest(offset=offset):
                    path.write_bytes(b"01234567")
                    path.chmod(0o644)
                    with path.open("rb") as old:
                        provision.write_provisioned_file(path, b"abcd", offset)
                        self.assertEqual(old.read(), b"01234567")
                    self.assertEqual(path.stat().st_mode & 0o777, 0o600)
                    self.assertEqual(path.read_bytes(), expected)

    def test_failed_image_write_preserves_original_and_removes_temp_file(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d, "image")
            path.write_bytes(b"original")
            with patch.object(provision.os, "replace", side_effect=OSError), self.assertRaises(OSError):
                provision.write_provisioned_file(path, b"new", 2)
            self.assertEqual(path.read_bytes(), b"original")
            self.assertEqual(list(Path(d).iterdir()), [path])

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


class RegistryTest(unittest.TestCase):
    """The tools follow the devices' rule: TLS when there is a CA, credentials
    by registry and repository."""

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.saved = {k: os.environ.get(k) for k in ("REGISTRY_CA", "REGISTRY_AUTH_FILE")}
        for k in self.saved:
            os.environ.pop(k, None)
        registry.SECRETS = self.dir.name
        with open(os.path.join(self.dir.name, "host"), "w") as f:
            f.write("reg.example:5443\n")
        with open(os.path.join(self.dir.name, "ca.crt"), "w") as f:
            f.write("CA")
        with open(os.path.join(self.dir.name, "auth.json"), "w") as f:
            json.dump({"auths": {"reg.example:5443": {"auth": "dTpw"},
                                 "https://reg.example:5443/team": {"username": "t", "password": "q"}}}, f)

    def tearDown(self):
        for k, v in self.saved.items():
            if v is not None:
                os.environ[k] = v
        self.dir.cleanup()

    def test_tls_only_for_the_registry_with_a_ca(self):
        self.assertEqual(registry.base_url("reg.example:5443"), "https://reg.example:5443")
        self.assertEqual(registry.base_url("10.0.0.1:5001"), "http://10.0.0.1:5001")
        self.assertEqual(registry.oras_args("10.0.0.1:5001"), ["--plain-http"])
        self.assertEqual(registry.oras_args("reg.example:5443")[:2], ["--ca-file", os.path.join(self.dir.name, "ca.crt")])

    def test_credentials_by_registry_and_repository(self):
        self.assertEqual(registry.basic_for("reg.example:5443", "flightctl-mcu/fw"), "dTpw")
        self.assertEqual(base64.b64decode(registry.basic_for("reg.example:5443", "team/app")), b"t:q")
        self.assertEqual(registry.basic_for("reg.example:5443", "teams/app"), "dTpw")
        self.assertIsNone(registry.basic_for("other:5443", "app"))

    def test_oras_cli_prints_only_flags_and_paths(self):
        directory = Path(self.dir.name)
        result = subprocess.run([sys.executable, registry.__file__, "oras-args", "reg.example:5443"],
                                env={**os.environ, "REGISTRY_SECRETS": str(directory)},
                                check=True, capture_output=True, text=True)
        self.assertEqual(result.stdout, f"--ca-file {directory}/ca.crt --registry-config {directory}/auth.json\n")
        self.assertEqual(result.stderr, "")

    def test_challenges(self):
        scheme, params = registry._challenge('Bearer realm="https://r/token",service="r",scope="repository:a:pull,push"')
        self.assertEqual(scheme, "bearer")
        self.assertEqual(params["scope"], "repository:a:pull,push")
        self.assertEqual(registry._challenge('Basic realm="x"')[0], "basic")


class RegistryConfigTest(unittest.TestCase):
    def test_ca_and_credentials_go_into_the_fleet(self):
        mod = load("registry_config", "tools/fleet/registry-config.py")
        with tempfile.TemporaryDirectory() as d:
            for name, text in (("host", "reg.example:5443"), ("ca.crt", "CA"), ("auth.json", '{"auths":{}}')):
                with open(os.path.join(d, name), "w") as f:
                    f.write(text)
            mod.registry_client.SECRETS = d
            files = mod.files_for("reg.example:5443")
            self.assertEqual([f["path"] for f in files],
                             ["/etc/containers/certs.d/reg.example/ca.crt", "/etc/containers/auth.json"])
            self.assertTrue(all(":" not in f["path"] for f in files))  # Flight Control refuses ':' there
            self.assertEqual(mod.files_for("10.0.0.1:5001"), [])


class GitExportTest(unittest.TestCase):
    FLEET = """apiVersion: flightctl.io/v1beta1
kind: Fleet
metadata:
  name: f
spec:
  selector:
    matchLabels: {app: a}
  template:
    spec:
      os:
        image: ${REGISTRY}/fw:v1
      config:
        - name: app
          inline:
            - path: /etc/a.json
              content: '{"x": {{ getOrDefault .metadata.labels "x" "1" }}}'
        - name: registry
          inline: []
"""

    def test_registry_spelled_out_and_secrets_mounted(self):
        import yaml
        mod = load("git_export", "tools/fleet/git-export.py")
        with tempfile.TemporaryDirectory() as d:
            for name, text in (("host", "reg.example:5443"), ("ca.crt", "CA"), ("auth.json", "{}")):
                with open(os.path.join(d, name), "w") as f:
                    f.write(text)
            mod.registry_client.SECRETS = d
            os.environ.pop("REGISTRY_AUTH_FILE", None)
            fleet = yaml.safe_load(mod.export(self.FLEET, "reg.example:5443"))
            spec = fleet["spec"]["template"]["spec"]
            self.assertEqual(spec["os"]["image"], "reg.example:5443/fw:v1")
            self.assertEqual([c["name"] for c in spec["config"]], ["app", "registry-ca", "registry-auth"])
            self.assertIn("{{ getOrDefault", spec["config"][0]["inline"][0]["content"])
            self.assertEqual(spec["config"][1]["secretRef"]["mountPath"], "/etc/containers/certs.d/reg.example")
            self.assertEqual(spec["config"][2]["secretRef"]["mountPath"], "/etc/containers")
            # plain HTTP: nothing mounted
            plain = yaml.safe_load(mod.export(self.FLEET, "10.0.0.1:5001"))
            self.assertEqual([c["name"] for c in plain["spec"]["template"]["spec"]["config"]], ["app"])


class SbomTest(unittest.TestCase):
    DIGEST = "sha256:" + "ab" * 32

    @staticmethod
    def app_header(version):
        # ESP image and first segment headers, followed by esp_app_desc_t.
        header = bytearray(32 + 256)
        header[0] = 0xe9
        header[32:36] = bytes.fromhex("3254cdab")
        header[144:144 + len(version)] = version.encode("ascii")
        return header

    def test_root_is_the_digest_flight_control_looks_up(self):
        doc = sbom.make_sbom("reg:5443/flightctl-mcu/fw:v9", self.DIGEST)
        root = doc["metadata"]["component"]
        # Flight Control finds the package URL that holds the device's image digest.
        self.assertIn("ab" * 32, root["purl"])
        self.assertTrue(root["purl"].startswith("pkg:oci/fw@sha256%3A"))
        self.assertEqual(doc["dependencies"][0]["ref"], root["bom-ref"])
        self.assertEqual(len(doc["dependencies"][0]["dependsOn"]), len(doc["components"]))

    def test_esp_idf_components_from_the_lock_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / "app.bin"
            image.write_bytes(self.app_header("v6.1.1"))
            doc = sbom.make_sbom("reg/fw:v9", self.DIGEST, [f"esp32c3={image}", f"esp32s3-r8={image}"])
        by_name = {}
        for c in doc["components"]:
            by_name.setdefault(c["name"], []).append(c)
        self.assertEqual([c["version"] for c in by_name["esp-idf"]], ["6.1.1"])  # once for both chips
        self.assertEqual(by_name["mbedtls"][0]["cpe"], "cpe:2.3:a:trustedfirmware:mbed_tls:4.1.1:*:*:*:*:*:*:*")
        self.assertNotIn("network_provisioning", by_name)  # fc_prov.c instead
        for name in ("flightctl-mcu", "cJSON", "wasm-micro-runtime"):
            self.assertIn(name, by_name)
        refs = [c["bom-ref"] for c in doc["components"]]
        self.assertEqual(len(refs), len(set(refs)))

    def test_rejects_invalid_app_descriptor(self):
        header = self.app_header("v6.1.1")
        for invalid in (header[:145], bytes(len(header)), self.app_header("")):
            with self.subTest(header=invalid), self.assertRaisesRegex(SystemExit, "app.bin"):
                sbom.image_idf_version(invalid, "app.bin")

    def test_nuttx_release_accepts_only_the_compact_image_for_its_board(self):
        with patch.dict(sys.modules, sbom=sbom):
            push = load("fw_push", "tools/fw-push.py")
        platform = "nuttx-esp32s3-n16r8"
        board = platform.encode()
        header = bytearray(32)
        struct.pack_into("<IIHHI", header, 0, 0x96f3b83d, 0, len(header), 8 + len(board), 4)
        tag = struct.pack("<HHHH", 0x6908, 8 + len(board), 0xa0, len(board)) + board
        image = header + b"body" + tag + struct.pack("<HHHH", 0x6907, 40, 0x10, 32) + bytes(32)
        push.validate_nuttx_image(platform, image, "firmware.bin")
        for target, data in ((platform, image[:-1]), (platform, image + bytes(4096)),
                             ("nuttx-esp32s3-n8r8", image)):
            with self.subTest(target=target, size=len(data)), self.assertRaises(SystemExit):
                push.validate_nuttx_image(target, data, "firmware.bin")

    def test_nuttx_sbom_lists_dependencies_of_the_selected_hardware(self):
        expected = {
            "nuttx-nucleo-f767zi": set(),
            "nuttx-nrf7002dk": {"littlefs", "open-amp", "libmetal"},
            "nuttx-pico2w": {"littlefs", "CYW43439 Wi-Fi firmware"},
            "nuttx-esp32s3-n16r8": {"littlefs", "esp-hal-3rdparty", "mcuboot"},
        }
        board_packages = set.union(*expected.values())
        for board, names in expected.items():
            with self.subTest(board=board):
                _, packages = sbom.release_inventory([], [], [board])
                self.assertEqual({p["name"] for p in packages} & board_packages, names)

    def test_arm_release_requires_board_and_counter_in_the_signed_region(self):
        with patch.dict(sys.modules, sbom=sbom):
            push = load("fw_push", "tools/fw-push.py")
        for board, header_size in (("nuttx-pico2w", 512), ("nuttx-nrf7002dk", 512),
                                   ("nuttx-nucleo-f767zi", 1024), ("nuttx-nucleo-f767zi-eth", 1024)):
            name = board.encode()
            protected = struct.pack("<HHHHIHH", 0x6908, 16 + len(name), 0x50, 4, 0, 0xa0, len(name)) + name
            header = bytearray(header_size)
            struct.pack_into("<IIHHI", header, 0, 0x96f3b83d, 0, header_size, len(protected), 4)
            image = header + b"body" + protected + struct.pack("<HH", 0x6907, 8) + b"test"
            with self.subTest(board=board):
                push.validate_nuttx_image(board, image, "ota.bin")
                for invalid in (image + bytes(256), image.replace(name, b"?" * len(name)),
                                image[:header_size + 8] + bytes(4) + image[header_size + 12:]):
                    with self.assertRaises(SystemExit):
                        push.validate_nuttx_image(board, invalid, "ota.bin")

    def test_fw_push_checks_all_platforms_before_contacting_registry(self):
        with patch.dict(sys.modules, sbom=sbom):
            push = load("fw_push", "tools/fw-push.py")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for chip, version in (("esp32s3", "v6.1.1"), ("esp32c3", "v6.0.3")):
                build = root / "port" / "espidf" / f"build-{chip}"
                build.mkdir(parents=True)
                (build / "flightctl_mcu_fw.bin").write_bytes(self.app_header(version))
            push.__file__ = str(root / "tools" / "fw-push.py")
            cases = [
                ("esp32c3", "ESP-IDF 6.0.3, lock file specifies 6.1.1"),
                (f"new_board={build}/flightctl_mcu_fw.bin", "Add it to ZEPHYR_BOARD_MODULES"),
            ]
            for platform, error in cases:
                with self.subTest(platform=platform), patch.object(push.registry_client, "base_url") as registry:
                    with patch.object(sys, "argv", ["fw-push.py", "reg/fw:v1", "esp32s3", platform]):
                        with self.assertRaisesRegex(SystemExit, error):
                            push.main()
                    registry.assert_not_called()

    @unittest.skipUnless(os.path.isdir(os.path.join(ROOT, "third_party", "zephyr-ws", "zephyr")),
                         "no Zephyr tree (third_party/fetch-zephyr.sh)")
    def test_zephyr_components_from_the_tree(self):
        doc = sbom.make_sbom("reg/fw:z9", self.DIGEST, [], ["esp32s3_devkitc"])
        names = [c["name"] for c in doc["components"]]
        for name in ("zephyr", "mbedtls", "mcuboot", "littlefs", "hal_espressif"):
            self.assertIn(name, names)
        refs = [c["bom-ref"] for c in doc["components"]]
        self.assertEqual(len(refs), len(set(refs)))


class AdvisoriesTest(unittest.TestCase):
    MBEDTLS = [("a", "trustedfirmware", "mbed_tls"), ("a", "arm", "mbed_tls")]

    def cve(self, *matches):
        return {"configurations": [{"nodes": [{"cpeMatch": list(matches)}]}]}

    def test_version_ranges_as_nvd_writes_them(self):
        m = {"vulnerable": True, "criteria": "cpe:2.3:a:trustedfirmware:mbed_tls:*:*:*:*:*:*:*:*",
             "versionStartIncluding": "4.0.0", "versionEndExcluding": "4.1.0"}
        self.assertTrue(advisories.affects(self.cve(m), self.MBEDTLS, "4.0.0"))
        self.assertFalse(advisories.affects(self.cve(m), self.MBEDTLS, "4.1.0"))
        self.assertFalse(advisories.affects(self.cve(m), self.MBEDTLS, "4.1.1"))
        self.assertFalse(advisories.affects(self.cve(m), self.MBEDTLS, "3.6.7"))
        # 3.10 is after 3.9, not before it
        old = {"vulnerable": True, "criteria": "cpe:2.3:a:arm:mbed_tls:*:*:*:*:*:*:*:*", "versionEndIncluding": "3.9"}
        self.assertFalse(advisories.affects(self.cve(old), self.MBEDTLS, "3.10.0"))

    def test_exact_versions_other_products_and_non_vulnerable_entries(self):
        exact = {"vulnerable": True, "criteria": "cpe:2.3:a:arm:mbed_tls:3.6.7:*:*:*:*:*:*:*"}
        self.assertTrue(advisories.affects(self.cve(exact), self.MBEDTLS, "3.6.7"))
        self.assertFalse(advisories.affects(self.cve(exact), self.MBEDTLS, "3.6.8"))
        other = {"vulnerable": True, "criteria": "cpe:2.3:a:other:mbed_tls:*:*:*:*:*:*:*:*"}
        self.assertFalse(advisories.affects(self.cve(other), self.MBEDTLS, "3.6.7"))
        platform = dict(exact, vulnerable=False)
        self.assertFalse(advisories.affects(self.cve(platform), self.MBEDTLS, "3.6.7"))

    def test_cves_that_do_not_apply_are_left_out(self):
        entries = advisories.load_not_affected()
        for cid, e in entries.items():
            self.assertRegex(cid, r"^CVE-\d{4}-\d+$")
            self.assertIn(e["justification"], ("code_not_present", "code_not_reachable", "requires_configuration",
                                               "requires_dependency", "requires_environment", "protected_by_compiler",
                                               "protected_at_runtime", "protected_at_perimeter",
                                               "protected_by_mitigating_control"))
            self.assertTrue(e["detail"] and e["component"])

    def test_osv_record_names_the_shipped_versions_by_package_url(self):
        cve = {"id": "CVE-2026-1", "published": "2026-01-01T00:00:00.000", "lastModified": "2026-02-01T00:00:00.000",
               "descriptions": [{"lang": "en", "value": "A flaw. More."}],
               "metrics": {"cvssMetricV31": [{"type": "Primary", "cvssData": {"vectorString": "CVSS:3.1/AV:N"}}]}}
        rec = advisories.osv_record(cve, {"pkg:github/mbed-tls/mbedtls": {"v4.1.1"}})
        self.assertEqual(rec["aliases"], ["CVE-2026-1"])  # Trustify makes the vulnerability from the CVE alias
        self.assertEqual(rec["affected"][0]["package"]["purl"], "pkg:github/mbed-tls/mbedtls")
        self.assertEqual(rec["affected"][0]["versions"], ["v4.1.1"])
        self.assertEqual(rec["severity"], [{"type": "CVSS_V3", "score": "CVSS:3.1/AV:N"}])


if __name__ == "__main__":
    unittest.main()
