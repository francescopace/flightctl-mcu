#!/usr/bin/env python3
"""Known vulnerabilities of the components in firmware SBOMs, from NVD, as OSV
records that Trustify ties to those components (docs/SBOM.md).

    tools/advisories.py build/sbom/fw-v3.cdx.json [...] [--out DIR] [--upload URL]

Trustify reads no CPE: it ties a CVE to a package only through a package URL,
which the CVE records of C libraries do not have. NVD has what is missing, a
CPE with version ranges for each CVE. For each component of the SBOMs that has
a CPE (tools/sbom.py), this asks NVD for the CVEs whose configurations match
it at the version shipped, checks the ranges again here, and writes one OSV
record per CVE: the CVE as its alias, NVD's CVSS vector and description, and
as "affected" the SBOM components' package URLs at the versions shipped. Once
in Trustify (--upload, or $TRUSTIFY_URL), Flight Control shows them on the
devices running those releases.

A record names only the versions the given SBOMs ship, so give it every
release that devices run. Run it again from time to time: NVD keeps adding
CVEs. $NVD_API_KEY raises NVD's rate limit (5 requests in 30 s without).
"""
import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

NVD = "https://services.nvd.nist.gov/rest/json/cves/2.0"

# Other names NVD files the same products under, besides the SBOM's CPE.
ALSO = {
    ("trustedfirmware", "mbed_tls"): [("a", "arm", "mbed_tls")],
    ("trustedfirmware", "tf-psa-crypto"): [("a", "linaro", "tf-psa-crypto")],
    ("zephyrproject", "zephyr"): [("a", "zephyrproject", "zephyr")],
    ("cjson_project", "cjson"): [("a", "davegamble", "cjson")],
}


def version_key(v):
    """1.10.2 > 1.9: numbers compared as numbers; a pre-release (rc, beta) before its release."""
    parts = re.findall(r"\d+|[a-zA-Z]+", v.lstrip("vV"))
    key = []
    for p in parts:
        key.append((1, int(p), "") if p.isdigit() else (0, 0, p.lower()))
    return key + [(1, -1, "")]


def in_range(version, m):
    """Does the CPE match `m` (one of a configuration's cpeMatch) cover this version?"""
    crit = m["criteria"].split(":")
    exact = crit[5]
    if exact not in ("*", "-"):
        return version_key(exact) == version_key(version)
    v = version_key(version)
    if "versionStartIncluding" in m and v < version_key(m["versionStartIncluding"]):
        return False
    if "versionStartExcluding" in m and v <= version_key(m["versionStartExcluding"]):
        return False
    if "versionEndIncluding" in m and v > version_key(m["versionEndIncluding"]):
        return False
    if "versionEndExcluding" in m and v >= version_key(m["versionEndExcluding"]):
        return False
    return True


def affects(cve, names, version):
    """True if one of the CVE's vulnerable CPE matches names this product at this version."""
    for conf in cve.get("configurations", []):
        for node in conf.get("nodes", []):
            for m in node.get("cpeMatch", []):
                if not m.get("vulnerable"):
                    continue
                c = m["criteria"].split(":")
                if (c[2], c[3], c[4]) in names and in_range(version, m):
                    return True
    return False


def nvd(params, key=None):
    url = f"{NVD}?{urllib.parse.urlencode(params)}"
    for attempt in range(6):
        req = urllib.request.Request(url, headers={"apiKey": key} if key else {})
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                return json.load(r)
        except urllib.error.HTTPError as e:
            if e.code not in (403, 429, 500, 502, 503, 504):
                raise
        except (urllib.error.URLError, TimeoutError):
            pass
        time.sleep(10 * (attempt + 1))
    sys.exit(f"NVD does not answer: {url}")


def components(paths):
    """{(part, vendor, product, version): [package URLs]} from the SBOMs' components with a CPE."""
    out = {}
    for path in paths:
        with open(path) as f:
            doc = json.load(f)
        for c in doc.get("components", []):
            if "cpe" not in c:
                continue
            p = c["cpe"].split(":")
            out.setdefault((p[2], p[3], p[4], p[5]), set()).add(c["purl"])
    return out


def cvss(cve):
    m = cve.get("metrics", {})
    for name, kind in (("cvssMetricV31", "CVSS_V3"), ("cvssMetricV30", "CVSS_V3"), ("cvssMetricV40", "CVSS_V4")):
        for entry in m.get(name, []):
            if entry.get("type") == "Primary" or len(m[name]) == 1:
                return [{"type": kind, "score": entry["cvssData"]["vectorString"]}]
    return []


def osv_record(cve, affected):
    """affected: {purl without version: {versions}}."""
    desc = next((d["value"] for d in cve.get("descriptions", []) if d["lang"] == "en"), "")
    return {
        "schema_version": "1.6.0",
        "id": f"FCMCU-{cve['id']}",
        "aliases": [cve["id"]],
        "published": cve["published"][:19] + "Z",
        "modified": cve["lastModified"][:19] + "Z",
        "summary": desc.split(". ")[0][:200],
        "details": desc + "\n\nFrom NVD's CPE configurations, for the components of flightctl-mcu's firmware "
                          "(tools/advisories.py).",
        "severity": cvss(cve),
        # The package URL is what Trustify ties the record by. Its OSV parser
        # (0.4) wants an ecosystem and knows no "GIT": OSS-Fuzz, the one OSV
        # gives C and C++ libraries.
        "affected": [{"package": {"ecosystem": "OSS-Fuzz", "name": purl.rsplit("/", 1)[-1], "purl": purl},
                      "versions": sorted(versions)}
                     for purl, versions in sorted(affected.items())],
        "references": [{"type": "ADVISORY", "url": f"https://nvd.nist.gov/vuln/detail/{cve['id']}"}]
                      + [{"type": "WEB", "url": r["url"]} for r in cve.get("references", [])[:10]],
        "database_specific": {"source": "NVD"},
    }


def find(paths, key=None, log=print):
    """{CVE id: OSV record} for the components of these SBOMs."""
    records = {}
    for (part, vendor, product, version), purls in sorted(components(paths).items()):
        names = [(part, vendor, product)] + ALSO.get((vendor, product), [])
        seen = 0
        for p, v, n in names:
            res = nvd({"virtualMatchString": f"cpe:2.3:{p}:{v}:{n}:{version}:*:*:*:*:*:*:*",
                       "resultsPerPage": 2000}, key)
            time.sleep(0.7 if key else 6.5)
            for item in res.get("vulnerabilities", []):
                cve = item["cve"]
                if cve.get("vulnStatus") == "Rejected" or not affects(cve, names, version):
                    continue
                seen += 1
                rec = records.get(cve["id"]) or {"cve": cve, "affected": {}}
                for purl in purls:
                    base, _, ver = purl.split("?")[0].partition("@")
                    rec["affected"].setdefault(base, set()).add(urllib.parse.unquote(ver))
                records[cve["id"]] = rec
        log(f"{vendor}:{product} {version}: {seen} CVE matches")
    return {cid: osv_record(r["cve"], r["affected"]) for cid, r in sorted(records.items())}


def upload(url, record):
    req = urllib.request.Request(f"{url.rstrip('/')}/api/v2/advisory", data=json.dumps(record).encode(),
                                 method="POST", headers={"Content-Type": "application/json"})
    token = os.environ.get("TRUSTIFY_TOKEN")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status
    except urllib.error.HTTPError as e:
        if e.code == 409:
            return 409
        raise


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("sbom", nargs="+", help="CycloneDX SBOMs of the releases devices run (tools/sbom.py)")
    p.add_argument("--out", help="write each OSV record into this directory")
    p.add_argument("--upload", default=os.environ.get("TRUSTIFY_URL"), help="Trustify's URL ($TRUSTIFY_URL)")
    a = p.parse_args()
    records = find(a.sbom, os.environ.get("NVD_API_KEY"))
    for cid, rec in records.items():
        versions = "; ".join(f"{x['package']['name']} {', '.join(x['versions'])}" for x in rec["affected"])
        sev = rec["severity"][0]["score"] if rec["severity"] else "no CVSS"
        print(f"{cid}  {versions}  {sev}")
        if a.out:
            os.makedirs(a.out, exist_ok=True)
            with open(os.path.join(a.out, f"{rec['id']}.json"), "w") as f:
                json.dump(rec, f, indent=2)
        if a.upload:
            upload(a.upload, rec)
    print(f"{len(records)} CVEs" + (f", in Trustify at {a.upload}" if a.upload else ""))


if __name__ == "__main__":
    main()
