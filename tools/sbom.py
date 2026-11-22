#!/usr/bin/env python3
"""The SBOM of a firmware release: CycloneDX 1.6, what the images in its OCI
index are built from, at the versions this repository pins.

    tools/sbom.py --digest sha256:<index> --ref <registry>/flightctl-mcu/fw:v3 \\
        [--esp-idf esp32c3,esp32s3 ...] [--zephyr esp32s3_devkitc ...] [--nuttx nuttx-esp32s3-n16r8] [-o FILE]
    tools/sbom.py ... --upload https://trustify.example.com   (or $TRUSTIFY_URL)

tools/fw-push.py makes one for each release it pushes (docs/SBOM.md). Its root
component is the release as Flight Control knows it: an OCI package URL with
the index's digest, the digest each device reports as its OS image
(status.os.imageDigest). Flight Control 1.4 asks Trustify for the SBOM that
holds that package URL and shows the advisories Trustify matches against the
components' package URLs and CPEs. --upload sends it to Trustify the way
Flight Control's own client does (POST /api/v2/sbom, label sha256~<digest>).

Versions come from cJSON's header, the fetch scripts, the ESP-IDF application
descriptors and their matching lock files (port/espidf/dependencies.lock.<chip>),
and the Zephyr tree's own files (third_party/zephyr-ws). The mbedTLS that comes
with an ESP-IDF release is not in this repository: MBEDTLS_IN_ESP_IDF names it.
"""
import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import urllib.parse
import urllib.request
import uuid

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WS = os.path.join(ROOT, "third_party", "zephyr-ws")

# The mbedTLS each ESP-IDF release ships (components/mbedtls). docs/DEPENDENCIES.md.
MBEDTLS_IN_ESP_IDF = {"5.5.6": "3.6.7", "6.0.3": "4.1.1", "6.1.1": "4.1.1"}

# Zephyr modules each board's image is built with, besides those of every build.
ZEPHYR_COMMON = ("mbedtls", "tf-psa-crypto", "mcuboot", "littlefs", "picolibc", "zcbor")
ZEPHYR_BOARD_MODULES = {
    "esp32_devkitc": ("hal_espressif",),
    "esp32c3_devkitm": ("hal_espressif",),
    "esp32c5-mini": ("hal_espressif",),
    "esp32c5_devkitc": ("hal_espressif",),
    "esp32c6_devkitc": ("hal_espressif",),
    "esp32s3-mini-n4r2": ("hal_espressif",),
    "esp32s3_devkitc": ("hal_espressif",),
    "nucleo_f767zi": ("hal_stm32", "cmsis", "cmsis_6"),
    "nucleo_f767zi-stlink": ("hal_stm32", "cmsis", "cmsis_6"),
    "nrf7002dk_nrf5340_cpuapp": ("hal_nordic", "nrf_wifi", "hostap", "open-amp", "libmetal", "cmsis", "cmsis_6"),
    "rpi_pico2_rp2350a_m33_0_w": ("hal_rpi_pico", "hal_infineon", "cmsis", "cmsis_6"),
}


def read(path):
    with open(os.path.join(ROOT, path)) as f:
        return f.read()


def grep(path, pattern):
    m = re.search(pattern, read(path), re.M)
    if not m:
        sys.exit(f"{path}: no {pattern}")
    return m.group(1)


# CPE names as NVD's dictionary has them (checked November 2026). MCUboot,
# littlefs and picolibc are not in it: they get none.
def cpe(vendor, product, version, part="a"):
    return f"cpe:2.3:{part}:{vendor}:{product}:{version}:*:*:*:*:*:*:*"


def component(name, version, purl, kind="library", cpe_id=None, license_id=None, description=None, ref=None):
    c = {"type": kind, "bom-ref": ref or purl, "name": name, "version": version, "purl": purl}
    if cpe_id:
        c["cpe"] = cpe_id
    if license_id:
        c["licenses"] = [{"license": {"id": license_id}}]
    if description:
        c["description"] = description
    return c


def agent_version():
    try:
        out = subprocess.run(["git", "-C", ROOT, "describe", "--always", "--dirty", "--tags"],
                             capture_output=True, text=True, check=True).stdout.strip()
        commit = subprocess.run(["git", "-C", ROOT, "rev-parse", "HEAD"],
                                capture_output=True, text=True, check=True).stdout.strip()
        return out, commit
    except (OSError, subprocess.CalledProcessError):
        return "unknown", "unknown"


def core_components():
    version, commit = agent_version()
    cjson = ".".join(grep("third_party/cjson/cJSON.h", rf"#define CJSON_VERSION_{p} (\d+)")
                     for p in ("MAJOR", "MINOR", "PATCH"))
    wamr = grep("third_party/fetch-wamr.sh", r'WAMR_TAG="\$\{WAMR_TAG:-([^}]+)\}"')
    return [
        component("flightctl-mcu", version, f"pkg:github/francescopace/flightctl-mcu@{commit}", "application",
                  license_id="Apache-2.0", description="The Flight Control agent for microcontrollers"),
        component("cJSON", cjson, f"pkg:github/davegamble/cjson@v{cjson}",
                  cpe_id=cpe("cjson_project", "cjson", cjson), license_id="MIT"),
        component("wasm-micro-runtime", wamr.removeprefix("WAMR-"),
                  f"pkg:github/wasm-micro-runtime/wasm-micro-runtime@{wamr}",
                  cpe_id=cpe("bytecodealliance", "webassembly_micro_runtime", wamr.removeprefix("WAMR-")),
                  license_id="Apache-2.0", description="WAMR, fast interpreter only"),
    ]


def image_idf_version(header, path):
    # The first segment starts at byte 32 with esp_app_desc_t. Its idf_ver
    # is a 32-byte string at offset 112 in the descriptor, also on signed apps.
    if len(header) < 176 or header[0] != 0xe9 or header[32:36] != bytes.fromhex("3254cdab"):
        sys.exit(f"{path}: missing ESP-IDF application descriptor")
    version = header[144:176].split(b"\0", 1)[0]
    if not re.fullmatch(rb"v?[0-9][0-9A-Za-z.+_-]*", version):
        sys.exit(f"{path}: invalid ESP-IDF version in application descriptor")
    return version.decode("ascii").removeprefix("v")


def esp_idf_components(images):
    out, seen = [], set()
    for chip, idf in images:
        lock = read(f"port/espidf/dependencies.lock.{chip.split('-')[0]}")  # esp32s3-r8: esp32s3
        deps = {}
        for name, version in re.findall(r"^  (\S+):\n(?:    .*\n)*?    version: (\S+)", lock, re.M):
            deps[name] = version
        locked_idf = deps.get("idf")
        if not locked_idf:
            sys.exit(f"dependencies.lock.{chip.split('-')[0]}: no idf version")
        if idf != locked_idf:
            sys.exit(f"{chip}: firmware uses ESP-IDF {idf}, lock file specifies {locked_idf}. "
                     "Rebuild the image or use the checkout that built it.")
        if ("idf", idf) not in seen:
            seen.add(("idf", idf))
            out.append(component("esp-idf", idf, f"pkg:github/espressif/esp-idf@v{idf}", "framework",
                                 cpe_id=cpe("espressif", "esp-idf", idf), license_id="Apache-2.0"))
            mbedtls = MBEDTLS_IN_ESP_IDF.get(idf)
            if mbedtls is None:
                sys.exit(f"which mbedTLS comes with ESP-IDF {idf}? Add it to MBEDTLS_IN_ESP_IDF in {__file__}")
            out.append(component("mbedtls", mbedtls, f"pkg:github/mbed-tls/mbedtls@v{mbedtls}",
                                 cpe_id=cpe("trustedfirmware", "mbed_tls", mbedtls), license_id="Apache-2.0",
                                 description=f"from ESP-IDF {idf}", ref=f"esp-idf-{idf}-mbedtls"))
        for name, version in deps.items():
            if name == "idf" or (name, version) in seen:
                continue
            seen.add((name, version))
            ns, short = name.split("/", 1)
            out.append(component(short, version,
                                 f"pkg:generic/{ns}/{short}@{version}?download_url="
                                 + urllib.parse.quote(f"https://components.espressif.com/components/{name}", safe=""),
                                 license_id="Apache-2.0", description="ESP-IDF component"))
    return out


def west_revisions():
    revs, name = {}, None
    for line in open(os.path.join(WS, "zephyr", "west.yml")):
        m = re.match(r"\s+- name: (\S+)", line)
        if m:
            name = m.group(1)
            continue
        m = re.match(r"\s+revision: (\S+)", line)
        if m and name:
            revs[name] = m.group(1)
    return revs


def zephyr_version(path):
    v = dict(re.findall(r"^(\w+) = (\S*)", read(path), re.M))
    return f"{v['VERSION_MAJOR']}.{v['VERSION_MINOR']}.{v['PATCHLEVEL']}"


def zephyr_module(name, revs):
    rev = revs.get(name, "unknown")
    if name == "mbedtls":
        v = grep("third_party/zephyr-ws/modules/crypto/mbedtls/include/mbedtls/build_info.h",
                 r'#define MBEDTLS_VERSION_STRING\s+"([^"]+)"')
        return component("mbedtls", v, f"pkg:github/mbed-tls/mbedtls@v{v}", cpe_id=cpe("trustedfirmware", "mbed_tls", v),
                         license_id="Apache-2.0", description=f"Zephyr's module, {rev}", ref=f"zephyr-mbedtls-{v}")
    if name == "tf-psa-crypto":
        v = grep("third_party/zephyr-ws/modules/crypto/tf-psa-crypto/include/tf-psa-crypto/build_info.h",
                 r'#define TF_PSA_CRYPTO_VERSION_STRING\s+"([^"]+)"')
        return component("TF-PSA-Crypto", v, f"pkg:github/mbed-tls/tf-psa-crypto@v{v}",
                         cpe_id=cpe("trustedfirmware", "tf-psa-crypto", v), license_id="Apache-2.0")
    if name == "mcuboot":
        v = zephyr_version("third_party/zephyr-ws/bootloader/mcuboot/boot/zephyr/VERSION")
        return component("mcuboot", v, f"pkg:github/mcu-tools/mcuboot@v{v}", license_id="Apache-2.0", description="the bootloader, built with the image")
    if name == "littlefs":
        n = int(grep("third_party/zephyr-ws/modules/fs/littlefs/lfs.h", r"#define LFS_VERSION (0x[0-9a-fA-F]+)"), 16)
        v = f"{n >> 16}.{n & 0xffff}"
        return component("littlefs", v, f"pkg:github/littlefs-project/littlefs@v{v}",
                         license_id="BSD-3-Clause")
    if name == "picolibc":
        v = grep("third_party/zephyr-ws/modules/lib/picolibc/meson.build", r"^\s+version: '([^']+)'")
        return component("picolibc", v, f"pkg:github/picolibc/picolibc@{v}", license_id="BSD-3-Clause")
    # The rest at the revision Zephyr's manifest names.
    return component(name, rev, f"pkg:github/zephyrproject-rtos/{name}@{rev}",
                     description="Zephyr module, at the revision of Zephyr's manifest")


def zephyr_components(platforms):
    modules = list(ZEPHYR_COMMON)
    for p in platforms:
        if p not in ZEPHYR_BOARD_MODULES:
            sys.exit(f"which Zephyr modules does {p} take? Add it to ZEPHYR_BOARD_MODULES in {__file__}")
        modules += [m for m in ZEPHYR_BOARD_MODULES[p] if m not in modules]
    v = zephyr_version("third_party/zephyr-ws/zephyr/VERSION")
    out = [component("zephyr", v, f"pkg:github/zephyrproject-rtos/zephyr@v{v}", "operating-system",
                     cpe_id=cpe("zephyrproject", "zephyr", v, part="o"), license_id="Apache-2.0")]
    revs = west_revisions()
    out += [zephyr_module(m, revs) for m in modules]
    if "nrf_wifi" in modules:
        manifest = "third_party/zephyr-ws/modules/lib/nrf_wifi/zephyr/module.yml"
        version = grep(manifest, r"path: wifi_fw_bins/default/nrf70\.bin\n(?:      [^\n]+\n)*?      version: '([^']+)'")
        radio = component("nRF70 Wi-Fi firmware", version, f"pkg:generic/nordic/nrf70-wifi@{version}", "firmware")
        radio["licenses"] = [{"license": {"name": "LicenseRef-Nordic-5-Clause"}}]
        out.append(radio)
    return out


def release_inventory(esp_idf, zephyr, nuttx=()):
    components = core_components() + esp_idf_components(esp_idf)
    if zephyr:
        components += zephyr_components(zephyr)
    platforms = [f"esp-idf/{chip}" for chip, _ in esp_idf] + [f"zephyr/{board}" for board in zephyr]
    if nuttx:
        v = grep("port/nuttx/build.sh", r"git clone --branch nuttx-([0-9.]+)")
        tls = grep("port/nuttx/build.sh", r'export FC_MBEDTLS="\$work/mbedtls-([0-9.]+)"')
        # Revisions selected by the pinned NuttX and nuttx-apps release.
        espressif = any(board.startswith("nuttx-esp32") for board in nuttx)
        if espressif:
            bootutil = "e36d3c8d6ca5e0ed1f510f2a7d110f405a7c396b"
            hal = "b90b1837cb5ad24747deb4c895246037cc206ce5"
            components.extend([
                component("mcuboot", bootutil, f"pkg:github/mcu-tools/mcuboot@{bootutil}", license_id="Apache-2.0",
                          description="Boot utility linked into the NuttX application"),
                component("esp-hal-3rdparty", hal, f"pkg:github/espressif/esp-hal-3rdparty@{hal}", license_id="Apache-2.0"),
            ])
        if espressif or "nuttx-pico2w" in nuttx or "nuttx-nrf7002dk" in nuttx:
            components.append(component("littlefs", "2.5.1", "pkg:github/littlefs-project/littlefs@v2.5.1", license_id="BSD-3-Clause"))
        if "nuttx-nrf7002dk" in nuttx:
            for name in ("open-amp", "libmetal"):
                components.append(component(name, "2025.10.0", f"pkg:github/OpenAMP/{name}@v2025.10.0",
                                            license_id="BSD-3-Clause"))
        if "nuttx-pico2w" in nuttx:
            components.append(component("CYW43439 Wi-Fi firmware", "7.95.49", "pkg:generic/infineon/cyw43439@7.95.49",
                                        "firmware"))
        components.extend([
            component("tf-psa-crypto", "1.1.1", "pkg:github/mbed-tls/tf-psa-crypto@v1.1.1",
                      license_id="Apache-2.0", ref="nuttx-tf-psa-crypto-1.1.1"),
            component("nuttx", v, f"pkg:github/apache/nuttx@nuttx-{v}", "operating-system", license_id="Apache-2.0"),
            component("nuttx-apps", v, f"pkg:github/apache/nuttx-apps@nuttx-{v}", license_id="Apache-2.0"),
            component("mbedtls", tls, f"pkg:github/mbed-tls/mbedtls@v{tls}", license_id="Apache-2.0",
                      ref=f"nuttx-mbedtls-{tls}", cpe_id=cpe("trustedfirmware", "mbed_tls", tls)),
        ])
        platforms.extend(f"nuttx/{board}" for board in nuttx)
    return platforms, components


def make_sbom(ref, digest, esp_idf=(), zephyr=(), nuttx=()):
    images = []
    for spec in esp_idf:
        chip, _, path = spec.partition("=")
        path = path or os.path.join(ROOT, "port", "espidf", f"build-{chip}", "flightctl_mcu_fw.bin")
        with open(path, "rb") as image:
            images.append((chip, image_idf_version(image.read(176), path)))
    return sbom_document(ref, digest, *release_inventory(images, zephyr, nuttx))


def sbom_document(ref, digest, platforms, comps):
    host, rest = ref.split("/", 1)
    repo, tag = rest.rsplit(":", 1) if ":" in rest else (rest, "")
    hexd = digest.removeprefix("sha256:")
    root_purl = (f"pkg:oci/{repo.rsplit('/', 1)[-1]}@sha256%3A{hexd}?repository_url="
                 + urllib.parse.quote(f"{host}/{repo}", safe="") + (f"&tag={tag}" if tag else ""))
    root = {"type": "firmware", "bom-ref": root_purl, "name": f"{host}/{repo}", "version": tag or digest,
            "purl": root_purl, "hashes": [{"alg": "SHA-256", "content": hexd}],
            "description": "flightctl-mcu firmware release (OCI index), one image per platform: "
                           + ", ".join(platforms)}
    return {
        "bomFormat": "CycloneDX",
        "specVersion": "1.6",
        "serialNumber": f"urn:uuid:{uuid.uuid4()}",
        "version": 1,
        "metadata": {
            "timestamp": datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat(),
            "tools": {"components": [{"type": "application", "name": "flightctl-mcu tools/sbom.py"}]},
            "component": root,
        },
        "components": comps,
        "dependencies": [{"ref": root_purl, "dependsOn": [c["bom-ref"] for c in comps]}],
    }


def upload(url, sbom, digest):
    """POST /api/v2/sbom with the label Flight Control's client gives (sha256~<hex>)."""
    labels = urllib.parse.quote("sha256~" + digest.removeprefix("sha256:"), safe="")
    req = urllib.request.Request(f"{url.rstrip('/')}/api/v2/sbom?labels={labels}",
                                 data=json.dumps(sbom).encode(), method="POST",
                                 headers={"Content-Type": "application/json"})
    token = os.environ.get("TRUSTIFY_TOKEN")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            return r.status, r.read().decode(errors="replace")
    except urllib.error.HTTPError as e:
        if e.code == 409:
            return 409, "already there"
        raise


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--ref", required=True, help="<registry>/<repository>:<tag> of the release")
    p.add_argument("--digest", required=True, help="the index's digest, sha256:...")
    p.add_argument("--esp-idf", default="", help="comma-separated chip or chip=app.bin (default: build-<chip>)")
    p.add_argument("--zephyr", default="", help="Zephyr platforms (CONFIG_FC_FW_PLATFORM), comma separated")
    p.add_argument("--nuttx", default="", help="NuttX firmware platform names, comma separated")
    p.add_argument("-o", "--out", help="write it here (default: stdout)")
    p.add_argument("--upload", default=os.environ.get("TRUSTIFY_URL"), help="Trustify's URL ($TRUSTIFY_URL)")
    a = p.parse_args()
    split = lambda s: [x for x in s.split(",") if x]
    try:
        sbom = make_sbom(a.ref, a.digest, split(a.esp_idf), split(a.zephyr), split(a.nuttx))
    except OSError as e:
        p.exit(1, f"{e}\n")
    text = json.dumps(sbom, indent=2) + "\n"
    if a.out:
        with open(a.out, "w") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    if a.upload:
        status, body = upload(a.upload, sbom, a.digest)
        print(f"Trustify: {status} {body[:200]}", file=sys.stderr)


if __name__ == "__main__":
    main()
