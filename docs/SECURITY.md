# Device security: Secure Boot, flash encryption, keys in hardware

The ESP-IDF `secure` variant protects against physical access to the board. Its first boot permanently programs eFuses. Keep a trusted Secure Boot signing key available for the lifetime of the devices. Losing the last trusted key prevents further firmware updates.

The default ESP-IDF build leaves eFuses untouched. A plain device requires USB flashing to switch to secure firmware. The agent rejects OTA images for a different security variant and reports the reason until the fleet spec changes. The secure variant has been tested in emulation only. [What was verified](#what-was-verified) · [Build configuration](../port/espidf/sdkconfig.secure) · [Zephyr protections](#zephyr)

| Protection | What the secure build enforces |
|---|---|
| Secure Boot v2 | The ROM verifies the bootloader's RSA-3072 signature. The bootloader verifies the firmware. OTA also checks the firmware signature before installation. |
| Flash and NVS encryption | A chip-generated key protects flash. Encrypted NVS holds credentials. Flash encryption protects the NVS keys. JTAG is disabled. |
| Serial download | Secure download mode permits provisioning writes and blocks reads and code execution. [Classic ESP32 is an exception](#esp32-download-mode). |
| Device identity | On C5, the ECDSA peripheral signs with a key sealed in eFuse. Other supported chips use encrypted storage. [Hardware keys](#device-key-in-hardware) |
| Rollback and rotation | Confirmed releases can raise the minimum security version. C3, C5, C6 and S3 can also retire an old Secure Boot key. [Anti-rollback](#anti-rollback) · [Key rotation](#key-rotation) |

## By chip

What the secure build uses on each supported chip.

| Chip | Secure Boot v2 | Secure Boot keys trusted | Retire a key | Secure download mode | Device key | Key peripheral |
|---|---|---|---|---|---|---|
| ESP32 | revision 3 or later | 1 | no | no | encrypted storage | none |
| ESP32-C3 | yes | up to 3 | yes | yes | encrypted storage | RSA-only DS, disabled |
| ESP32-C5 | yes | up to 3 | yes | yes | sealed in eFuse, [with limits](#limits-with-either-peripheral) | ECDSA |
| ESP32-C6 | yes | up to 3 | yes | yes | encrypted storage | RSA-only DS, disabled |
| ESP32-S3 | yes | up to 3 | yes | yes | encrypted storage | RSA-only DS, disabled |

## Factory procedure

The example below provisions a C3 with one spare key. For a classic ESP32 revision 3 or later, use its own build directory and omit all spare keys. [Key rotation by chip](#key-rotation)

1. **Keys.** Generate the Secure Boot key and each planned spare. `secure_boot_key.pem` is the key that signs the build. [Two signing keys](#two-signing-keys) · [Keys in an HSM](#keys-in-an-hsm)

   ```bash
   espsecure generate-signing-key --version 2 --scheme rsa3072 secure_boot_key.pem
   ```

2. **Build.** Set `FW_TAG` to the firmware release tag before building with a Secure Boot key file. The build lands in `port/espidf/build-esp32c3-secure`.

   ```bash
   TARGET=esp32c3 FW_VERSION="${FW_TAG:?}" VARIANT=secure SECURE_BOOT_KEY=secure_boot_key.pem port/espidf/build.sh
   ```

   `VARIANT=secure-release` gives the release profile of the same, and `fw-push.py --variant secure-release` pushes it. [Security version](#anti-rollback) · [Signing a CI build](#signing-a-ci-build) · [Classic ESP32](#esp32-download-mode)

3. **Merged image.** ESP-IDF leaves the secure bootloader out of its flash arguments. Use `merge-secure.py` to include it in `merged-binary.bin`.

   ```bash
   port/espidf/merge-secure.py --extra-key secure_boot_key_1.pem port/espidf/build-esp32c3-secure
   ```

4. **Provision into the image**, so the board is flashed once and its first boot has everything:

   ```bash
   tools/provision.py --image port/espidf/build-esp32c3-secure/merged-binary.bin --wifi-ssid <ssid> --wifi-password <password>
   ```

   Check that provisioning reports `signing key yes` before flashing the batch. Boards in the same batch can share the provisioned image. Each device generates its own management key on the chip.

5. **Flash.** `flash.sh` refuses a secure build unless `BURN_EFUSES` names the port, so that no board is locked by accident:

   ```bash
   BURN_EFUSES=/dev/cu.usbmodem1101 BUILD=build-esp32c3-secure port/espidf/flash.sh /dev/cu.usbmodem1101
   ```

   The script prints `flashing build-esp32c3-secure (secure: bootloader included, eFuses are burned on first boot)`.

6. **First boot**, about two seconds longer than usual. The bootloader:
   - burns the hashes of the Secure Boot keys, revokes the empty slots and turns Secure Boot on;
   - burns the image's security version;
   - generates the flash encryption key in the chip;
   - encrypts the bootloader, the partition table, `otadata`, `nvs_keys` and the app in place, then restarts.

   The app then generates the NVS keys and moves what `fc_creds` holds into the encrypted `nvs_store` (namespace `fc_creds`). Then it erases `fc_creds` and enrolls. Until that first boot, `fc_creds` is in clear on the flash: provision in a trusted place.

7. **Check it in Flight Control.** The device reports what its chip enforces in `customInfo`: `secureBoot: "true"`, `flashEncryption: "release"`, `deviceKey` (`"ecdsa"` on the C5, `"storage"` elsewhere), `secureBootKeys` and `secureVersion`.

To provision a device again later (new endpoint, new enrollment certificate), write `fc_creds` again (`provision.py --port`): the next boot moves it into the encrypted store, replacing what was there. Secure download mode allows the write; esptool then needs `--no-stub`. [Not yet tried on a real board](#what-was-verified)

## Two signing keys

- The **OCI signing key** (ECDSA P-256, `tools/signing/keygen.sh`) signs firmware releases and WASM artifacts in cosign format. Provision the publisher’s `signing.pub` to enable verification before the agent downloads the payload. If the key is absent, provisioning warns and the agent skips OCI signature checks for both firmware and apps. [Provisioning credentials](PROVISIONING.md#at-the-factory-over-usb)
- The **Secure Boot key** (RSA-3072) signs the bootloader and firmware binary. The ROM and bootloader verify the boot chain, and the OTA path checks the firmware signature. Keep production signing keys offline or in an HSM. [Generating the key and building](#factory-procedure)

With the OCI public key provisioned, a firmware update passes both the OCI signature check and the Secure Boot image check.

### Signing a CI build

Each build directory contains a padded bootloader and application ready for signing. Use your own key. The first boot establishes which keys the chip trusts. For external signing, see [Keys in an HSM](#keys-in-an-hsm).

1. Unpack the CI artifact `firmware-<chip>-secure` under `port/espidf/`.

2. Sign the application and the bootloader. Run the commands from the repository root.

   ```bash
   B=port/espidf/build-esp32c5-secure-release
   espsecure sign-data --version 2 --keyfile secure_boot_key.pem --output $B/fw-signed.bin $B/flightctl_mcu_fw.bin
   espsecure sign-data --version 2 --keyfile secure_boot_key.pem --output $B/bl-signed.bin $B/bootloader/bootloader.bin
   mv $B/fw-signed.bin $B/flightctl_mcu_fw.bin && mv $B/bl-signed.bin $B/bootloader/bootloader.bin
   ```

3. Run `port/espidf/merge-secure.py "$B"` to generate the factory image. Publish the update with `tools/fw-push.py --variant secure-release`.

## Device key in hardware

ESP-IDF's PSA drivers expose a signing handle to TLS. It refers to the ECDSA eFuse block or to encrypted Digital Signature (DS) parameters.

### ESP32-C5: the ECDSA peripheral

With `CONFIG_FC_DEVICE_KEY_ECDSA`, the agent generates a P-256 key and CSR on first boot. It burns the private key into a free ECDSA eFuse block, verifies a signature made by the peripheral and wipes its RAM copy. It stores the CSR. Subsequent TLS operations use the peripheral. The device reports `deviceKey: "ecdsa"`.

### Other chips: the DS peripheral, off for now

S3, C3 and C6 have an RSA-only DS peripheral. Flight Control 1.4.0 cannot approve that enrollment, so `CONFIG_FC_DEVICE_KEY_DS` is disabled and secure builds keep an ECDSA key in encrypted storage. [RSA enrollment blocker](FLIGHTCTL-LIMITATIONS.md#upstream-blockers-for-agent-v100)

With a compatible CA and DS enabled, the agent generates an RSA-3072 key and CSR. It burns a random HMAC key into a protected eFuse block, encrypts the RSA parameters with `esp_ds_encrypt_params`, wipes the plaintext key and verifies a test signature. With a compatible server the device reports `deviceKey: "ds"` and uses RSA-PSS for TLS. Its name derives from the hash of the RSA modulus.

### Limits, with either peripheral

- Only the first hardware key can be provisioned. The build keeps eFuse read protection configurable until that key is burned, then locks it. A replacement identity uses encrypted storage (`deviceKey: "storage"`). The same fallback applies when no block is free or provisioning fails. [Re-enrollment through this fallback](#what-was-verified)
- The generated key is briefly present in RAM before sealing. A device already using a stored key retains it and keeps its name.

## Anti-rollback

`SECURE_VERSION=<n>` when building gives the image a security version (0 by default):

```bash
TARGET=esp32c3 FW_VERSION="${NEW_TAG:?set the new firmware tag}" VARIANT=secure-release SECURE_VERSION=1 SECURE_BOOT_KEY=secure_boot_key.pem port/espidf/build.sh
```

Confirmation burns the security version into eFuse. The agent and bootloader then reject images below it. Raise it for vulnerability fixes only. The C3 has 16 version bits. Read `secureVersion` as `<this image>/<lowest accepted>`.

## Key rotation

The classic ESP32 (revision 3 or later) stores a single Secure Boot key digest. Every release for that device must use its original key. [Espressif Secure Boot v2 guide for ESP32](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/security/secure-boot-v2.html)

On C3, C5, C6 and S3, first boot can trust up to three keys and revokes the unused slots. To allow rotation, sign the factory bootloader with the current key and each planned spare before first boot.

```bash
port/espidf/merge-secure.py --extra-key secure_boot_key_1.pem --extra-key secure_boot_key_2.pem port/espidf/build-esp32c3-secure
```

Keep the spare keys offline. Sign releases with key 0 until it needs retirement. Build the replacement with `SECURE_BOOT_KEY=secure_boot_key_1.pem`. Once the device confirms that release and verifies that the bootloader and running app work without key 0, it permanently revokes key 0. In `secureBootKeys: "0*,1"`, the asterisk marks a key that signs the running image.

A spare key that was exposed is revoked through the fleet's config, with a file the device reads as a command:

```yaml
config:
  - name: security
    inline:
      - path: /etc/flightctl-mcu/secure-boot.json
        content: '{"revokeKeys": [2]}'
```

Revocation is permanent and requires confirmed firmware. The agent rejects a request that would invalidate the running firmware or bootloader, or that targets a device without Secure Boot. Reapplying an already revoked slot succeeds.

## Keys in an HSM

The build signs with a key file (`SECURE_BOOT_KEY`). To keep the Secure Boot keys in an HSM instead, build without a key and sign afterwards.

1. Build without a key.

   ```bash
   TARGET=esp32c3 VARIANT=secure-release SECURE_BOOT_SIGNER=external port/espidf/build.sh
   ```

2. Sign with the HSM. The signing command receives a SHA-256 digest file and writes an RSA-PSS signature (SHA-256, 32-byte salt). The script checks it against the public key and appends the signature block to the bootloader and app.

   ```bash
   port/espidf/sign-external.py port/espidf/build-esp32c3-secure-release --pub-key key0.pub.pem --sign-cmd '<command> {digest} {signature}'
   ```

3. On chips that support rotation, repeat with `--bootloader-only` for each spare key.

## ESP32 download mode

Classic ESP32 lacks secure download mode. Its default secure build leaves the ROM serial loader open, including RAM execution. The `nodl` variant permanently disables the loader at first boot.

```bash
TARGET=esp32 VARIANT=secure-nodl-release SECURE_BOOT_KEY=secure_boot_key.pem port/espidf/build.sh
```

Provision `fc_creds` before first boot. Once `nodl` takes effect, USB reprovisioning and serial boot recovery are unavailable. On other chips, `nodl` also disables the loader.

## Flash layout

The signed C5 bootloader can reach 48 KB. All ESP-IDF builds place the [partition table](../port/espidf/partitions.csv) at 0x10000. OTA preserves the installed layout.

## Zephyr

MCUboot verifies each firmware image with its ECDSA P-256 key. The agent separately verifies the OCI release signature when the publisher’s public key is provisioned. To set the image security counter (default 0):

```bash
SECURE_VERSION=1 BOARD=esp32s3_devkitc/esp32s3/procpu port/zephyr/build.sh
```

The agent rejects an update below the running counter before reboot. MCUboot also checks the counter, which lives in flash. This protects against an older signed OTA image. Someone with physical access can rewrite flash over USB or SWD. [Counter rejection on hardware](#what-was-verified)

The shipped Zephyr S3 build stores the device key, credentials and field Wi-Fi in clear. Its MCUboot configuration enables neither chip Secure Boot nor flash encryption. For physical protection on Espressif, use the ESP-IDF secure variant. [Planned Zephyr security work](ROADMAP.md#v200-planned)

## Product compliance

For a product assessment, use the [SBOM tied to the release digest](SBOM.md), the protections for the variant you ship and the [signed firmware update results](DELTA-UPDATES.md#what-was-verified).

The manufacturer assesses the finished product against the requirements that apply to it. See the [Cyber Resilience Act, Article 13 and Annex I](https://op.europa.eu/en/publication-detail/-/publication/21b7d4eb-a6e2-11ef-85f0-01aa75ed71a1/language-en) and [ETSI EN 303 645](https://portal.etsi.org/webapp/WorkProgram/Report_WorkItem.asp?WKI_ID=69970).

## What was verified

CI means the [end-to-end workflow](TESTING.md#end-to-end-tests) checks it in the emulator on C5, C6 and S3. Manual means a check run by hand in the emulator. Pending means not verified yet. A dash means no result is recorded.

| Feature | Emulator | Board |
|---|---|---|
| First-boot eFuses, encrypted flash and migration of factory credentials | CI | pending |
| Signed OTA, rejected signatures and variants | CI | pending |
| Anti-rollback | CI | pending |
| Initial security version of 2 | manual | pending |
| Key rotation | CI | pending |
| Boot with two trusted keys | manual | pending |
| External signing: boot with two trusted keys, using OpenSSL in place of an HSM | manual | pending |
| Altered bootloader signatures | manual | pending |
| ECDSA device key on the C5 | CI | pending |
| DS key sealing | manual | pending |
| USB reprovisioning of a secure device | — | pending |
| Re-enrollment through the encrypted-storage fallback | untested | untested |
| Secure build on the classic ESP32 | no emulator coverage | pending |
| Zephyr: the security counter rejects an older image | — | verified on a board since removed, pending on a supported target |

## Not yet done

- Physical validation of the ESP-IDF secure variant is pending, including first boot, signed OTA and USB reprovisioning. [What was verified](#what-was-verified)
- The repository's OCI signing tool uses a key file. For KMS or PKCS#11 signing, use cosign.
