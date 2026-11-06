# PoC plan: ESP32 devices managed by Flight Control (POSIX approach, ESP-IDF first)

This is the plan as written before the work started (4 November 2026). Some details changed along the way: there is no LittleFS (config files live in NVS), the firmware is one multi-chip OCI index rather than one tag per chip, and WASM apps use the OCI wasm artifact format only. What was built and measured is in [POC-RESULTS.md](POC-RESULTS.md).

## Context

Goal: show that Espressif microcontrollers can be managed, monitored and updated from a Flight Control control plane, with no Linux on the device and **no changes to the server**. Flight Control is the upstream of Red Hat Edge Manager and can be integrated with OpenShift/ACM. The PoC covers three levels of update:

- configuration, without a reboot;
- full firmware, through A/B OTA with rollback;
- WASM application logic distributed as an OCI image, without a reboot.

The core code is POSIX C (pthreads, BSD sockets, files, mbedTLS, WAMR). It runs both on a Linux/macOS host, for development and testing without hardware, and on ESP-IDF. Zephyr remains a future target behind the same `port/` layer.

## Assumptions

**Checked in the Flight Control code (main, 4 Nov 2026):**

- **Agent API** (`api/agent/v1beta1/openapi.yaml`):
  - `POST/GET enrollmentrequests`;
  - `POST/GET certificatesigningrequests`;
  - `GET devices/{name}/rendered?knownRenderedVersion=`, a long-poll of up to ~4 minutes that returns `204` if nothing changed;
  - `PUT/PATCH devices/{name}/status`.
- **TLS**: mTLS with **TLS 1.3** and ECDSA P-256/SHA-256 (`docs/user/references/certificate-architecture.md`).
- **Device name**: `lower(base32hex_nopad(sha256(trimLeft0(X) || trimLeft0(Y))))` of the public key (`pkg/crypto/key.go:89`, `internal/agent/identity/identity.go:132`).
- **Required status fields**: `conditions`, `systemInfo{architecture,bootID,operatingSystem,agentVersion}`, `applications`, `applicationsSummary`, `resources{cpu,memory,disk}`, `integrity`, `config{renderedVersion}`, `os{image,imageDigest}`, `updated`, `summary`, `lifecycle`.
- **Update state**:
  - A device is "up to date" when `status.config.renderedVersion` equals the rendered version served.
  - The OS is out of date when `status.os.image != spec.os.image`, a plain string comparison (`internal/service/common/device.go:262`).
  - Use `osMode: image`.
- **`container` apps**: the server only validates the OCI reference, the Podman-style limits format, and ports/env/volumes (`api/core/v1beta1/validation.go:1231`). Delta hints fail silently if the image cannot be inspected.
- **Disconnection**: a device becomes `Unknown` after 5 minutes without contact (`DeviceDisconnectedTimeout`, not configurable).
- **Fleets**: templates support Go placeholders (`{{ .metadata.labels.x }}`); rollouts use `BatchSequence` with a `successThreshold`.
- **Local deployment**:
  - `make deploy` on kind; host ports 3443 (API), 7443 (agent), 9001 (UI).
  - Enrollment config: `flightctl certificate request --signer=enrollment --expiration=365d --output=embedded`.
  - Approval: `flightctl approve -l k=v enrollmentrequest/<name>`.

**To verify during the PoC:**

- ESP-IDF v5.x mbedTLS as a **TLS 1.3 client** with ECDSA mTLS, and the RAM needed for the handshake on C3/S2.
- WAMR (fast interpreter) on ESP-IDF for Xtensa (S3) and RISC-V (C3/C5/C6).
- kind on a Podman machine (macOS): port 7443 reachable from the LAN, and a server certificate SAN with the LAN IP or nip.io hostname.
- Whether the server accepts "non-Linux" status values, e.g. `operatingSystem: esp-idf`, `architecture: xtensa-esp32s3`, `disk` from LittleFS.

**PoC simplifications (out of scope):**

- no TPM or attestation;
- the key stays in NVS without flash encryption;
- no cosign signatures;
- a local registry over HTTP without auth;
- WiFi configured at build time;
- no server changes.

## Hardware and roles

| Chip | Qty | PoC role |
|---|---|---|
| ESP32-S3 | 2 | Main target: config + firmware + WASM. One is `stage=canary` |
| ESP32-C3 | 3 | RISC-V: config + firmware + WASM (same `.wasm` as the S3) |
| ESP32-C5 / C6 | 2 / 1 | RISC-V: like the C3; later an ECDSA key in hardware |
| ESP32-S2 / ESP32 | 3 / 1 | Config + firmware only (limited RAM), heterogeneous fleet |

## PoC use case: "line temperature alarm"

Each device reads its internal temperature sensor, with a simulated fallback on the classic ESP32. It reports the value in its status and turns on the on-board LED above a threshold.

- **Config**: `threshold_c` and `sample_ms` live in an inline file in the fleet. Changing them needs no reboot.
- **Firmware**:
  - v1 → v2 (new information in the status), rolled out canary first;
  - a deliberately broken v3 → automatic rollback.
- **WASM** (`alarm` v1 → v2):
  - v1 lights the LED steadily; v2 blinks it, with hysteresis;
  - the app is replaced without a reboot, with the same digest on Xtensa and RISC-V.
- **Monitoring**:
  - heap, RSSI, uptime and temperature in `status`/`customInfo`;
  - a device that is switched off becomes `Unknown` after 5 minutes.

## Architecture (new repository, working name `flightctl-mcu`)

```
core/            POSIX C, no ESP-IDF includes
  fc_client.c    HTTP/TLS (mbedTLS) + JSON (cJSON) towards the agent API
  fc_identity.c  P-256 key, PKCS#10 CSR, device name
  fc_enroll.c    enrollment request -> poll for approval -> management cert
  fc_spec.c      rendered long-poll loop, knownRenderedVersion
  fc_status.c    builds the full status, periodic PUT/PATCH
  fc_reconcile.c applies config / os / applications, computes state
  oci_client.c   manifest/index/blob, sha256 check, streaming
  wasm_rt.c      WAMR: load/instantiate/start/stop, host API
port/port.h      ~12 functions: key/kv storage, files, OTA, sysinfo, time, reboot
port/linux/      host: OTA = file, kv = directory (development and CI)
port/espidf/     NVS, LittleFS, esp_ota_* + rollback, esp_chip_info, SNTP
esp-idf/         ESP-IDF component + examples/poc-sensor
wasm-apps/alarm  source (wasi-sdk) + Containerfile (platform wasip1/wasm)
deploy/          fleet.yaml, registry/enrollment scripts, demo Makefile
```

WASM host API (modelled on Ocre names where possible): `log`, `temp_read`, `led_set`, `config_get`, `sleep_ms`, `now_ms`.

OCI formats:

- **App**: two forms, both built with `podman build` and pushed to the local registry:
  - a "compat" image (`FROM scratch`, `COPY alarm.wasm /`, `ENTRYPOINT ["/alarm.wasm"]`, platform `wasip1/wasm`);
  - a raw `application/vnd.wasm.content.layer.v1+wasm` layer.
- **Firmware**: an artifact pushed with `oras push`, with its own `artifactType` and one `.bin` layer per chip. Each chip gets a tag (`fw:esp32s3-v2`), referenced from the fleet template through the `chip` label.

## Phases

**F0: local environment (Mac)**
- Steps:
  - a Podman machine with enough resources, a flightctl clone, `make deploy` (kind) and `flightctl login`;
  - a local `registry:2` on port 5000, exposed on the LAN;
  - generate the embedded enrollment config, then extract the CA, the enrollment cert and key, and the server URL.
- Exit criteria: UI on :9001, and an enrollment `curl --cert/--key` on `:7443` answers from the LAN.

**F1: core on the host (port/linux)**
- Steps: enrollment → `flightctl approve` → management cert → long-poll → full status.
- Exit criteria: a "fake device" on the Mac shows as `Online` in the UI, with its status accepted (no 400).

**F2: ESP-IDF port on the S3**
- Steps: WiFi, SNTP (for certificate validity), NVS for keys and certs, mbedTLS TLS 1.3, LittleFS.
- Exit criteria: the S3 is enrolled and `Online`; RAM and flash of agent + TLS are measured.

**F3: config**
- Steps: inline config → file in LittleFS → the app re-reads `threshold_c`, and the device reports `renderedVersion`.
- Exit criteria: a change in the fleet is applied within one long-poll cycle.

**F4: firmware OTA (`spec.os.image`)**
- Steps:
  - pull the manifest, then stream the blob into `esp_ota_write` with a sha256 check, then reboot;
  - confirm the app as valid only after it reconnects to Flight Control, otherwise roll back;
  - report `os.image` and `imageDigest`.
- Exit criteria:
  - the rollout reaches the canary (S3 `stage=canary`) first, then the rest of the fleet;
  - a broken v3 goes back to v2, and the device shows as Degraded/OutOfDate.

**F5: WASM app (`spec.applications`, type `container`)**
- Steps: pull the app image, extract the `.wasm`, run it in WAMR, manage its lifecycle and report `status.applications[]` (Running/Error, restarts, imageDigests).
- Exit criteria: `alarm` v1 → v2 without a reboot on the S3 and on C3/C5/C6, with the same digest.

**F6: heterogeneous fleet and results**
- Steps:
  - a template using `{{ .metadata.labels.chip }}`; S2 and ESP32 get config and firmware only;
  - measure RAM, flash, idle traffic per hour, and enrollment and update times.
- Exit criteria: a document listing the limitations found, as proposals for upstream: a disconnect timeout for sleeping devices, and an explicit declaration of the WASM runtime or MCU class.

## End-to-end verification (local)

1. `make deploy` on kind; `flightctl get devices` works and the local registry answers.
2. Host fake device (F1): `flightctl get enrollmentrequests` → approve → `flightctl get device/<name> -o yaml` shows a full status and `Online`.
3. Flash the S3/C3 with the embedded enrollment config, then approve with `-l chip=esp32s3 -l stage=canary -l site=lab`.
4. `flightctl apply -f deploy/fleet.yaml` (config + `os.image` v1 + app `alarm:v1`) → everything `UpToDate`.
5. Change the threshold → the LED reacts without a reboot, and `status.config.renderedVersion` is aligned.
6. Set the template `os.image` to v2 → the canary updates first, then the remaining batches. A broken v3 → the rollback shows in the status.
7. App `alarm:v2` → the LED behaviour changes without a reboot, on both Xtensa and RISC-V.
8. Switch a device off → `Unknown` after 5 minutes. Switch it back on → it returns `Online` and reconciles.
9. Automated host tests (port/linux) in CI against Flight Control on kind.

## Main risks

| Risk | Fallback |
|---|---|
| TLS 1.3/mTLS on ESP-IDF mbedTLS: support or RAM | Measure on the S3 before the smaller chips |
| kind not reachable from the LAN on macOS | Port-forward (`socat`/ssh) or a Linux machine |
| 4-minute long-poll through a home NAT/AP | Timeouts and reconnection are handled by the loop |
| Status validation rejects non-Linux values | Check in F1 on the host, before any hardware |
