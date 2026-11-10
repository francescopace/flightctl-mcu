# Provisioning

A device needs two things to join a fleet: credentials for Flight Control,
which belong to the installation, and a network, which belongs to the site.
The firmware has neither built in: one image serves every installation.

## At the factory, over USB

`deploy/provision.py` writes the `fc_creds` partition: the CA, the enrollment
certificate and key, the image signing key, the agent endpoint and the
labels, and optionally the WiFi network. One partition serves a whole batch:
each device generates its own key on its first boot. Into a merged image, to
flash a batch:

```bash
deploy/provision.py --image firmware/build-esp32c3-secure/merged-binary.bin --secrets .secrets/batch-2026-11-a --pop <secret>
```

### One enrollment certificate per batch

```bash
BATCH=2026-11-a EXPIRATION=30d deploy/enrollment.sh .secrets/batch-2026-11-a
```

A leaked enrollment certificate lets anyone create enrollment requests until
it expires: Flight Control 1.3.1 cannot revoke one (it checks only the signer
and the expiry). A certificate per batch with a short lifetime limits that;
approving devices from an allow list (`deploy/auto-approve`) decides who
actually gets in. The lifetime must cover the factory window and the longest
a device may stay offline before it enrolls, or enrolls again after its
management certificate expired.

## In the field: the WiFi network

When the site's network is not known at the factory, provision the device
with a proof of possession instead of a network (`--pop`, 8 to 63
characters, printed on the device's label or its box). With no network, the
device opens an access point named `FC-xxxxxx` (the end of its MAC) and waits
for an installer:

- with Espressif's provisioning app (ESP SoftAP Prov, iOS and Android): join
  `FC-xxxxxx` with the proof of possession as its password, then pick the
  site's network;
- with ESP-IDF's `esp_prov`, from a laptop on that access point:

  ```bash
  python $IDF_PATH/tools/esp_prov/esp_prov.py --transport softap --service_name 192.168.4.1:80 --sec_ver 1 --pop <secret> --ssid <site network> --passphrase <password>
  ```

The session is ESP-IDF's provisioning Security 1: an X25519 key exchange,
authenticated by the proof of possession, then AES-CTR; a wrong proof is
refused before anything is sent. The network is checked: if the device cannot
join it, the installer is told and can send another. Once the device is on
the network the access point closes, and the device enrolls.

The network is kept by the WiFi driver, in the default NVS partition (under
flash encryption in the secure variant). The device uses it from then on;
erasing the flash brings the access point back. A network given at the
factory (`--wifi-ssid`) always wins, and is not stored by the driver.

`CONFIG_FC_FIELD_PROVISIONING` (on by default) adds the provisioning manager,
its HTTP server and protocomm: about 128 KB of flash.

### Over BLE

The `ble` variant does the same over Bluetooth Low Energy instead of an
access point, which suits phones better and needs no WiFi hop:

```bash
TARGET=esp32c3 VARIANT=ble firmware/build.sh
```

(`VARIANT=secure-ble` with the secure variant.) The device advertises
`FC-xxxxxx` with ESP-IDF's provisioning service; the installer uses
Espressif's provisioning app (ESP BLE Prov) or `esp_prov --transport ble
--service_name FC-xxxxxx`, with the same proof of possession and the same
Security 1 session. NimBLE runs only while the device has no network: its
memory is given back once provisioning is over, or at boot on a device that
has its network. It adds about 230 KB of flash (1.54 MB on the C3, 1.58 MB
secure, in a 1.7 MB slot). Not on the ESP32-S2, which has no Bluetooth.

## What the CI checks

The plain emulator run (`ci/e2e-emu.sh`) provisions its device in the field:
the image has a proof of possession and no network, the emulator joins the
device's access point, `ci/esp-prov.sh` tries a wrong proof (refused) and then
the right one with the emulator's network, and the device carries on to
enroll. The end-to-end test also takes a one-day enrollment certificate for
the run, as for a batch.

An extra CI job does it over BLE (`PROV=ble ci/e2e-emu.sh`, C3, ble variant):
`ci/ble-prov.py` runs Google's Bumble as the emulator's BLE controller and as
the installer's phone, and ESP-IDF's esp_prov code for the session. A wrong
proof is refused there too.

Not done: setting the Flight Control endpoint or labels in the field.
