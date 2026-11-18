# Flight Control: limitations found

The gaps found in Flight Control for microcontrollers, with what the source shows, how each was found, and a proposal. The numbers are the same as in the short list of [RESULTS.md](RESULTS.md#flight-control-limitations-found-upstream-proposals), which other pages cite.

## 1. `matchExpressions` in the fleet selector is accepted but ignored

- The fleet stays empty with no error. The controller only uses `matchLabels` (`internal/tasks/fleet_selector.go`).
- Proposal: support it, or reject it at validation.

## 2. Batch success is based only on `renderedVersion`

- An agent that reports the version before confirming the new OS makes the rollout advance. The Linux agent avoids this implicitly; this agent keeps reporting the previous version until the new firmware is confirmed.
- Proposal: write this requirement into the agent API contract, or base success also on `os.image` and the `Updating` condition.

## 3. Fixed 5-minute disconnect timeout (`DeviceDisconnectedTimeout`)

- Battery devices in deep sleep would be `Unknown` most of the time.
- Proposal: make it configurable per fleet or per device, or let the device declare its expected contact interval.

## 4. No way to declare the device class or the supported runtimes

- For an MCU the server also accepts `compose`, `quadlet` or `helm` apps, and Podman fields (ports, volumes, `runAs`) that mean nothing there. The error only surfaces on the device.
- Proposal: let the device report something like `applicationRuntimes: [wasm]` (in `status.systemInfo`, where 1.4 moved `osMode` from `status.capabilities`), and validate rendering against it.
- Possibly also add an explicit `appType: wasm`, or recognize images with platform `wasip1/wasm` as WASM.

## 5. No free-form status field per app

- `DeviceApplicationStatus` has no field such as `message` or `info`.
- The app state ended up in `customInfo` (`app.<name>`).

## 6. Cost of each status update

- Every PUT sends the full status, 2–3 KB of JSON. The agent API also takes a JSON Patch of the status (`patchDeviceStatus`), which the Linux agent uses for urgent changes, but nothing documents it as the way for a constrained device, and there is no heartbeat-only variant.
- Proposal: document PATCH as the recommended path for constrained devices, or add a heartbeat-only variant.

## 7. The server compares `os.image` only as a string

- Re-pushing the same tag is not seen as an update.
- Proposal: recommend digest references (`@sha256:`) for firmware and apps, or compare `imageDigest`.

## 8. An enrollment certificate cannot be revoked

- The enrollment endpoint checks only the certificate's signer and expiry (`internal/api_server/middleware/enrollment_auth.go`). One leaked from a batch of devices can enroll new devices until it expires; deleting its CertificateSigningRequest changes nothing.
- Workaround: one certificate per batch with a short lifetime (`BATCH=… EXPIRATION=… tools/enrollment.sh`), and approval from an allow list.
- Proposal: a revocation list for enrollment certificates, or refuse those whose CertificateSigningRequest was deleted or denied.
- Fixed on Flight Control's main branch, not in 1.4.0: enrollment is refused when the certificate's CertificateSigningRequest is revoked or not approved (`CSRStatusAllowsEnrollment`, with an API to revoke it). It waits for a release.

## 9. A device with an RSA key cannot enroll

- The internal CA builds the certificate with the request's signature algorithm (`internal/crypto/internal_ca.go`: `SignatureAlgorithm: csr.SignatureAlgorithm`). An RSA request asks for SHA256WithRSA, the CA's key is ECDSA, and Go's `x509.CreateCertificate` refuses: "requested SignatureAlgorithm does not match private key type". Approving the request fails with a 400.
- It matters for keys in hardware: the ESP32 DS peripheral signs only with RSA.
- Proposal: leave `SignatureAlgorithm` unset in the template, so the CA signs with its own algorithm whatever the device key.

## 10. Since 1.4, a change in the status is a label event

- The default `custom-info` mapping copies all of `customInfo` into labels, and the service logs "label reconciliation was requested" at every change in the status (`internal/service/common/device.go`: `deviceStatusesEqual`), even when no label changes ([below](#label-sync-in-detail)).
- `customInfo` is the only free-form place in the status, so a constrained device's counters end up there (limitation 5).
- Proposal: ask for a reconciliation only when a mapping's inputs changed, and do not copy all of `customInfo` by default; or give counters a place of their own.

## 11. A fleet's OS image cannot come from the software catalog

- The fleet accepts `spec.os.catalogItemRef` and keeps it in its template version, but the device gets `os: {}`: building the device's spec from the template copies `os.image` only (`internal/tasks/desired_spec.go`: `assembleDesiredSpec`). The device is reported `UpToDate` and its firmware never changes, with no error.
- Tried with a catalog for the Zephyr firmware and the `alarm` app. The app's `catalogItemRef` works: the server turned the catalog's version into the app's image reference, and the agent got the image reference as before.
- Proposal: copy `os.catalogItemRef` too, as is done for the applications.

## 12. An inline file given in base64 reaches the device percent-encoded

- The server keeps inline files as data URLs (`pkg/ignition/ignition_wrapper.go`: `SetFile`). For content already in base64 it sets the URL's encoding to ASCII, which percent-encodes the text. Building the rendered spec (`internal/tasks/device_render.go`: `ignitionConfigToRenderedConfig`) then cuts off the `data:...,` prefix and serves what is left as base64, escapes included. `aGk=` arrives as `aGk%3D`.
- The Linux agent decodes with Go's `base64.StdEncoding` (`api/core/v1beta1/types.go`: `decodeContents`), which refuses `%`. Base64 content whose length is not a multiple of three, which is most of it, should then fail to be written (read in the source, not tried with the Linux agent).
- Found with an `auth.json` for the registry ([REGISTRY.md](REGISTRY.md)). Content given as text is turned into proper base64 by the same code and arrives intact. This agent undoes the escapes before decoding, and the tools give text.
- Proposal: unescape the data URL's text (or use the URL's own decoding) before serving it as base64.

## 13. A device cannot enroll again under its own name

- An enrollment request for a name that already has a device is refused (`internal/service/enrollmentrequest/handler.go`: `ErrDuplicateName`). A device whose management certificate expired while it was offline can only enroll again under a new key, hence a new name; the old record stays until an operator deletes it.
- Proposal: let an enrollment certificate re-enroll a known device, with the same approval as a new one.

## 14. A withdrawn advisory stays on the devices

- The vulnerability sync only inserts and updates findings (`internal/store/vulnerabilityfinding/store.go`: `UpsertFindings`); nothing removes one that Trustify no longer returns.
- Found with a test advisory deleted from Trustify ([SBOM.md](SBOM.md#what-was-verified-with-a-test-advisory)): two syncs later Flight Control still counted it. It went away once the device moved to another release: the device's, fleet's and organization's lists count only the images the devices run.
- Proposal: replace a digest's findings with what the scan returns for it.

## 15. A console session whose user left before the device came stays listed

- A session is listed in the device's annotation (`device-controller/console`) when it starts, and taken out only when the user's websocket closes after the device joined (`internal/console/console.go`: `CloseSession`). If the user leaves first (`flightctl console` gives up after 60 s, the UI's Logs tab after 50 s), nothing takes it out. While any session is listed the rendered spec is served at once, with no long-poll (`internal/store/model/device.go`), so the device asks for its spec every few seconds, for good.
- Seen on boards: sessions asked for while a board was restarting stayed listed. This agent remembers the last 64 sessions it opened (it never opens one twice), parses only the list of sessions while the spec is unchanged, ends a session nobody joins within a minute, and polls every 15 s instead of 5 once only sessions it has seen are listed.
- Proposal: take a session out when its user leaves, joined or not, and give listed sessions a lifetime.

All of these were read again in the source of 1.4.0 and of Flight Control's main branch of 12 November 2026: they hold in both, except limitation 8, fixed on main. Limitation 15 was read in main's source and seen with 1.4.0.

## Label sync in detail

Flight Control 1.4 installs `LabelSyncMapping` resources, and one of them copies every entry of `customInfo` into the device's labels (`custominfo.flightctl.io/<key>`). These devices report counters there: free heap, traffic, uptime. With the default, each status update rewrote the device's labels and logged two events ("label reconciliation was requested", "updated successfully (labels)").

`tools/fleet/labelsync-custom-info.yaml` copies only what stays the same between firmware updates: device class, board, firmware and OS version, security state. The labels then stop changing, and so does the second event. The first is logged whenever anything in the status changes (the service compares the whole status, `lastSeen` aside), before it knows nothing will come of it (limitation 10). So the agent keeps its status still: the `Updating` condition's time changes only with the condition, as in Kubernetes, and the counters are read at the start, five minutes later, then once an hour. What an app reports is not held back: an app whose state changes about twice a minute makes an event at each change.

The labels a mapping makes are its own: editing a device without them, to change another label, is refused (409, "cannot edit a managed label owned by a mapping"). An edit must carry them as they are.

The new labels also let a fleet select on what a device says it is, `custominfo.flightctl.io/deviceClass=zephyr` for instance, which is part of what limitation 4 asks for.
