# Firmware updates as deltas

A firmware image is 1.1 to 1.6 MB, and most of it does not change from one
release to the next. A release can therefore carry deltas: for a device that
runs a known earlier release, the difference between the two images instead
of the whole new one.

Measured on release builds for the ESP32-C3 (an image of 1.11 MB):

| Update | Delta | Of the image |
|---|---|---|
| A large release (field provisioning added, 103 KB of new code) | 136 KB | 12.2% |
| A small release (bug fixes) | 32 KB | 2.9% |
| The same source, another version string | 11 KB | 1.0% |

## Publishing a release with deltas

```bash
tools/fw-push.py --delta-from v3.0 --delta-from v3.1 <registry>/flightctl-mcu/fw:v3.2
```

For each chip, `fw-push.py` takes that chip's image out of each release named
(a tag or a digest in the same repository), makes the delta to the new image
and pushes it next to it. A delta no smaller than the image is left out.
Nothing else changes: the fleet's `spec.os.image` names the new release as
before, and each device finds out by itself whether a delta is for it.

The deltas are made with [detools](https://github.com/eerimoq/detools), the
tool Espressif's `esp_delta_ota` uses: bsdiff's algorithm, in a patch that is
applied in order (`sequential`), compressed with heatshrink. `fw-push.py`
uses the `detools` Python package if it is installed, or installs it in a
virtual environment in `build/detools-venv` the first time (it has C
extensions: a compiler and the Python headers are needed).

## In the registry

A delta is one more layer in the chip's manifest, after the image:

```
index (tag)
  manifest       platform {architecture: esp32c3}
    layer        application/vnd.espressif.app-image                        the image
    layer        application/vnd.flightctl-mcu.firmware.delta.v1+detools    a delta
                   io.flightctl-mcu.delta.from:      sha256 of the image it starts from
                   io.flightctl-mcu.delta.from-size: that image's size
```

The image stays the first layer, which is what firmware older than this takes.
The index is signed as before, and the signature covers the deltas as it
covers the image: manifests and layers are fetched by digest.

## On the device

1. The agent resolves the release and checks its signature, as for any update.
2. For each delta in its chip's manifest, it hashes the first `from-size`
   bytes of the image it is running, read from its own slot, and compares
   with `from`. No match: it downloads the image, as before.
3. With a match, it downloads the delta and rebuilds the new image into the
   inactive slot as the delta arrives, reading the old image from the running
   slot. Nothing is kept in RAM but the decoder: about 7 KB, a decompression
   window of 4 KB, 2 KB of the old image read ahead, and its state.
4. The rebuilt image must have the sha256 of the first layer, which the signed
   manifest names. Then the slot is committed, with the usual checks (Secure
   Boot signature, security variant, anti-rollback), and the device reboots
   into it, confirms it or rolls back, as with a whole image.

If anything goes wrong on the way (the delta does not decode, the result has
another digest, memory is short), the slot is given up and the image is
downloaded whole, in the same update. An image the device refuses at commit
would be refused whole too, and is not downloaded again.

The device reports how its last firmware came in `customInfo`:
`firmwareDownload: "delta 136 of 1085 KB"`, or `"full 1085 KB"`.

With Secure Boot and flash encryption nothing changes: the device reads its
image in clear through the flash encryption, the delta is between the signed
images, and what it rebuilds is the signed image, byte for byte.

## A delta is not trusted until the end

The device decodes a delta before it can check its digest, which comes with
the last byte, over a registry that is plain HTTP. What stands between a
hostile delta and the device:

- the decoder reads only inside the running image and writes at most the new
  image's size into a slot that is not committed (`core/src/fc_delta.c`);
- it gets one allocation of at most 5 KB: a delta asking for a larger window
  is refused;
- the rebuilt image is committed only if its sha256 is the one in the signed
  manifest.

detools' C library computes with sizes read from the patch in plain `int`s,
and the fuzzer found a signed overflow in it within seconds (reported upstream:
[eerimoq/detools#22](https://github.com/eerimoq/detools/issues/22)). It is
built with wrapping arithmetic (`-fwrapv`) for that reason, and `fc_delta.c`
refuses any seek outside the old image before detools counts it. `tests/fuzz/fuzz_delta.c`
fuzzes the decoder in CI with the others.

## Other ports

The decoder is in the core. A port only has to read its running image:
`fc_port_fw_read(off, buf, len)`. One that cannot returns -1, and its updates
are whole images. On the host port, the image is the file `FC_FW_IMAGE`
names; on Zephyr, MCUboot's first slot, and the image is the one signed for
MCUboot (`fw-push.py ... PLATFORM=zephyr.signed.bin`).

## What the tests check

- Unit tests (`tests/unit/test_oci.c`): a delta is used when it starts from
  the running image and not otherwise; a delta that rebuilds another image,
  one corrupted at each byte in turn, one cut short, one with a larger window
  and one that does not match its digest all end with the whole image in the
  slot.
- End-to-end, on every emulated chip and variant (`tests/e2e/e2e-emu.sh`): the
  release is pushed with a delta from the debug build the device starts on,
  and the device must report that it came as a delta; the next image, which
  has none, must come whole.

## Limits

- **Deltas are per pair of images.** A device on a release with no delta to
  the new one downloads the image. Which releases to make deltas from is the
  publisher's choice.
- **A device flashed over USB** takes a delta only if what was flashed is,
  byte for byte, an image that was pushed (it is, when both come from the same
  build).
- **Tried on two boards.** A Raspberry Pi Pico W with Zephyr and MCUboot
  ([ZEPHYR.md](ZEPHYR.md)): a delta of 8.6 KB rebuilt a 734 KB image from the
  running one in 10 s, download included; and a delta took the board from
  its Zephyr 4.3 build to the 4.5 one (232 KB, a third of the image). An
  ESP32-S3 with both ports: the ESP-IDF firmware went from its debug build to
  the release one with a delta of 511 KB (half of the image: the two are
  compiled differently) in 13 s, the Zephyr one from a build to the next
  with 19 KB (2.2%) in 8 s. The other ESP32 chips: emulator only.
