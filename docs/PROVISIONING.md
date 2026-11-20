# Provisioning

Provision the Flight Control credentials at the factory. Supply Wi-Fi there too, or let an installer set it at the site.

## At the factory, over USB

`tools/provision.py` writes the CA, enrollment certificate and key, image-signing **public key**, agent endpoint and labels to `fc_creds`. Each device generates its own management key on first boot. For a P-256 key, the Flight Control device name is `lower(base32hex_nopad(sha256(X || Y)))`. Use `--port` for one board, `--image` to provision a merged image, or `--out` for a partition file. [Single-board example](QUICKSTART.md#4-provision-it)

### One enrollment certificate per batch

Generate the batch enrollment credential. Place the publisher's `signing.pub` in that same directory before provisioning. Without it the provisioning script warns that OCI signatures will not be checked.

```bash
BATCH=2026-11-a EXPIRATION=30d tools/enrollment.sh .secrets/batch-2026-11-a
```

For a batch with field Wi-Fi setup, provision the merged image with a proof of possession. For a secure build, follow the [factory procedure](SECURITY.md#factory-procedure).

```bash
tools/provision.py --image port/espidf/build-esp32c3-secure/merged-binary.bin --secrets .secrets/batch-2026-11-a --pop <secret>
```

Choose a certificate lifetime that covers production and the longest expected offline interval before enrollment or re-enrollment. Use an allow list to approve devices. A leaked credential can submit requests until expiry on Flight Control 1.4.0. [Revocation limits](FLIGHTCTL-LIMITATIONS.md#upstream-blockers-for-agent-v100)

![Pending enrollment request](img/enrollment-pending.png)
<img src="img/enrollment-approve.png" alt="Reviewing device labels before approval" width="420">

## In the field: the WiFi network

Set `--pop` to an 8–63 character proof of possession and give it to the installer. Build ESP-IDF with `LOCAL=0` and leave `WIFI_SSID` unset during USB provisioning. Existing Wi-Fi driver credentials can still cause a device to try its saved network.

1. Join `FC-xxxxxx` (last six MAC digits) using the proof as the access-point password.
2. In **ESP SoftAP Prov** on iOS or Android, select the site network and enter its password. From a laptop on the access point, use the CLI instead.

   ```bash
   tests/e2e/esp-prov.sh 192.168.4.1:80 --pop <secret> --ssid <site network> --passphrase <password>
   ```

3. Wait for the connection result. On ESP-IDF a failed join leaves the access point open so you can correct the password. After a successful join the device saves the network, closes the access point and enrolls.

Both ports use Espressif Security 1 (X25519 authenticated by the proof of possession, then AES-CTR). A QR label can encode `{"ver":"v1","name":"FC-xxxxxx","pop":"<secret>","password":"<secret>","transport":"softap","security":1}`. The iOS app requires a numeric `security` value.

ESP-IDF keeps the network in `fc_creds` or in encrypted storage on secure builds. Reprovision with `--wifi-ssid` to replace it. For development, `sdkconfig.wifi` and `sdkconfig.local` can supply defaults. Provisioned values take precedence. Field setup changes Wi-Fi only. Update the management endpoint or labels over USB.

## On Zephyr

Use `tools/provision.py --format zephyr`. A Zephyr device uses the factory network, a previously saved field network, or the access-point procedure above. Radios supporting simultaneous station and AP mode report success or failure before closing the AP, with 5 s for the final response. Other radios close the AP to try the network and reopen it after three failures.

Field credentials are stored in clear in the storage partition. Replacing the provisioning credentials or erasing storage clears the saved field network. [Zephyr build settings](ZEPHYR.md#on-an-esp32-s3) · [Phone provisioning results](RESULTS.md#timings)
