#!/usr/bin/env python3
"""The SBOM of a firmware release: CycloneDX 1.6, what the images in its OCI
index are built from, at the versions this repository pins.

    tools/sbom.py --digest sha256:<index> --ref <registry>/flightctl-mcu/fw:v3 \\
        [--esp-idf esp32c3,esp32s3 ...] [--zephyr rpi_pico_w,esp32s3_devkitc ...] [-o FILE]
    tools/sbom.py ... --upload https://trustify.example.com   (or $TRUSTIFY_URL)

tools/fw-push.py makes one for each release it pushes (docs/SBOM.md). Its root
component is the release as Flight Control knows it: an OCI package URL with
the index's digest, the digest each device reports as its OS image
(status.os.imageDigest). Flight Control 1.4 asks Trustify for the SBOM that
holds that package URL and shows the advisories Trustify matches against the
components' package URLs and CPEs. --upload sends it to Trustify the way
Flight Control's own client does (POST /api/v2/sbom, label sha256~<digest>).

Versions come from where they are set: cJSON's header, the fetch scripts
(WAMR, detools, Zephyr), the ESP-IDF lock files (port/espidf/dependencies.lock.<chip>),
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
MBEDTLS_IN_ESP_IDF = {"5.5.6": "3.6.7", "6.0.3": "4.1.1"}

# Zephyr modules each board's image is built with, besides those of every build.
ZEPHYR_COMMON = ("mbedtls", "tf-psa-crypto", "mcuboot", "littlefs", "picolibc", "zcbor")
ZEPHYR_BOARD_MODULES = {
    "rpi_pico_w": ("hal_rpi_pico", "hal_infineon", "cmsis", "cmsis_6"),
    "esp32s3_devkitc": ("hal_espressif",),
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
# littlefs, picolibc and detools are not in it: they get none.
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
    detools = grep("third_party/fetch-detools.sh", r'DETOOLS_TAG="\$\{DETOOLS_TAG:-([^}]+)\}"')
    return [
        component("flightctl-mcu", version, f"pkg:github/francescopace/flightctl-mcu@{commit}", "application",
                  license_id="Apache-2.0", description="The Flight Control agent for microcontrollers"),
        component("cJSON", cjson, f"pkg:github/davegamble/cjson@v{cjson}",
                  cpe_id=cpe("cjson_project", "cjson", cjson), license_id="MIT"),
        component("wasm-micro-runtime", wamr.removeprefix("WAMR-"),
                  f"pkg:github/bytecodealliance/wasm-micro-runtime@{wamr}",
                  cpe_id=cpe("bytecodealliance", "webassembly_micro_runtime", wamr.removeprefix("WAMR-")),
                  license_id="Apache-2.0", description="WAMR, fast interpreter only"),
        component("detools", detools, f"pkg:github/eerimoq/detools@{detools}", license_id="BSD-2-Clause",
                  description="C library, with the heatshrink decoder it includes: applies firmware deltas"),
    ]


def esp_idf_components(chips):
    out, seen = [], set()
    for chip in chips:
        lock = read(f"port/espidf/dependencies.lock.{chip}")
        deps = {}
        for name, version in re.findall(r"^  (\S+):\n(?:    .*\n)*?    version: (\S+)", lock, re.M):
            deps[name] = version
        idf = deps.get("idf")
        if idf is None:
            sys.exit(f"dependencies.lock.{chip}: no idf version")
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
    v = zephyr_version("third_party/zephyr-ws/zephyr/VERSION")
    out = [component("zephyr", v, f"pkg:github/zephyrproject-rtos/zephyr@v{v}", "operating-system",
                     cpe_id=cpe("zephyrproject", "zephyr", v, part="o"), license_id="Apache-2.0")]
    revs = west_revisions()
    modules = list(ZEPHYR_COMMON)
    for p in platforms:
        if p not in ZEPHYR_BOARD_MODULES:
            sys.exit(f"which Zephyr modules does {p} take? Add it to ZEPHYR_BOARD_MODULES in {__file__}")
        modules += [m for m in ZEPHYR_BOARD_MODULES[p] if m not in modules]
    out += [zephyr_module(m, revs) for m in modules]
    return out


def make_sbom(ref, digest, esp_idf=(), zephyr=()):
    host, rest = ref.split("/", 1)
    repo, tag = rest.rsplit(":", 1) if ":" in rest else (rest, "")
    hexd = digest.removeprefix("sha256:")
    root_purl = (f"pkg:oci/{repo.rsplit('/', 1)[-1]}@sha256%3A{hexd}?repository_url="
                 + urllib.parse.quote(f"{host}/{repo}", safe="") + (f"&tag={tag}" if tag else ""))
    comps = core_components()
    if esp_idf:
        comps += esp_idf_components(esp_idf)
    if zephyr:
        comps += zephyr_components(zephyr)
    platforms = [f"esp-idf/{c}" for c in esp_idf] + [f"zephyr/{p}" for p in zephyr]
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
    p.add_argument("--esp-idf", default="", help="chips with an ESP-IDF image, comma separated")
    p.add_argument("--zephyr", default="", help="Zephyr platforms (CONFIG_FC_FW_PLATFORM), comma separated")
    p.add_argument("-o", "--out", help="write it here (default: stdout)")
    p.add_argument("--upload", default=os.environ.get("TRUSTIFY_URL"), help="Trustify's URL ($TRUSTIFY_URL)")
    a = p.parse_args()
    split = lambda s: [x for x in s.split(",") if x]
    sbom = make_sbom(a.ref, a.digest, split(a.esp_idf), split(a.zephyr))
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
