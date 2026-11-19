# Flight Control: limitations found

These findings cover Flight Control 1.4.0. 

---

## Upstream blockers for agent v1.0.0

The official agent 1.0.0 release depends on the five Flight Control changes below. Each entry describes what works with 1.4.0 and what still needs a server change or release. [Release roadmap](ROADMAP.md#v100-in-progress)

### 1. Fixed 5-minute disconnect timeout (`DeviceDisconnectedTimeout`)

- **Works today.** For normally connected devices, operators can use the five-minute timeout to identify a missed check-in.
- **Feature limit.** A battery-powered device that reports only on wake-up would appear `Unknown` after five minutes of sleep, even if its next check-in were due hours later. Waking it just to send status would defeat the planned sleep schedule.
- **Server change.** Flight Control decides when a device is overdue. It needs to honor an expected contact interval, configured per fleet or device or reported by the agent.

### 2. An enrollment certificate cannot be revoked

- **Works today.** Operators can issue a short-lived certificate per batch and approve devices from an allow list. A leaked certificate can still submit enrollment requests until expiry.
- **Feature limit.** Deleting the CertificateSigningRequest leaves a leaked certificate usable. Operators have no way to revoke it immediately on 1.4.0.
- **Server change.** The 1.4.0 enrollment endpoint checks the signer and expiry. On Flight Control’s main branch, an operator can revoke the certificate through its CertificateSigningRequest, and enrollment checks the CSR’s approval and revocation status (`CSRStatusAllowsEnrollment`). Operators need a server release containing that API. On 1.4.0, running `tools/enrollment.sh` issues a certificate and leaves earlier certificates valid until expiry.

### 3. A device with an RSA key cannot enroll

- **Works today.** The ESP32-S3, C3 and C6 can enroll with an ECDSA device key kept in storage.
- **Feature limit.** Their Digital Signature peripheral signs only with RSA. Approval of an enrollment request using that key fails with HTTP 400, so the agent keeps this hardware-key option disabled.
- **Server change.** Flight Control's ECDSA certificate authority copies the RSA request's signature algorithm into the certificate template (`internal/crypto/internal_ca.go`). Leaving `SignatureAlgorithm` unset would let the authority certify the device's RSA public key using its own ECDSA signing key.

### 4. A fleet's OS image cannot come from the software catalog

- **Works today.** Operators deploy firmware by setting `os.image` directly. Catalog references also work for apps, as verified with the `alarm` app.
- **Feature limit.** Selecting Zephyr firmware through the fleet’s `spec.template.spec.os.catalogItemRef` gave the device an empty `os` field. The agent had no image to fetch, so the device could appear `UpToDate` while still running the old firmware.
- **Server change.** When building the device spec from the fleet template, `assembleDesiredSpec` copies only `os.image` (`internal/tasks/desired_spec.go`). Flight Control needs to carry the firmware catalog reference through and resolve it to an image the agent can fetch, as it does for apps.

### 5. A device cannot enroll again under its own name

- **Works today.** Routine renewal preserves identity. If the management certificate expires offline, a device with a valid enrollment credential can re-enroll under a new key and name.
- **Feature limit.** The returning device appears under a new record. Its old record remains until an operator deletes it, so the operator loses continuity under the original name.
- **Server change.** The enrollment handler rejects any request for an existing device name with `ErrDuplicateName`. Flight Control needs a way for an operator to approve re-enrollment against the existing record.

---

## Non-blocking for agent v1.0.0

### 6. `matchExpressions` in the fleet selector is accepted but ignored

- **Impact.** The fleet controller ignores `matchExpressions`. A selector using only expressions can leave a fleet empty without informing the operator (`internal/tasks/fleet_selector.go`).
- **Today.** The supplied fleet definitions use `matchLabels`. For example, the ESP-IDF fleet selects devices labeled `app=alarm`. [Fleet definition](../tools/fleet/idf-lab.yaml)
- **Why v1.0 can proceed.** Exact label matches let operators target the intended devices and roll out a fleet spec. More expressive selectors would require controller support.

<a id="7-batch-success-is-based-only-on-renderedversion"></a>

### 7. Batch success can precede firmware confirmation

- **Impact.** Flight Control counts a device as successful when its template and rendered versions match the rollout. If the device reports the new `renderedVersion` at download time, the next batch can start before the first device confirms a working image.
- **Today.** The agent keeps reporting the previous version while the new firmware is awaiting confirmation. It reports the new version only after confirmation.
- **Why v1.0 can proceed.** The server therefore sees a successful firmware rollout step only after the agent reaches the required state. Documenting this reporting rule in the API would help other agents implement it.

### 8. No way to declare the device class or the supported runtimes

- **Impact.** Flight Control lets operators assign `compose`, `quadlet` or `helm` workloads to an MCU. It also accepts Podman fields such as ports and volumes that the MCU cannot use.
- **Today.** Operators deploy WASM images with `appType: container`. The agent checks the app type on the device and reports an error for unsupported types. [MCU apps](APPS.md)
- **Why v1.0 can proceed.** Supported WASM apps can be deployed and updated. A runtime declaration in `status.systemInfo` would let the server catch incompatible workloads before they reach the device.

### 9. No free-form status field per app

- **Impact.** An operator looking at the standard per-app status can see the lifecycle state, but `DeviceApplicationStatus` has no message field for an app reading such as `calibrating`.
- **Today.** The agent publishes the text supplied by `fc_set_state` under `status.systemInfo.customInfo["app.<name>"]`, keyed by app name. [App state reporting](APPS.md#where-the-data-goes)
- **Why v1.0 can proceed.** Operators can already read each app's message alongside the device status. A dedicated per-app field would change where they find it.

### 10. Cost of each status update

- **Impact.** Each status PUT sends 2–3 KB of JSON, even for a heartbeat with no state change. This consumes bandwidth on constrained links.
- **Today.** The agent sends full status on its heartbeat and groups app notifications so they trigger at most one status update every 15 seconds.
- **Why v1.0 can proceed.** Flight Control receives both liveness and state changes with the existing API. Smaller updates would save bandwidth. The API already accepts JSON Patch (`patchDeviceStatus`), and a heartbeat-only request could reduce traffic further.

### 11. The server compares `os.image` only as a string

- **Impact.** Publishing different firmware under the same tag leaves `os.image` unchanged. Devices can keep running the old firmware even though the registry now serves a new image for that tag.
- **Today.** Operators change the fleet's `os.image` for each release, using a new version tag or pinning the new digest (`@sha256:…`).
- **Why v1.0 can proceed.** A changed reference triggers the normal firmware rollout. Detecting a change behind a reused tag would require the server to compare image digests.

<a name="label-sync"></a>

### 12. A change in the status is a label event

- **Impact.** The default `custom-info` mapping turns changing heap, uptime and traffic counters into label writes. Flight Control also requests label reconciliation whenever status changes beyond `lastSeen` (`deviceStatusesEqual` in `internal/service/common/device.go`).
- **Today.** The supplied mapping copies stable fields such as board and device class. Operators can select devices with `custominfo.flightctl.io/deviceClass=zephyr` and must preserve managed labels when editing a device to avoid HTTP 409. The agent updates the `Updating` timestamp only when the condition changes and samples counters at startup, after five minutes and then hourly. [Fleet label mapping](../tools/fleet/labelsync-custom-info.yaml)
- **Why v1.0 can proceed.** Fleet selection and app state reporting still work. On a simulated device with no app and one status report per minute, the changes reduced reconciliation events from 10 to 2 over nine minutes. Other status changes can still trigger reconciliation even when labels stay the same.

### 13. An inline file given in base64 reaches the device percent-encoded

- **Impact.** An inline base64 file such as registry credentials can arrive with percent escapes. For example, `aGk=` arrives as `aGk%3D`. The Linux agent's base64 decoder rejects `%` according to its source. The file delivery test covered the MCU agent.
- **Today.** The MCU agent removes the escapes before base64 decoding. The provisioning tools supply plain text, which Flight Control encodes correctly. [Registry configuration](REGISTRY.md#fleet-configuration)
- **Why v1.0 can proceed.** Both input forms deliver usable files on the MCU. Decoding the data URL from `SetFile` in the server's `ignitionConfigToRenderedConfig` would remove the need for the agent workaround.

### 14. A withdrawn advisory stays on the devices

- **Impact.** A withdrawn advisory can still count against a device, its fleet and the organization. Flight Control's `UpsertFindings` inserts and updates findings but leaves entries that Trustify has removed.
- **Today.** The agent reports the running image digest. It cannot clear a server finding. In the test, the withdrawn advisory remained after two syncs and disappeared from the device's list only after it moved to another release. [Advisory test](SBOM.md#what-was-verified-with-a-test-advisory)
- **Why v1.0 can proceed.** The reported digest identifies the running release and its SBOM, and matching advisories reach the fleet view. Cleaning up withdrawn findings requires the server to reconcile each digest against the latest scan.

### 15. A console session whose user left before the device came stays listed

- **Impact.** If the user leaves before the device joins (after 60 s for a CLI timeout or 50 s in the UI's Logs tab), the stale session keeps the server answering spec requests immediately. This increases management traffic.
- **Today.** The agent remembers the last 64 sessions it opened across restarts, closes an unjoined session after one minute and switches from 5-second to 15-second polling after only remembered sessions have been listed for a minute. [Console behavior](CONSOLE.md#how-it-works)
- **Why v1.0 can proceed.** Operators can still open a new console session, and a remembered session cannot replay its command. Extra polling continues until the stale entry clears. The server could remove it when the user leaves or give it an expiry.
