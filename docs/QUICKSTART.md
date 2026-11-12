# Quick start

Flight Control on your laptop, then a device in it: an Espressif board, or a simulated one with no hardware at all.

You need Docker, kind, helm, kubectl, the [flightctl CLI](https://github.com/flightctl/flightctl/releases), esptool, oras, Python 3 with PyYAML, and for WASM apps clang plus `wasm-ld` (`brew install lld`). The simulated device also needs CMake, Ninja and mbedTLS 4.1 or later on the machine (`brew install mbedtls`). The ESP-IDF toolchain runs in the `espressif/idf:v6.0.3` container, so you don't install it.

The scripts were written on macOS (Apple Silicon). CI runs the host agent, the firmware build and the kind setup on Linux too.

The walk below is for an Espressif board with the ESP-IDF firmware, the most complete of the three ports. For a board with the Zephyr build, steps 3 and 4 are in [ZEPHYR.md](ZEPHYR.md) and the fleet is `tools/fleet/zephyr-lab.yaml`; with no board at all, see [Without a board](#without-a-board).

## 1. Control plane

Flight Control on kind, reachable from your LAN at `<your-ip>.nip.io`, and a plain-HTTP OCI registry on port 5001. If you already run Flight Control elsewhere, on Kubernetes or on a Linux host, skip the first script and set its agent endpoint in `port/espidf/sdkconfig.local` (step 2).

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

Flight Control 1.4 copies everything a device reports in `customInfo` into its labels. These devices report counters there, so narrow that to what does not change ([why](RESULTS.md#flight-control-140)):

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

The firmware has no credentials in it, so one build serves every installation. `flash.sh` detects the chip on the port:

```bash
third_party/fetch-wamr.sh
```

```bash
third_party/fetch-detools.sh
```

```bash
TARGET=esp32c3 FW_VERSION=v1 port/espidf/build.sh
```

```bash
port/espidf/flash.sh /dev/cu.usbmodem1101
```

That is a debug build. `VARIANT=release` builds with `-Os` and quieter logs, about 15% smaller, in `port/espidf/build-<chip>-release`; push those with `fw-push.py --variant release`.

Nothing above touches the chip's eFuses. The `secure` variant does: Secure Boot v2, flash encryption and NVS encryption, for devices in the field. It is never built by default, only with `VARIANT=secure` and a Secure Boot key of your own, in its own directory (`port/espidf/build-<chip>-secure`). Its first boot burns the chip's eFuses for good; read [SECURITY.md](SECURITY.md) first, which also has the factory procedure. `flash.sh` will not flash it without being told, and a device takes over the air only firmware of its own variant: a plain device refuses a secure build, and the reverse.

## 4. Provision it

`provision.py` writes the board's `fc_creds` partition: the CA, the enrollment credentials, the signing key, the agent endpoint (by default `agent-api.<your-ip>.nip.io:8443`) and the labels. Pass the WiFi as well:

```bash
tools/provision.py --port /dev/cu.usbmodem1101 --wifi-ssid <ssid> --wifi-password <password>
```

When the site's network is not known yet, pass `--pop <secret>` instead of the WiFi: the device then opens an access point (or, built with `VARIANT=ble`, advertises over Bluetooth) where an installer gives it the network, with Espressif's provisioning app or `esp_prov` (see [PROVISIONING.md](PROVISIONING.md)).

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

Push a firmware index and the WASM app to the registry, then apply the fleet. `apply.sh` fills in the registry address, which defaults to your LAN IP on port 5001; set `REGISTRY=host:port` to use another one. Use the same tags as in `tools/fleet/idf-lab.yaml`, or edit them there. (The fleet of the boards with the Zephyr build is `tools/fleet/zephyr-lab.yaml`: the two ports are released apart for now.)

```bash
tools/fw-push.py <your-ip>:5001/flightctl-mcu/fw:v3.0
```

```bash
wasm-apps/build.sh alarm v2.2
```

```bash
tools/fleet/apply.sh
```

The fleet selects devices labelled `app=alarm` (the firmware requests that label at enrollment). It rolls out to the `stage=canary` device first, then to the rest.

## Without a board

Steps 1 and 2, then fetch WAMR and detools, build the simulated device and run it on your machine. It enrolls like a board (step 5) and takes config and apps; a firmware update only saves the image to a file.

```bash
third_party/fetch-wamr.sh
```

```bash
third_party/fetch-detools.sh
```

```bash
cmake -S . -B build/host -G Ninja && cmake --build build/host
```

```bash
build/host/fc-host-agent --server agent-api.<your-ip>.nip.io:8443 --ca .secrets/ca.crt --enroll-cert .secrets/enroll.crt --enroll-key .secrets/enroll.key --signing-key .secrets/signing.pub --label site=lab
```
