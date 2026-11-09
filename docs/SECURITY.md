# Device security: Secure Boot, flash encryption, NVS encryption

A device in the field has to resist someone who holds it: who can read its
flash, or write a firmware of their own. The `secure` firmware variant
([firmware/sdkconfig.secure](../firmware/sdkconfig.secure)) turns on what the
chip offers against that:

| | What it does | Against |
|---|---|---|
| **Secure Boot v2** (RSA-3072) | The ROM checks the bootloader's signature, the bootloader checks the app's, and an OTA update is refused before it is installed if its signature does not match. | Running firmware not signed by you, by flashing it over USB or by pushing it to the registry. |
| **Flash encryption** (release mode) | The flash is encrypted with an AES-XTS key generated on the chip, which nothing can read back. JTAG is off. | Reading the firmware out of the flash chip. |
| **NVS encryption** | The NVS partitions are encrypted with keys held in the `nvs_keys` partition, itself under flash encryption. | Reading the device key, its certificate and the enrollment credentials out of the flash. |
| **Secure download mode** | The ROM serial loader can still write the flash, to provision the device again, but cannot read it or run code. | Using the USB port to dump memory or run code. |

All this is fixed in the chip's eFuses on the device's first boot, and cannot
be undone: from then on the chip runs only images signed with your Secure Boot
key. **Lose that key and the devices can never be updated again.**

The variant is tested on emulated chips (esp-emu) in CI; it has not been
burned on a real board yet.

## Two signing keys

They protect different things, and both belong offline (an HSM in production):

- the **image signing key** (`deploy/signing/keygen.sh`, ECDSA P-256) signs
  the OCI artifacts in the registry, firmware and apps, in cosign's format.
  The agent checks it before it downloads anything;
- the **Secure Boot key** (RSA-3072) signs the firmware binaries themselves.
  The ROM, the bootloader and the OTA code check it. A firmware is built
  signed with it:

```bash
espsecure generate-signing-key --version 2 --scheme rsa3072 secure_boot_key.pem
```

```bash
TARGET=esp32c3 FW_VERSION=v4 VARIANT=secure SECURE_BOOT_KEY=secure_boot_key.pem firmware/build.sh
```

`VARIANT=secure-release` gives the release profile of the same, and
`fw-push.py --variant secure-release` pushes it. An OTA update thus passes two
checks: the cosign signature of the artifact, then the Secure Boot signature of
the image.

## Flash layout

Signed, a bootloader with Secure Boot and flash encryption takes up to 48 KB
(ESP32-C5), more than fits below the default partition table at 0x8000. The
table is at 0x10000 in every build, secure or not
([firmware/partitions.csv](../firmware/partitions.csv)); the OTA slots did not
move. A board flashed with the earlier layout has to be flashed over USB and
provisioned again: an OTA update cannot change the partition table.

## Factory procedure

1. **Merged image.** With Secure Boot, ESP-IDF leaves the bootloader out of
   its flash arguments, so it is never flashed by accident.
   `firmware/merge-secure.py` writes `merged-binary.bin` with it:

   ```bash
   firmware/merge-secure.py firmware/build-esp32c3-secure
   ```

2. **Provision into the image**, so the board is flashed once and its first
   boot has everything:

   ```bash
   deploy/provision.py --image firmware/build-esp32c3-secure/merged-binary.bin --wifi-ssid <ssid> --wifi-password <password>
   ```

   One image serves a whole batch: `fc_creds` is the same for every device of
   an installation, and each device generates its own key on the chip.

3. **Flash.** `flash.sh` refuses a secure build unless `BURN_EFUSES` names the
   port, so that no board is locked by accident:

   ```bash
   BURN_EFUSES=/dev/cu.usbmodem1101 BUILD=build-esp32c3-secure firmware/flash.sh /dev/cu.usbmodem1101
   ```

4. **First boot**, about two seconds longer than usual. The bootloader:
   - burns the hash of the Secure Boot key and turns Secure Boot on;
   - generates the flash encryption key in the chip;
   - encrypts the bootloader, the partition table, `otadata`, `nvs_keys` and
     the app in place, then restarts.

   The app then generates the NVS keys and moves what `fc_creds` holds into
   the encrypted `nvs_store` (namespace `fc_creds`). Then it erases `fc_creds`
   and enrolls. Until that first boot, `fc_creds` is in clear on the flash:
   provision in a trusted place.

5. **Check it in Flight Control.** The device reports what its chip enforces in
   `customInfo`: `secureBoot: "true"` and `flashEncryption: "release"`.

To provision a device again later (new endpoint, new enrollment certificate),
write `fc_creds` again (`provision.py --port`): the next boot moves it into the
encrypted store, replacing what was there. Secure download mode allows the
write; esptool then needs `--no-stub`. Not yet tried on a real board.

## What the CI checks

The emulator runs the real ROM and models the eFuses, the XTS-AES flash
encryption and its decrypting cache. `SECURE=1 ci/e2e-emu.sh` (C3 and C5 in
CI, with a throwaway Secure Boot key):

- the first boot burns the eFuses, encrypts the flash and moves `fc_creds` into
  the encrypted store;
- the device enrolls and reports `secureBoot` and `flashEncryption: release`;
- an OTA update to the release build, signed with the same key, is installed;
- the same release build, signed with another Secure Boot key and properly
  cosign-signed, is refused, and the device stays on its firmware;
- the broken build is rolled back, as without Secure Boot.

Tried by hand in the emulator: the ROM refuses a bootloader whose signature
block was altered, and one signed with another key.

## Not yet done

- **Real boards.** Nothing has been burned yet. Before a batch: one board per
  chip, then a full OTA cycle.
- **ESP32 (classic).** Secure Boot v2 needs silicon revision 3
  (`CONFIG_ESP32_REV_MIN_3`, set by the variant), and the chip has no secure
  download mode: the build falls back to leaving the ROM download mode fully
  open (`CONFIG_SECURE_INSECURE_ALLOW_DL_MODE`), which can load code into RAM.
  For the field, `CONFIG_SECURE_DISABLE_ROM_DL_MODE` closes it, at the price
  of no provisioning over USB afterwards. The emulator does not run it.
- **Anti-rollback.** A signed older firmware can still be installed. ESP-IDF
  can refuse it (secure version in eFuse); left for when releases are numbered.
- **Device key in hardware** (roadmap 2.2). The key is encrypted at rest, but
  the CPU reads it in clear; with the DS peripheral it would never be readable.
- **Key rotation.** Secure Boot v2 holds up to three key hashes. The build
  signs with one key, and the first boot revokes the two empty slots: the key
  can never be replaced. Signing the bootloader with two or three keys at
  build time would keep a way to rotate.
