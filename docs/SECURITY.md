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
| **Device key in hardware** (ESP32-C5; off on the other chips, see below) | The device's TLS key is generated on the first boot and burned into an eFuse key block that only the chip's ECDSA peripheral reads. The peripheral signs; nothing can read the key, not even the firmware. | Taking a device's identity from its memory, even with code running on it. |
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
An image a device refuses (this, an older security version, the bootloader's
checks) is reported in its status and not downloaded again until the fleet's
spec changes: the device stays out of date with the reason, and its flash is
not written every minute.
Devices on firmware older than this check (v3.0) do not have it.

The variant is tested on emulated chips (esp-emu, which runs the chip's real
ROM) in CI. It has not been burned on a real board, and this project does not
plan to: a burn cannot be undone.

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

### ESP32-C5: the ECDSA peripheral

The secure variant sets `CONFIG_FC_DEVICE_KEY_ECDSA`, which takes effect on
chips with an ECDSA peripheral: the C5 among those built here. On the first
boot, with no key yet, the agent:

1. generates a P-256 key and the CSR with it;
2. burns the key into a free eFuse key block, with the ECDSA purpose:
   software cannot read that block, the peripheral signs with it;
3. has the peripheral sign, and checks the signature with the public key;
4. wipes its copy of the key and stores only the CSR.

From then on TLS signs through the peripheral. The key is of the same kind
as one kept in storage, and so is the device's name: Flight Control sees no
difference. The device reports `deviceKey: "ecdsa"`.

### Other chips: the DS peripheral, off for now

The S2, S3, C3 and C6 have a Digital Signature peripheral instead, which
signs only with RSA, and Flight Control 1.3.1 cannot enroll a device with an
RSA key: its internal CA copies the request's signature algorithm into the
certificate (`internal_ca.go`), and with its ECDSA key Go refuses to sign
("requested SignatureAlgorithm does not match private key type"). Found by
the end-to-end test. Until Flight Control fixes it (upstream item 9 in
RESULTS.md) these chips keep the device key in the encrypted store, and
`CONFIG_FC_DEVICE_KEY_DS` stays off. With a CA that signs RSA keys it can be
turned on. Then, on the first boot, the agent:

1. generates an RSA-3072 key (about 3 s on the C3) and the CSR with it;
2. burns a random HMAC key into a free eFuse key block, with the DS purpose:
   software cannot read that block, the DS peripheral can use it;
3. encrypts the key's parameters with it (`esp_ds_encrypt_params`) and stores
   them with the CSR; the key itself is wiped;
4. has the peripheral sign, and checks the signature with the public key.

TLS then signs through the peripheral (RSA-PSS, as TLS 1.3 asks). Flight
Control names a device after its RSA key as it does after an ECDSA one
(`sha256` of the modulus). The device reports `deviceKey: "ds"`.

### Limits, with either peripheral

- **One key in hardware per device.** Secure Boot normally stops any further
  read protection of eFuse blocks on its first boot, because a read-protected
  key digest would brick the device. A build with the device key in hardware
  leaves it open (`CONFIG_SECURE_BOOT_V2_ALLOW_EFUSE_RD_DIS`) until the key
  is burned, then closes it. A later key (a device that enrolls again after
  its certificate expired) is therefore made in software and kept in the
  encrypted store (`deviceKey: "storage"`), as when no block is free or
  anything above fails. The end-to-end test does not go through this case.
- **The key is in RAM once**, on the first boot, between its generation and
  the burn.
- **A key already in storage stays there**: devices keep their name.
- **The ESP32** has neither peripheral: its key stays in the encrypted store.
- **Emulator only**, like the rest: no real chip has been burned.

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

A spare key that was exposed is revoked through the fleet's config, with a
file the device reads as a command:

```yaml
config:
  - name: security
    inline:
      - path: /etc/flightctl-mcu/secure-boot.json
        content: '{"revokeKeys": [2]}'
```

Each device revokes those slots for good once its running firmware is
confirmed. It refuses the file (the update is reported as failed) when a key
cannot go: the one that signs the running firmware or the bootloader, or any
key on a device without Secure Boot. Keeping the file in the fleet is
harmless: revoking a key already revoked succeeds.

## Keys in an HSM

The build signs with a key file (`SECURE_BOOT_KEY`). To keep the Secure Boot
keys in an HSM instead, build without a key and sign afterwards:

```bash
TARGET=esp32c3 VARIANT=secure-release SECURE_BOOT_SIGNER=external firmware/build.sh
```

```bash
firmware/sign-external.py firmware/build-esp32c3-secure-release --pub-key key0.pub.pem --sign-cmd '<command> {digest} {signature}'
```

The command gets a file with the image's SHA-256 digest and must write an
RSA-PSS signature of it (SHA-256, salt length 32): the HSM's PKCS#11 tool, or
OpenSSL through its PKCS#11 engine or provider. `sign-external.py` checks the
signature against the public key and appends the signature block, to the
bootloader and to the app. For the factory bootloader, run it again with
`--bootloader-only` for each spare key. Tried in the emulator with OpenSSL
and key files standing in for the HSM: bootloader signed with two keys, app
with one; it booted and trusted both keys.

## ESP32 download mode

The ESP32 (classic) has no secure download mode, so the secure variant leaves
its ROM serial loader fully open (`CONFIG_SECURE_INSECURE_ALLOW_DL_MODE`): with
the device in hand, it can load code into RAM. For the field, the `nodl`
variant turns the ROM download mode off for good on first boot:

```bash
TARGET=esp32 VARIANT=secure-nodl-release SECURE_BOOT_KEY=secure_boot_key.pem firmware/build.sh
```

After that the serial port can no longer write the flash either: `fc_creds`
cannot be written again over USB (provision before the first boot, or in the
field for the WiFi), and a device that does not boot cannot be recovered over
it. On the other chips `nodl` replaces secure download mode the same way. It
builds; the emulator does not run the ESP32.

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
   and enrolls. Until that first boot, `fc_creds` is in clear on the flash:
   provision in a trusted place.

5. **Check it in Flight Control.** The device reports what its chip enforces in
   `customInfo`: `secureBoot: "true"`, `flashEncryption: "release"`,
   `deviceKey` (`"ecdsa"` on the C5, `"storage"` elsewhere), `secureBootKeys`
   and `secureVersion`.

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
- when the test builds the images: the bootloader trusts three keys; the
  third, a spare, is revoked through the fleet's config, revoking the key in
  use is refused, and a release signed with the second key alone retires the
  first (key rotation). CI's prebuilt images come without their keys.

On the C5 the test also checks the device key: burned into eFuse, read
protected, the peripheral's signature verified (`deviceKey: "ecdsa"`); every
request after that, enrollment and certificate renewals included, is signed
by the peripheral. On the C3 the key stays in the encrypted store
(`deviceKey: "storage"`).

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

- **Device key in hardware on chips without an ECDSA peripheral.** Blocked by
  Flight Control (above).
- **Real boards.** Nothing has been burned, here or in CI: everything above
  was verified on emulated chips. Before a batch, burn one board per chip,
  then run a full OTA cycle on it.
- **ESP32 (classic).** Secure Boot v2 needs silicon revision 3
  (`CONFIG_ESP32_REV_MIN_3`, set by the variant). Not run anywhere yet: the
  emulator does not run the ESP32.
- **The image signing key in an HSM.** `deploy/signing` signs OCI artifacts
  with a key file; cosign itself can sign with a KMS or PKCS#11 key.
