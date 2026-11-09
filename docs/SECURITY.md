# Device security: Secure Boot, flash encryption, keys in hardware

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
| **Device key in the DS peripheral** (RSA-3072; not on the ESP32) | The device's TLS key is generated on the first boot and sealed into the Digital Signature peripheral, under an HMAC key burned into an eFuse block that software cannot read. The DS signs; nothing can read the key, not even the firmware. | Taking a device's identity from its memory, even with code running on it. |
| **Anti-rollback** | Each image carries a security version. Once a release is confirmed, the chip keeps its version in eFuse and refuses older ones, at update and at boot. | Putting back a signed firmware with a known vulnerability. |
| **Secure Boot key rotation** | The bootloader can trust up to three keys. A release signed with the next key retires the previous one on each device that confirms it. | A Secure Boot key that is lost or exposed. |

All this is fixed in the chip's eFuses on the device's first boot, and cannot
be undone: from then on the chip runs only images signed with your Secure Boot
key. **Lose that key and the devices can never be updated again.**

**It is a separate build, off by default.** `firmware/build.sh` without
`VARIANT=secure` builds the plain firmware, which never touches the eFuses: the
development boards and most of CI run it. The secure variant is built only on
purpose, with your Secure Boot key, in its own directory
(`firmware/build-<chip>-secure`), and pushed with its own `--variant`. A device
stays on the variant it was flashed with: each image carries the security
features it was built for, and the agent refuses an update built for others
before it is installed (`firmware built for another security variant`). A
secure build pushed to plain devices by mistake therefore changes nothing on
them; a plain device can become a secure one only by flashing it over USB.
Devices on firmware older than this check (v3.0) do not have it.

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

## Device key in hardware

On every chip with a DS peripheral (S2, S3, C3, C5, C6) the secure variant
keeps the device key there (`CONFIG_FC_DEVICE_KEY_DS`). On the first boot,
with no key yet, the agent:

1. generates an RSA-3072 key (about 3 s on the C3) and the CSR with it;
2. burns a random HMAC key into a free eFuse key block, with the DS purpose:
   software cannot read that block, the DS peripheral can use it;
3. encrypts the key's parameters with it (`esp_ds_encrypt_params`) and stores
   them with the CSR; the key itself is wiped;
4. has the peripheral sign, and checks the signature with the public key.

From then on TLS signs through the peripheral (RSA-PSS, as TLS 1.3 asks).
Flight Control names a device after its RSA key as it does after an ECDSA
one (`sha256` of the modulus). The device reports `deviceKey: "ds"`.

Limits:

- **One eFuse block per key.** A new key (a device that enrolls again after
  its certificate expired) takes another block. With Secure Boot (one or more
  blocks for the key digests) and flash encryption (one), two or three are
  left for that. When none is, or anything above fails, the key is made in
  software and kept in the encrypted store, as before (`deviceKey: "storage"`).
- **A key already in storage stays there**: devices keep their name.
- **The ESP32** has no DS peripheral: its key stays in the encrypted store.

## Anti-rollback

`SECURE_VERSION=<n>` when building gives the image a security version (0 by
default):

```bash
TARGET=esp32c3 FW_VERSION=v5 VARIANT=secure-release SECURE_VERSION=1 SECURE_BOOT_KEY=secure_boot_key.pem firmware/build.sh
```

When a device confirms a release, it burns that version into eFuse; from then
on it refuses an update below it (`firmware older than this device accepts`)
and the bootloader will not boot one. Raise it only for releases that fix a
vulnerability: the C3 has 16 versions, one eFuse bit each. The device reports
`secureVersion: "<this image>/<lowest accepted>"`.

## Key rotation

Secure Boot v2 trusts up to three keys, those that sign the bootloader when
the device first boots; the empty slots are revoked then. To keep a way to
replace the key, sign the factory bootloader with the next ones too:

```bash
firmware/merge-secure.py --extra-key secure_boot_key_1.pem --extra-key secure_boot_key_2.pem firmware/build-esp32c3-secure
```

Keep keys 1 and 2 offline, unused. Releases are signed with key 0. To retire
it, build the next release with `SECURE_BOOT_KEY=secure_boot_key_1.pem`: once a
device has confirmed that release, it revokes key 0 for good (after checking
that the bootloader and the running app verify without it), and from then on
accepts only images signed with key 1. The device reports
`secureBootKeys: "0*,1"` (usable slots, `*` for those that sign the running
image).

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
   `firmware/merge-secure.py` writes `merged-binary.bin` with it, signed with
   the spare keys too (see "Key rotation"):

   ```bash
   firmware/merge-secure.py --extra-key secure_boot_key_1.pem firmware/build-esp32c3-secure
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
   - burns the hashes of the Secure Boot keys, revokes the empty slots and
     turns Secure Boot on;
   - burns the image's security version;
   - generates the flash encryption key in the chip;
   - encrypts the bootloader, the partition table, `otadata`, `nvs_keys` and
     the app in place, then restarts.

   The app then generates the NVS keys and moves what `fc_creds` holds into
   the encrypted `nvs_store` (namespace `fc_creds`). Then it erases `fc_creds`
   and generates the device key in the DS peripheral (about 3 s), then
   enrolls. Until that first boot, `fc_creds` is in clear on the flash:
   provision in a trusted place.

5. **Check it in Flight Control.** The device reports what its chip enforces in
   `customInfo`: `secureBoot: "true"`, `flashEncryption: "release"`,
   `deviceKey: "ds"`, `secureBootKeys` and `secureVersion`.

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
- the plain release build signed with the device's own Secure Boot key is
  refused as another variant (when the test builds the images: CI's prebuilt
  ones come without their key). On the plain run, the plain device refuses
  the secure release build;
- the debug build (security version 0) is refused once the release (1) is
  confirmed (anti-rollback);
- the broken build is rolled back, as without Secure Boot;
- when the test builds the images: the bootloader trusts a second key, and a
  release signed with it alone retires the first (key rotation). CI's
  prebuilt images come without their keys.

The device key is in the DS peripheral throughout: the device enrolls and
talks to Flight Control with it.

Tried by hand in the emulator:

- the ROM refuses a bootloader whose signature block was altered, and one
  signed with another key;
- with a bootloader signed with keys 0 and 1, an app signed with key 0 boots
  (`secureBootKeys: 0*,1`); one signed with key 1 boots and revokes key 0
  (`1*`); one signed with an unknown key does not boot;
- the DS key: generated, sealed (eFuse `BLOCK_KEY3` after the two Secure Boot
  digests and the flash encryption key) and checked with an RSA-PSS
  signature; an image with security version 2 burns it on first boot.

## Not yet done

- **Against Flight Control.** The device key in the DS peripheral, the
  anti-rollback and the key rotation steps above have run in the emulator
  without a server; the end-to-end test with Flight Control (RSA enrollment,
  TLS 1.3 signed by the DS peripheral, the update steps) has not run yet.
- **Real boards.** Nothing has been burned yet. Before a batch: one board per
  chip, then a full OTA cycle.
- **ESP32 (classic).** Secure Boot v2 needs silicon revision 3
  (`CONFIG_ESP32_REV_MIN_3`, set by the variant), and the chip has no secure
  download mode: the build falls back to leaving the ROM download mode fully
  open (`CONFIG_SECURE_INSECURE_ALLOW_DL_MODE`), which can load code into RAM.
  For the field, `CONFIG_SECURE_DISABLE_ROM_DL_MODE` closes it, at the price
  of no provisioning over USB afterwards. The emulator does not run it.
- **Revoking a key that is not in use** (one of the spares, exposed): ESP-IDF
  can (`esp_ota_revoke_secure_boot_public_key`), the agent has no command for
  it yet.
- **Keys in an HSM.** The scripts sign with key files; ESP-IDF also supports
  signing with an external signature (`espsecure sign_data --pub-key`).
