# SBOM and known vulnerabilities

Every firmware release has an SBOM: what its images are built from, at which
version. Flight Control shows the known vulnerabilities of each device's
firmware from it, as it does for its own Linux images, once a Trustify
instance holds the SBOM.

## What a release's SBOM holds

`tools/fw-push.py` makes it with `tools/sbom.py` when it pushes a release:
CycloneDX 1.6, JSON. Its root component is the release itself: an OCI package
URL with the digest of the release's index, which is what each device reports
as its OS image (`status.os.imageDigest`). Under it, each component with a
package URL, a CPE where one is known, and its license:

| | From |
|---|---|
| the agent (this repository, at its commit), cJSON, WAMR, detools | cJSON's header, `third_party/fetch-wamr.sh`, `third_party/fetch-detools.sh` |
| ESP-IDF, the mbedTLS it ships, its managed components | `port/espidf/dependencies.lock.<chip>`; ESP-IDF's mbedTLS from `MBEDTLS_IN_ESP_IDF` in `tools/sbom.py` |
| Zephyr, its modules: mbedTLS, TF-PSA-Crypto, MCUboot, littlefs, picolibc, zcbor, and each board's HALs | the Zephyr tree (`third_party/zephyr-ws`): the modules' own version files, else the revision Zephyr's manifest names |

One SBOM covers every image in the release: an index with ESP-IDF and Zephyr
images lists both.

## Where it goes

- `build/sbom/<repository>-<tag>.cdx.json`;
- the registry, as an artifact that refers to the release's index (OCI 1.1
  `subject`, type `application/vnd.cyclonedx+json`), also tagged
  `sha256-<index digest>.sbom`, as cosign tags its attachments: `registry:3` has
  no referrers API, so `oras discover` finds it only on registries that do;
- Trustify, when `TRUSTIFY_URL` is set (and `TRUSTIFY_TOKEN`, when it asks for
  one): `POST /api/v2/sbom`, labelled with the index's digest, as Flight
  Control's image builder uploads its own.

Another release's, or one made again: `tools/sbom.py --ref <registry>/flightctl-mcu/fw:<tag>
--digest sha256:<index> --esp-idf esp32c3 --zephyr rpi_pico_w --upload <Trustify URL>`.

## In Flight Control

Flight Control 1.4 reads the vulnerabilities from Trustify (Helm values
`vulnerabilityReporting.enabled: true`, `vulnerabilityReporting.trustify.endpoint`).
Its client speaks Trustify 0.4's API (`/api/v2`): Trustify 0.5 and later moved
it to `/api/v3`. For the lab, `tools/trustify/up.sh` runs Trustify 0.4.12 next
to the kind cluster and points Flight Control at it; then:

```bash
TRUSTIFY_URL=http://127.0.0.1:8095 tools/fw-push.py ...
```

Every 15 minutes it takes the image digests the devices report, finds in
Trustify the package URL that holds each one and the SBOM that holds that
package URL, and stores the advisories Trustify matches against the SBOM's
components. The devices need nothing more: they report the digest already.

Which advisories Trustify matches depends on what it has imported (CVE, OSV,
CSAF feeds) and on how those name the components: by package URL or by CPE.
For C libraries the CPE (`cpe:2.3:a:arm:mbed_tls:...`, `cpe:2.3:o:zephyrproject:zephyr:...`)
is often the only name an advisory gives.

## What was verified with a test advisory

13 November 2026, Flight Control 1.4.0 and Trustify 0.4.12 on the lab's kind
cluster, with the Zephyr boards on release z13:

- Trustify took the SBOMs of z11, z12 and z13 (CycloneDX 1.6), and found each
  release's SBOM by the digest the devices report, as Flight Control asks.
- With a test advisory (OSV, made up and named as such, alias `CVE-2099-99999`,
  "affected": mbedTLS `v4.1.1`), Trustify listed it for z13's SBOM, and within
  a minute Flight Control showed it for the devices on z13: in
  `/api/v1/vulnerabilities/devices/<name>`, with the release's image and digest,
  severity and score, and in the fleet's and the organization's summaries.
- The advisory was then deleted from Trustify; Flight Control kept the finding
  ([limitation 14](RESULTS.md#flight-control-limitations-found-upstream-proposals)).

A test advisory had to carry a CVE alias: Trustify 0.4 made no vulnerability
of an OSV record without one.

## Trustify's feeds: no real advisory reaches these components

With Trustify 0.4's own feeds, no real advisory is matched to a firmware
release, whatever its SBOM says:

- **The CVE List** (cvelistV5, the `cve` importers): Trustify ties a CVE record
  to a package only for Maven (`divine_purl` in its CVE loader); it does not use
  CPEs. The records of the C libraries also say nothing a tool could use: the six
  2025–2026 CVEs of mbedTLS in cvelistV5 name vendor and product `n/a`, with no
  version ranges, only a description. NVD adds CPEs and ranges afterwards, and
  Trustify does not read NVD.
- **OSV**: Trustify turns into packages only the ecosystems of programming
  languages (npm, PyPI, Maven, Go, crates.io, NuGet...). The OSS-Fuzz and Git
  records that cover C libraries have no package URL, so they reach no package.

Checked on 13 November 2026 with Trustify 0.4.12: those six mbedTLS CVE records,
ingested from cvelistV5, were tied to no package, and the SBOMs of z11, z12 and
z13 had no advisory. (None of the six affects these releases anyway: all were
fixed in mbedTLS 4.1.0 and TF-PSA-Crypto 1.1.0; the releases have 4.1.1 and
1.1.1.)

The whole feed changes nothing: with the `cve-from-2024` importer run to the
end (176,157 advisories), the SBOMs still had none.

## From NVD: `tools/advisories.py`

NVD has what the feeds lack: for each CVE, CPEs with version ranges. For each
component of the given SBOMs that has a CPE, `tools/advisories.py` asks NVD for
the CVEs that match it at the version shipped, checks the ranges again, and
writes one OSV record per CVE: the CVE as its alias, NVD's description and CVSS
vector, and as affected the SBOM components' package URLs at the versions
shipped. Trustify ties those to the SBOMs, and Flight Control shows them:

```bash
tools/advisories.py build/sbom/fw-v3.2.cdx.json build/sbom/fw-z13.cdx.json --upload http://127.0.0.1:8095
```

Give it the SBOM of every release devices run, and run it again from time to
time: NVD keeps adding CVEs. Without `NVD_API_KEY` NVD answers five requests
in 30 seconds, about a minute for the components here. The CPE names it asks
for are those of NVD's dictionary, with the other names NVD files a product
under (`arm:mbed_tls` besides `trustedfirmware:mbed_tls`, for instance; `ALSO`
in the script). MCUboot, littlefs, picolibc and detools are not in the
dictionary, so nothing is found for them this way.

13 November 2026, the three releases the lab's boards run: v3.0 (ESP-IDF 5.5.6,
built from commit `bd007f08`, its SBOM rebuilt from that commit), v3.2 (ESP-IDF
6.0.3) and z13 (Zephyr 4.5). NVD gave six CVEs; within a minute Flight Control
showed them on the devices of each release, in the fleets' and the
organization's summaries (one critical, three high, two medium):

| CVE | Release | Component | Applies here? |
|---|---|---|---|
| CVE-2025-57052 (9.8) | v3.0 | cJSON 1.7.18 | No: it is in `cJSON_Utils.c`, which is not built (only `cJSON.c` is) |
| CVE-2026-67215 (7.5), CVE-2026-67217 (5.3) | all | cJSON 1.7.18, 1.7.19 | No: JSON Patch, in `cJSON_Utils.c` too |
| CVE-2026-67216 (7.5) | all | cJSON 1.7.18, 1.7.19 | No: `cJSON_Compare()`, which nothing calls |
| CVE-2026-16554 (7.8) | v3.2, z13 | cJSON 1.7.19 | Not in practice: it needs a string of about 859 million control characters |
| CVE-2025-66442 | v3.0 | mbedTLS 3.6.7 | No: a timing side channel only with LLVM's select-optimize; ESP-IDF builds with GCC |

So a match by version is not a vulnerability of the firmware: what a release
compiles and calls decides. Flight Control has no way to mark a finding as not
applicable; the records written here say "affected" for every version NVD
covers. cJSON has no release with these fixed (1.7.19 is the latest).
