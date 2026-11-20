# Firmware updates as deltas

Publish a delta to reduce the download for devices on a known release. Keep the full image in the same signed index so every device has an update path. [Measured download sizes](RESULTS.md#firmware-deltas)

## Publishing a release with deltas

```bash
tools/fw-push.py --delta-from "$OLD_TAG" "$REGISTRY/flightctl-mcu/fw:$NEW_TAG"
```

Set `REGISTRY` to the registry host, `OLD_TAG` to the installed release tag and `NEW_TAG` to a fresh tag. Repeat `--delta-from` for each source tag or digest in the same repository. For each platform, the publisher generates a sequential, heatshrink-compressed patch and includes it only if smaller than the new image. The fleet still sets `spec.os.image` to the new release.

The publisher installs detools in `build/detools-venv` if needed. Its C extensions require a compiler and Python headers. The device uses the [core decoder](../core/src/fc_delta.c).

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

Older firmware reads the first layer, which holds the full image. The index signature covers both full images and deltas through their manifest and layer digests.

## On the device

1. With the [OCI public key provisioned](SECURITY.md#two-signing-keys), verify the release signature. For each candidate delta, hash the first `from-size` bytes of the running slot and compare the digest with `from`.
2. If a delta matches, stream it into the decoder. Read unchanged bytes from the running slot and write the rebuilt image to the inactive slot. Decoder state, a 4 KB window and 2 KB of read-ahead use about 7 KB of RAM.
3. Check the rebuilt image against the signed full-image SHA-256 before committing the slot. Normal signature, security variant, anti-rollback and boot confirmation checks still apply.

Read `status.os.lastDelta.outcome` after an update.

| Outcome | Meaning |
|---|---|
| `Applied` | Installed a matching delta. |
| `NotUsed` | No matching delta. The device downloaded the full image. |
| `Fallback` | Patch decoding, allocation or digest verification failed. The device retried with the full image. See `status.os.lastDelta.fallbackReason`. |

If the device rejects the rebuilt image at commit, downloading it whole would give the same result. The agent reports the error without another pull.

![status.os.lastDelta in a device's YAML](img/delta-applied.png)
Flash encryption allows the decoder to read the running image in clear. With Secure Boot, the patch reconstructs the signed binary byte for byte.

## A delta is not trusted until the end

The decoder processes bytes before the final digest is available. It bounds reads to the source image, writes to the expected output size and rejects decompression windows over 4 KB.

## Other ports

The decoder is in the core. A port only has to read its running image: `fc_port_fw_read(off, buf, len)`. One that cannot returns -1, and its updates are whole images. On the host port, the image is the file `FC_FW_IMAGE` names; on Zephyr, MCUboot's first slot, and the image is the one signed for MCUboot (`fw-push.py ... PLATFORM=zephyr.signed.bin`).

## What was verified

On the ESP-IDF S3, a 511 KB delta from a debug build to a release build took 13 s. On the Zephyr S3, a 19 KB delta between successive builds took 8 s and was 2.2% of the full image. [Delta sizes across all six boards](RESULTS.md#firmware-deltas)

Unit tests cover truncated, corrupted and mismatched deltas and full-image fallback. The emulator checks both delta and full updates. [Test suite](TESTING.md#end-to-end-tests)

## Limits

The running image must match the source of a published delta byte for byte. For a board flashed over USB, publish the same binary used for flashing.
