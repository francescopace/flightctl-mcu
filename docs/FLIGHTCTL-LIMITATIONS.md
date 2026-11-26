# Flight Control: limitations found

These findings were recorded against Flight Control 1.4.0. The blocker table also covers 1.4.1.

---

## Upstream blockers for agent v1.0.0

Flight Control 1.4.1 lacks all three fixes. As of 25 November 2026, RSA enrollment is the only one still awaiting merge into `main`. [Changes from 1.4.0 to 1.4.1](https://github.com/flightctl/flightctl/compare/v1.4.0...v1.4.1)

| Requirement | Status on `main` | Target Flight Control release | Upstream |
| --- | --- | --- | --- |
| 1. Revoke an enrollment certificate | Merged | 1.5 | [PR #3817](https://github.com/flightctl/flightctl/pull/3817) |
| 2. Enroll with an RSA device key | Pending merge | Unconfirmed | [PR #3940](https://github.com/flightctl/flightctl/pull/3940) |
| 3. Deploy a fleet OS image from the catalog | Merged | 1.5 | [PR #3937](https://github.com/flightctl/flightctl/pull/3937) |

---

## Non-blocking for agent v1.0.0

Upstream references were checked on 21 November 2026 against [`main` at `4917c9a`](https://github.com/flightctl/flightctl/tree/4917c9a88847e6cca6e1945a8ed94f653ef99278), the public upstream branches and GitHub issues and pull requests.

| # | Limitation | Upstream status | Upstream |
| --- | --- | --- | --- |
| 4 | [Re-enrollment after certificate expiry creates a new device record](#4-re-enrollment-after-certificate-expiry-creates-a-new-device-record) | Issue #3942 tracks recovery under the original identity. Open PR #3950 clarifies when the Linux agent starts renewal while its certificate is still valid. | [Maintainer response on #3942](https://github.com/flightctl/flightctl/issues/3942#issuecomment-7043342015) · [PR #3950](https://github.com/flightctl/flightctl/pull/3950) |
| 5 | [Fixed 5-minute disconnect timeout (`DeviceDisconnectedTimeout`)](#5-fixed-5-minute-disconnect-timeout-devicedisconnectedtimeout) | The same constant applies to every device on `main`. To distinguish scheduled sleep from a missed check-in, Flight Control would need to take the device's expected contact interval into account. | [Timeout definition](https://github.com/flightctl/flightctl/blob/4917c9a88847e6cca6e1945a8ed94f653ef99278/api/core/v1beta1/consts.go#L54) |
| 6 | [`matchExpressions` in the fleet selector is accepted but ignored](#6-matchexpressions-in-the-fleet-selector-is-accepted-but-ignored) | The API accepts `matchExpressions` from PR #582. Fleet ownership on `main` still reads only `matchLabels`. | [PR #582](https://github.com/flightctl/flightctl/pull/582) · [Fleet matching code](https://github.com/flightctl/flightctl/blob/4917c9a88847e6cca6e1945a8ed94f653ef99278/internal/tasks/fleet_selector.go#L589-L615) |
| 7 | [Batch success can precede firmware confirmation](#7-batch-success-can-precede-firmware-confirmation) | On `main`, PR #3866 also prevents devices with desired `spec.firmware` or an outstanding firmware attempt from counting as successful. MCU rollouts use `spec.os.image`, so their completion still depends on when the agent reports `renderedVersion`. | [PR #3866](https://github.com/flightctl/flightctl/pull/3866) · [Batch completion query](https://github.com/flightctl/flightctl/blob/4917c9a88847e6cca6e1945a8ed94f653ef99278/internal/store/device/store.go#L1515-L1547) |
| 8 | [No way to declare the device class or the supported runtimes](#8-no-way-to-declare-the-device-class-or-the-supported-runtimes) | On `main`, `status.systemInfo` can report OS mode, GPUs and KVM. Open PRs add capability checks for rendered configuration resources and one-time schedules. A list of supported application runtimes is still missing. | [PR #3645](https://github.com/flightctl/flightctl/pull/3645) · [PR #3920](https://github.com/flightctl/flightctl/pull/3920) · [PR #3936](https://github.com/flightctl/flightctl/pull/3936) |
| 9 | [No free-form status field per app](#9-no-free-form-status-field-per-app) | The upstream per-app schema still has no message field. | [Application status schema](https://github.com/flightctl/flightctl/blob/4917c9a88847e6cca6e1945a8ed94f653ef99278/api/core/v1beta1/openapi.yaml#L7801-L7851) |
| 10 | [The server compares `os.image` only as a string](#10-the-server-compares-osimage-only-as-a-string) | Upstream records observed runtime digests for delta updates. The renderer still copies a directly configured `os.image` into the spec, so republishing that tag alone does not trigger a fleet rollout. | [PR #3782](https://github.com/flightctl/flightctl/pull/3782) · [OS image rendering](https://github.com/flightctl/flightctl/blob/4917c9a88847e6cca6e1945a8ed94f653ef99278/internal/tasks/device_render.go#L521-L530) |
| 11 | [A change in the status is a label event](#11-a-change-in-the-status-is-a-label-event) | PR #3852 moved reconciliation to a separate queue and suppressed requests when only `lastSeen` changes. A change to other status fields can still enqueue reconciliation even when the mapped label values stay the same. | [PR #3852](https://github.com/flightctl/flightctl/pull/3852) |
| 12 | [An inline file given in base64 reaches the device percent-encoded](#12-an-inline-file-given-in-base64-reaches-the-device-percent-encoded) | The upstream renderer still strips the data URL prefix and copies the remaining bytes into the rendered file, including any percent escapes. | [Inline file rendering](https://github.com/flightctl/flightctl/blob/4917c9a88847e6cca6e1945a8ed94f653ef99278/internal/tasks/device_render.go#L1242-L1252) |
| 13 | [A withdrawn advisory stays on the devices](#13-a-withdrawn-advisory-stays-on-the-devices) | The upstream sync still skips empty scan results and upserts the findings returned for each digest. It has no removal step for a finding omitted from a later scan. | [Trustify scanner refactor](https://github.com/flightctl/flightctl/pull/3437) · [Scan reconciliation](https://github.com/flightctl/flightctl/blob/4917c9a88847e6cca6e1945a8ed94f653ef99278/internal/tasks/vulnerability_sync.go#L122-L147) |
| 14 | [A console session whose user left before the device came stays listed](#14-a-console-session-whose-user-left-before-the-device-came-stays-listed) | On `main`, the device-console handler returns HTTP 504 when the device fails to join within one minute, before reaching session cleanup. Open PR #3946 adds journal retrieval over the agent link. It leaves the device-console timeout path in place. | [Device-console timeout](https://github.com/flightctl/flightctl/blob/4917c9a88847e6cca6e1945a8ed94f653ef99278/internal/transport/v1beta1/console.go#L60-L87) · [PR #3946](https://github.com/flightctl/flightctl/pull/3946) |

Each section below gives the impact and what the agent or the operator does today.

### 4. Re-enrollment after certificate expiry creates a new device record

If the management certificate expires while the device is offline, its original name, labels and fleet association stay with the old record. With a valid enrollment credential, the MCU agent generates a new key and requests enrollment under a new name. Once enrollment is approved, the operator can apply the labels needed for fleet selection to the new record. Re-enrollment restores management of the device.

### 5. Fixed 5-minute disconnect timeout (`DeviceDisconnectedTimeout`)

Flight Control marks a device as `Unknown` after five minutes without contact, even when a battery-powered device is sleeping until its next scheduled check-in. For normally connected devices, operators use the five-minute timeout to detect a missed check-in. Fleet-scheduled sleep is planned for v2.0. [Roadmap](ROADMAP.md#v200-planned)

### 6. `matchExpressions` in the fleet selector is accepted but ignored

The fleet controller ignores `matchExpressions`. A selector using only expressions can leave a fleet empty without informing the operator (`internal/tasks/fleet_selector.go`). The supplied fleet definitions use `matchLabels`. For example, the ESP-IDF fleet selects devices labeled `app=alarm`. Exact label matches let operators target the intended devices and roll out a fleet spec. [Fleet definition](../tools/fleet/idf-lab.yaml)

### 7. Batch success can precede firmware confirmation

Flight Control counts a device as successful when its template and rendered versions match the rollout. If the device reports the new `renderedVersion` at download time, the next batch can start before the first device confirms a working image. The agent keeps reporting the previous version while the new firmware is awaiting confirmation. It reports the new version only after confirmation. The server therefore sees a successful firmware rollout step only after the agent reaches the required state.

### 8. No way to declare the device class or the supported runtimes

Flight Control lets operators assign `compose`, `quadlet` or `helm` workloads to an MCU. It also accepts Podman fields such as ports and volumes that the MCU cannot use. Operators deploy WASM images with `appType: container`. The agent checks the app type on the device and reports an error for unsupported types. Supported WASM apps can be deployed and updated. [MCU apps](APPS.md)

### 9. No free-form status field per app

An operator looking at the standard per-app status can see the lifecycle state, but `DeviceApplicationStatus` has no message field for an app reading such as `calibrating`. The agent publishes the text supplied by `fc_set_state` under `status.systemInfo.customInfo["app.<name>"]`, keyed by app name. Operators can already read each app's message alongside the device status. [App state reporting](APPS.md#where-the-data-goes)

### 10. The server compares `os.image` only as a string

Publishing different firmware under the same tag leaves `os.image` unchanged. Devices can keep running the old firmware even though the registry now serves a new image for that tag. Operators change the fleet's `os.image` for each release, using a new version tag or pinning the new digest (`@sha256:…`). A changed reference triggers the normal firmware rollout.

### 11. A change in the status is a label event

The default `custom-info` mapping turns changing heap, uptime and traffic counters into label writes. Flight Control also requests label reconciliation whenever status changes beyond `lastSeen` (`deviceStatusesEqual` in `internal/service/common/device.go`).

The supplied mapping copies stable fields such as board and device class. Operators can select devices with `custominfo.flightctl.io/deviceClass=zephyr` and must preserve managed labels when editing a device to avoid HTTP 409. The agent updates the `Updating` timestamp only when the condition changes and samples counters at startup, after five minutes and then hourly. Fleet selection and app state reporting still work. [Fleet label mapping](../tools/fleet/labelsync-custom-info.yaml) · [Measured reconciliation events](METRICS.md#management-traffic)

### 12. An inline file given in base64 reaches the device percent-encoded

An inline base64 file such as registry credentials can arrive with percent escapes. For example, `aGk=` arrives as `aGk%3D`. The Linux agent's base64 decoder rejects `%` according to its source. File delivery was tested on the MCU agent.

The MCU agent removes the escapes before base64 decoding. The provisioning tools supply plain text, which Flight Control encodes correctly. Both input forms deliver usable files on the MCU. [Registry configuration](REGISTRY.md#fleet-configuration)

### 13. A withdrawn advisory stays on the devices

A withdrawn advisory can still count against a device, its fleet and the organization. Flight Control's `UpsertFindings` inserts and updates findings but leaves entries that Trustify has removed. The agent reports the running image digest. It cannot clear a server finding. The reported digest identifies the running release and its SBOM, and matching advisories reach the fleet view. [Advisory test](SBOM.md#what-was-verified-with-a-test-advisory)

### 14. A console session whose user left before the device came stays listed

If the user leaves before the device joins (after 60 s for a CLI timeout or 50 s in the UI's Logs tab), the stale session keeps the server answering spec requests immediately. This increases management traffic.

The agent remembers the last 64 sessions it opened across restarts, closes an unjoined session after one minute and switches from 5-second to 15-second polling after only remembered sessions have been listed for a minute. Operators can still open a new console session, and a remembered session cannot replay its command. Extra polling continues until the stale entry clears. [Console behavior](CONSOLE.md#how-it-works)
