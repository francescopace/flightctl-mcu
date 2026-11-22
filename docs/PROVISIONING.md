# Provisioning

Provision the Flight Control credentials at the factory. Supply Wi-Fi there too, or let an installer set it at the site.

## At the factory, over USB

`tools/provision.py` writes the CA, enrollment certificate and key, image-signing **public key**, agent endpoint and labels to `fc_creds`. Each device generates its own management key on first boot. For a P-256 key, the Flight Control device name is `lower(base32hex_nopad(sha256(X || Y)))`. [Single-board example](QUICKSTART.md#4-provision-it)

| Flag | Default | Effect |
|---|---|---|
| `--port <port>` | — | Writes the partition to the board on that serial port. Give exactly one of `--port`, `--image` and `--out`. |
| `--image <file>` | — | Writes the partition into a merged flash image. |
| `--out <file>` | — | Writes the partition alone to a file. |
| `--format` | `esp-idf` | Partition format: `esp-idf`, `zephyr` or `nuttx`. |
| `--board <board>` | `native_sim_native_64` with `--format zephyr` | Zephyr board, or NuttX board profile. |
| `--secrets <dir>` | `.secrets` | Directory holding the CA, enrollment certificate and key, and `signing.pub`. |
| `--server <host[:port]>` | `agent-api.<your-ip>.nip.io:8443` | Agent endpoint. |
| `--labels <k=v,k=v>` | `site=lab,app=alarm` | Labels written to `fc_creds`. |
| `--wifi-ssid <ssid>` | `$WIFI_SSID` | Factory Wi-Fi network. Without it the device uses the Wi-Fi it was built with. |
| `--wifi-password <password>` | `$WIFI_PASSWORD` | Password of that network. |
| `--pop <secret>` | `$PROV_POP` | Proof of possession for [field Wi-Fi setup](#in-the-field-the-wifi-network). |

`port/espidf/sdkconfig.local`, when present, supplies the default endpoint and labels. With `--format zephyr`, `--port` works on Espressif boards only. With `--format nuttx`, `--pop` is refused and Arm boards take `--out` or `--image`.

Both output formats contain the enrollment key and any provisioned Wi-Fi password in clear text on the provisioning host. The tool writes `--out` and `--image` with owner-only permissions (0600). Patching an image requires temporary space for a second copy in the same directory. Store provisioned images on encrypted storage.

### One enrollment certificate per batch

1. Generate the batch enrollment credential.

   ```bash
   BATCH=2026-11-a EXPIRATION=30d tools/enrollment.sh .secrets/batch-2026-11-a
   ```

2. Place the publisher's `signing.pub` in that same directory before provisioning. Without it the provisioning script warns that OCI signatures will not be checked.

3. For a batch with field Wi-Fi setup, provision the merged image with a proof of possession. For a secure build, follow the [factory procedure](SECURITY.md#factory-procedure).

   ```bash
   tools/provision.py --image port/espidf/build-esp32c3-secure/merged-binary.bin --secrets .secrets/batch-2026-11-a --pop <secret>
   ```

   The tool reports `signing key yes` and `written into` with the image path.

Choose a certificate lifetime that covers production and the longest expected offline interval before enrollment or re-enrollment. Use an allow list to approve devices. A leaked credential can submit requests until expiry on Flight Control 1.4.0. [Revocation limits](FLIGHTCTL-LIMITATIONS.md#upstream-blockers-for-agent-v100)

![Pending enrollment request](img/enrollment-pending.png)
<img src="img/enrollment-approve.png" alt="Reviewing device labels before approval" width="420">

## In the field: the WiFi network

Field setup is available on ESP-IDF and Zephyr. For NuttX, `tools/provision.py` refuses `--pop`.

Set `--pop` to an 8–63 character proof of possession and give it to the installer. Build ESP-IDF with `LOCAL=0` and leave `WIFI_SSID` unset during USB provisioning. Existing Wi-Fi driver credentials can still cause a device to try its saved network.

1. Join `FC-xxxxxx` (last six MAC digits) using the proof as the access-point password.
2. In **ESP SoftAP Prov** on iOS or Android, select the site network and enter its password. From a laptop on the access point, use the CLI instead.

   ```bash
   tests/e2e/esp-prov.sh 192.168.4.1:80 --pop <secret> --ssid <site network> --passphrase <password>
   ```

3. Wait for the connection result. On ESP-IDF a failed join leaves the access point open so you can correct the password. After a successful join the device saves the network, closes the access point and enrolls.

ESP-IDF and Zephyr use Espressif Security 1 (X25519 authenticated by the proof of possession, then AES-CTR). A QR label can encode `{"ver":"v1","name":"FC-xxxxxx","pop":"<secret>","password":"<secret>","transport":"softap","security":1}`. The iOS app requires a numeric `security` value.

ESP-IDF keeps the network in `fc_creds` or in encrypted storage on secure builds. Reprovision with `--wifi-ssid` to replace it. For development, `sdkconfig.wifi` and `sdkconfig.local` can supply defaults. Provisioned values take precedence. Field setup changes Wi-Fi only. Update the management endpoint or labels over USB.

## On Zephyr

Use `tools/provision.py --format zephyr`. A Zephyr device uses the factory network, a previously saved field network, or the access-point procedure above. Radios supporting simultaneous station and AP mode report success or failure before closing the AP, with 5 s for the final response. Other radios close the AP to try the network and reopen it after three failures.

Field credentials are stored in clear in the storage partition. Replacing the provisioning credentials or erasing storage clears the saved field network. [Zephyr build settings](ZEPHYR.md#on-an-esp32-s3) · [Phone provisioning results](#enrollment-timings)

## Enrollment timings

| Operation | Target | Environment | Time (s) | Firmware |
|---|---|---|---:|---|
| Boot to enrollment request visible in API, three clean starts | ESP32-S3 | Emulation | 7.3–10.2 | not stated |
| Boot to enrollment request visible in API, simulated network | Zephyr `native_sim` | Emulation | 0.6 | not stated |
| Wi-Fi provisioning from a phone: join | C6 | Physical board, earlier test | ~2 | not stated |
| Wi-Fi provisioning from a phone: access point closed after the join | C6 | Physical board, earlier test | ~8 | not stated |
| Wi-Fi provisioning from a phone: join | Zephyr S3 | Physical board, earlier test | 8 | not stated |
| Wi-Fi provisioning from a phone: app running after access point closure | Zephyr S3 | Physical board, earlier test | 1 | not stated |
| Wi-Fi provisioning: installer | C3 | Emulation | 6.6 | not stated |
| Decommissioning: device key and certificate erased, resident app still running | Both ports | Physical boards, earlier tests | 2.4–3 | not stated |
| Decommissioning: wipe began | C3 | Emulation | 3.6 | not stated |
| Decommissioning: unenrolled status reported after reboot | C3 | Emulation | 7.2 | not stated |
| Decommissioning: wipe and unenrolled status both reported | Zephyr `native_sim` | Emulation | ≤ 6.1 | not stated |

The Zephyr radio simulation passed immediate and deferred joins.
