# Quick start

Flight Control on your laptop, then a device in it: an Espressif board, or a simulated one with no hardware at all.

You need Docker, kind, helm, kubectl, the [flightctl CLI](https://github.com/flightctl/flightctl/releases), esptool, oras, Python 3 with PyYAML, and for WASM apps clang plus `wasm-ld` (`brew install lld`). The simulated device also needs CMake, Ninja and the same mbedTLS 4.1.1 as the firmware, built with pthread support. The ESP-IDF toolchain runs in the `espressif/idf:v6.1.1` container, so you don't install it.

The scripts were written on macOS (Apple Silicon). CI runs the host agent, the firmware build and the kind setup on Linux too.

The walk below is for an Espressif board with the ESP-IDF firmware, the most complete of the three ports. For a board with the Zephyr build, steps 3 and 4 are in [ZEPHYR.md](ZEPHYR.md#on-an-esp32-s3) and its example fleet is `tools/fleet/zephyr-lab.yaml`; with no board at all, see [Without a board](#without-a-board).

## 1. Control plane

Flight Control on [kind](https://kind.sigs.k8s.io/) (Kubernetes in Docker: a Kubernetes cluster whose nodes are containers on one machine), reachable from your LAN at `<your-ip>.nip.io`, and an OCI registry with two doors to the same images: TLS with a password at `registry.<your-ip>.nip.io:5443`, which devices pull through, and plain HTTP on port 5001, which the tools on your machine push through ([REGISTRY.md](REGISTRY.md)). If you already run Flight Control elsewhere, on Kubernetes or on a Linux host, skip the first script and give the device its agent endpoint with `tools/provision.py --server` (step 4).

```bash
tools/kind/up.sh
```

```bash
tools/registry/up.sh
```

```bash
flightctl login https://api.<your-ip>.nip.io:8443 --insecure-skip-tls-verify -t "$(kubectl create token flightctl-admin -n flightctl --duration=24h)"
```

The UI is at `http://ui.<your-ip>.nip.io:8080`; log in with the same token.

`tools/kind/up.sh` creates the [MCU lab catalog](../tools/kind/mcu-lab.yaml) only if `mcu-lab` is missing. Open **Software Catalog → MCU lab (test)** for the nine WASM apps, including CoreMark, and the ESP-IDF and Zephyr firmware entries. To reapply the manifest to an existing catalog or finish an interrupted restore, run `tools/kind/apply-catalog.sh`. Both scripts accept the host's LAN IP as an argument.

The manifest restores the catalog entries and their version tags. The OCI images live in the registry's separate Docker volume, `fc-registry`, which survives kind cluster deletion. If you also remove that volume, republish the images with the tags in the manifest before deploying them.

The app and lab firmware entries use the registry’s TLS endpoint at `registry.<your-ip>.nip.io:5443`. Devices need the registry configuration from step 6.

Flight Control 1.4 copies everything a device reports in `customInfo` into its labels. These devices report counters there, once an hour, so narrow that to what does not change ([why](FLIGHTCTL-LIMITATIONS.md#label-sync)):

```bash
tools/fleet/apply.sh tools/fleet/labelsync-custom-info.yaml
```

## 2. Credentials

The enrollment credentials go to `.secrets/`, which is git-ignored:

```bash
tools/enrollment.sh
```

Create the key that signs firmware and apps. The device gets its public half, and from then on installs only images signed with it. The push scripts sign with it automatically:

```bash
tools/signing/keygen.sh
```

## 3. Build and flash a board (ESP-IDF)

Build the image with `LOCAL=0` to exclude the checkout’s WiFi credentials and endpoint. Step 4 provisions each board separately. `flash.sh` detects the chip on the port:

```bash
third_party/fetch-wamr.sh
```

```bash
LOCAL=0 TARGET=esp32c3 FW_VERSION=v0.9.0 port/espidf/build.sh
```

```bash
port/espidf/flash.sh /dev/cu.usbmodem1101
```

That build is optimized for size (`-Os`), with WAMR, which runs the apps, at `-O2` and the CPU at the chip's highest clock. `VARIANT=release` builds the same with quieter logs, in `port/espidf/build-<chip>-release`; select that profile with `--variant release` before the registry reference in step 6.

Nothing above touches the chip's eFuses. The `secure` variant does: Secure Boot v2, flash encryption and NVS encryption, for devices in the field. It is never built by default, and its first boot burns the chip's eFuses for good: read [SECURITY.md](SECURITY.md) first.

## 4. Provision it

`provision.py` writes the board's `fc_creds` partition: the CA, the enrollment credentials, the signing key, the agent endpoint (by default `agent-api.<your-ip>.nip.io:8443`) and the labels. Pass the WiFi as well:

```bash
tools/provision.py --port /dev/cu.usbmodem1101 --wifi-ssid <ssid> --wifi-password <password>
```

When the site's network is not known yet, pass `--pop <secret>` instead of the WiFi: the device then opens an access point where an installer gives it the network, with Espressif's provisioning app or `esp_prov` (see [PROVISIONING.md](PROVISIONING.md)).

`--image` writes the same partition into a merged flash image instead (the emulator uses it), `--out` into a file. During development you can also build the WiFi and the endpoint into the firmware, from `port/espidf/sdkconfig.wifi` and `port/espidf/sdkconfig.local` (see the `.example` files); a provisioned value wins. `LOCAL=0 port/espidf/build.sh` leaves those two files out, for images that leave your desk.

## 5. Approve it

A few seconds after boot the board asks to enroll:

```bash
flightctl get enrollmentrequests
```

```bash
flightctl approve -l stage=canary enrollmentrequest/<name>
```

For more than a few boards, list them in `tools/auto-approve/allowlist.yaml` (start from `allowlist.example.yaml`), by device name or MAC address with their labels, and let the script approve them as they show up:

```bash
tools/auto-approve/auto-approve.py tools/auto-approve/allowlist.yaml --watch 10
```

## 6. Give it firmware, config and an app

In an empty registry, these commands publish the C3 image built in step 3 and the alarm app using the tags in `tools/fleet/idf-lab.yaml`. In the populated MCU lab, the signed firmware index covers all five ESP-IDF platforms and the Zephyr S3. The signed alarm image is also available. Skip both publish commands there. For a new release, build each target in the fleet, pass all those targets to one `fw-push.py` invocation under a fresh tag, then update the fleet reference.

`apply.sh` fills in the registry address from `.secrets/registry/host` and adds the registry CA and password to the fleet as config files. Set `REGISTRY=host:port` to use another registry.

```bash
tools/fw-push.py localhost:5001/flightctl-mcu/fw:v0.9.0 esp32c3
```

It also pushes each image's ELF, which devices never pull: after a crash, `tools/fw-elf.py <registry>/flightctl-mcu/fw:<tag> <chip>` gets it back to decode the device's `lastFatalError` with `addr2line`.

```bash
wasm-apps/build.sh alarm v0.9.0
```

```bash
tools/fleet/apply.sh
```

The fleet selects devices labelled `app=alarm` (the firmware requests that label at enrollment). It rolls out to the `stage=canary` device first, then to the rest.

![The example fleet in Flight Control's UI](img/fleet.png)
*The fleet once applied: the firmware release as its system image, the four config sources, and every device up to date.*

The pushes go through the plain door, the devices pull the same images through the one with TLS: they speak nothing else. A fleet that still names the plain door has to move first; see [REGISTRY.md](REGISTRY.md#moving-a-fleet-to-the-tls-door). To keep the fleet in Git instead, see [REGISTRY.md](REGISTRY.md#fleets-kept-in-git).

## 7. Take it out

```bash
flightctl decommission device/<name>
```

The device reports that it started, then that it completed. It then wipes its key and management certificate and restarts. Flight Control shows it as `Decommissioned`, and it can be deleted:

```bash
flightctl delete device/<name>
```

The device keeps running its apps and config, but it no longer talks to Flight Control. It also does not enroll again by itself, even after a restart. To bring it back as a new device, erase its storage and flash it again:

- ESP-IDF: `esptool -p <port> erase-flash`, then steps 3 and 4 again.
- Zephyr on an ESP32: `ERASE=1 port/zephyr/flash-esp32.sh <port> .secrets` ([ZEPHYR.md](ZEPHYR.md#on-an-esp32-s3)).

## Without a board

Steps 1 and 2, then fetch WAMR, build the simulated device and run it on your machine. It enrolls like a board (step 5) and takes config and apps; a firmware update only saves the image to a file.

```bash
third_party/fetch-wamr.sh
```

```bash
third_party/build-mbedtls-host.sh  # once, to install the pinned library in build/mbedtls-install
cmake -S . -B build/host -G Ninja && cmake --build build/host
```

```bash
build/host/fc-host-agent --server agent-api.<your-ip>.nip.io:8443 --ca .secrets/ca.crt --enroll-cert .secrets/enroll.crt --enroll-key .secrets/enroll.key --signing-key .secrets/signing.pub --label site=lab
```
