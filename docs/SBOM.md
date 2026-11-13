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
Every 15 minutes it takes the image digests the devices report, finds in
Trustify the package URL that holds each one and the SBOM that holds that
package URL, and stores the advisories Trustify matches against the SBOM's
components. The devices need nothing more: they report the digest already.

Which advisories Trustify matches depends on what it has imported (CVE, OSV,
CSAF feeds) and on how those name the components: by package URL or by CPE.
For C libraries the CPE (`cpe:2.3:a:arm:mbed_tls:...`, `cpe:2.3:o:zephyrproject:zephyr:...`)
is often the only name an advisory gives.

## What was verified

The SBOM of the Zephyr releases from z11 on is made and pushed with them. Not yet tried: Trustify with
Flight Control, and which advisories it finds for these components.
