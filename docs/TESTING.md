# Tests and CI

GitHub Actions runs two workflows. On every push that touches code, [ci.yml](../.github/workflows/ci.yml) builds everything and runs the unit tests and a quick fuzzing pass, in about 5 minutes. The end-to-end tests are [e2e.yml](../.github/workflows/e2e.yml), on what a CI run built: the host agent, the firmware images and the WASM apps, kept as the run's artifacts for 90 days. It runs every night on the last successful CI run of main, unless that commit has passed already; by hand from the Actions tab (with a CI run's id, or the last one); and from a CI run, by re-running its `e2e` job, which the first attempt skips. Only the broken firmware of the rollback test is built there.

- **Builds**: the simulated device on Linux, the firmware for every chip in the default and release profiles (with `ccache`), and the `secure` variant for the chips the end-to-end tests emulate, the example WASM apps, and the Zephyr builds. The firmware has no credentials in it. The secure images are not signed: whoever flashes them signs them with their own Secure Boot key ([SECURITY.md](SECURITY.md#signing-a-ci-build)), and the end-to-end test signs them with throwaway keys.
- **Unit tests** ([tests/unit.sh](../tests/unit.sh), under a second): the C core on an in-memory port, with AddressSanitizer and UBSan (HTTP framing, OCI references, index and signatures, registry credentials and token services' answers, device identity against an independently computed name, rendered specs, certificate renewal answers, re-enrollment, WASM apps run by WAMR: install, update, a module that cannot be stored or loaded or was built for the earlier interface, after a reboot, an app in an endless loop; the app interface against a fake board (GPIO, I2C, SPI, UART, ADC, PWM), the board map with pins or devicetree names, and the example apps; the remote console against a fake stream; gRPC streams on the HTTP/2 client; resource monitors and their alert rules), and the tools' logic in Python (cosign payloads and signatures, the provisioning partition, the allow list, the fleet export for Git).
- **Fuzzing** ([tests/fuzz.sh](../tests/fuzz.sh)): the HTTP response reader, the OCI manifest and signature handling, the agent's handling of rendered specs and certificates, and the remote console's handling of the server's bytes. 30 seconds per target on every push, 20 minutes per target every night ([fuzz-nightly.yml](../.github/workflows/fuzz-nightly.yml)), with the corpus kept from one night to the next. Crashes found so far are kept as seed inputs in `tests/fuzz/corpus`.
- **Hardware for apps on the boards**: [hwcheck](../wasm-apps/hwcheck/hwcheck.c), granted `i2c0 spi0 uart0 adc0 pwm0` with a board map per board, opens each and says what it answers. With nothing wired it scans the I2C bus, transfers on SPI, writes on the UART and reads a floating ADC pin; with a sensor or a jumper it shows what is there. Run by hand on the lab boards, not in CI.
- **End-to-end test** ([tests/e2e/e2e.sh](../tests/e2e/e2e.sh), in e2e.yml): the job deploys Flight Control 1.4.0 on kind and a registry on the runner, and the device pulls through its door with TLS and a password ([REGISTRY.md](REGISTRY.md)). The simulated device then goes through:
  - enrollment, approved by the allow-list script;
  - Online status;
  - the registry's CA without the password, which it must report as missing credentials;
  - inline config;
  - a signed WASM app install, and `cosign verify` on its signature;
  - an in-place update from v1 to v2;
  - a config change;
  - an unsigned app, which it must refuse;
  - an update while the registry is down, which must complete once it is back (the TLS door stopped);
  - the app's removal, then the second example app;
  - metrics: with the fleet's `metrics.json`, the device asks for the Telemetry Gateway's certificate, and its own metrics and the app's reach the gateway's Prometheus exporter ([METRICS.md](METRICS.md));
  - the remote console: `flightctl console device/<name> -- status` answers, and an unknown command fails; the command the UI's Logs tab sends on `bash -s` gets the log and the end marker the tab waits for;
  - a memory alert from the fleet's `spec.resources`: `Warning` and the summary `Degraded`, then `Healthy` again with the default monitors;
  - the renewal of its management certificate (the server issues 2-minute certificates for the test), and a status accepted after the first one expired;
  - a server restart, after which it must reconnect and take a new spec;
  - the server down until its certificate expired, after which it must enroll again, under a new name, with its config;
  - the fleet kept in Git: exported with `tools/fleet/git-export.py`, served by `git daemon` and imported by a `ResourceSync`, with the registry's CA and password mounted from Kubernetes secrets;
  - last, its decommissioning: Started, then Completed, the key and certificate wiped, and no new enrollment once it is restarted. The host agent gets `FactoryReset`, sent to the API (the CLI and the UI send `Unenroll` only), and its app has to be removed too; the Zephyr builds get `Unenroll` from the CLI.
- **Firmware in the emulator** ([tests/e2e/e2e-emu.sh](../tests/e2e/e2e-emu.sh), in e2e.yml): the real firmware for the C5, the S3 and the C6 runs in Espressif's [esp-emu](https://github.com/espressif/esp-emulator), whose WiFi access point NATs to the runner, against Flight Control on kind. It provisions the default image for the test cluster, then goes through enrollment, a signed update to the release image with a reboot into the other slot, a signed app and an unsigned one, all pulled through the registry's door with TLS and a password, certificate renewal, and the rollback of a broken image that cannot reach the server (built with a 30-second confirmation window instead of 150), and last its decommissioning. The same runs for the `secure` variant (`SECURE=1`): the first boot burns the emulated eFuses and encrypts the flash, the device reports Secure Boot and flash encryption on, and a firmware signed with another Secure Boot key is refused even though its cosign signature is valid. It also revokes a spare Secure Boot key and moves to a second one ([SECURITY.md](SECURITY.md#what-the-ci-checks)). Each device also refuses a valid firmware of the other variant: the plain one gets the secure build. Six runs in all: the C5, the S3 and the C6, plain and secure (the classic ESP32 is not emulated, and the C3 is built only); the plain runs give the device its WiFi network in the field ([PROVISIONING.md](PROVISIONING.md#what-the-ci-checks)). Certificate renewal, with 2-minute certificates, is in the plain C5 run only (`CERT=0` leaves it out): the renewal code is the same on every chip and the host and Zephyr runs cover it too, while short certificates make any slow step fail the run.
- **A crash in the emulator** ([tests/e2e/fatal-emu.sh](../tests/e2e/fatal-emu.sh), in e2e.yml after the plain S3 and C5 runs, against their cluster): a test build (`sdkconfig.faulttest`) faults 30 s after power-on; after the restart the status must give `resetReason: panic` and a `lastFatalError` naming the task, with a `pc` that `addr2line` decodes against the build's ELF to the function that faulted and its caller among the return addresses, and the previous boot's log must end with the same text. `MODE=abort` does the same with an `abort()`, whose `pc` is its caller's; `MODE=iwdt` with a loop with interrupts off, which the interrupt watchdog ends (`resetReason: int_wdt`); `MODE=twdt`, by hand only, with a loop that starves the idle task and the task watchdog set to panic (`resetReason: task_wdt`). The workflow runs the first three on the S3 (Xtensa) and the C5 (RISC-V), whose handlers differ; by hand it runs on the C3 too.
- **Zephyr** ([port/zephyr/build.sh](../port/zephyr/build.sh)): the Zephyr application built for `native_sim`, then, in e2e.yml, the end-to-end test of the simulated device with that build as the device (`DEVICE_KIND=zephyr tests/e2e/e2e.sh`), the waits for a certificate to expire included: the device working after its first certificate expired, and enrolling again after the server was down, with the Zephyr build's reboot under its new identity. The same test runs with the Zephyr build for `mps2/an385`, an Arm Cortex-M3 on Ethernet, in QEMU (`DEVICE_KIND=qemu`): built with the Arm toolchain of the Zephyr SDK, with the memory of a 384 KB device. The build for the ESP32-S3, with MCUboot and the Xtensa toolchain, is in CI too, built only: no emulator runs it. The port's hardware for apps (GPIO, ADC, PWM, I2C, SPI, UART from the devicetree) runs on `native_sim`'s emulated drivers: `tests/zephyr-hw.sh`, a ztest suite.

You can run both tests against a cluster you already have:

```bash
SKIP_DEPLOY=1 tests/e2e/e2e.sh
```

```bash
SKIP_DEPLOY=1 CHIP=esp32c5 tests/e2e/e2e-emu.sh
```

The emulator test needs `esp-emu` on the PATH or in `~/.local/bin`. In CI it does not rebuild the firmware (`PREBUILT=1`): it provisions the images of the CI run, after signing the secure ones with throwaway keys, and only the broken build is made by the workflow. To try a build in the emulator by hand, provision its merged image for the emulator's access point and run it:

```bash
tools/provision.py --image port/espidf/build-esp32c3/merged-binary.bin --wifi-ssid myssid --wifi-password mypassword
```

```bash
esp-emu --chip esp32c3 --firmware port/espidf/build-esp32c3/merged-binary.bin --net user
```

What is checked for a single feature is described with it: [firmware deltas](DELTA.md#what-the-tests-check), [provisioning](PROVISIONING.md#what-the-ci-checks), [the secure variant](SECURITY.md#what-the-ci-checks), [the Zephyr port](ZEPHYR.md#what-the-tests-check).

## Registry tokens

[tests/registry-bearer.sh](../tests/registry-bearer.sh), by hand, not in CI: the device's registry client (`fc_oci.c` and `fc_http.c`, built for the host as `fc-oci-pull`, without the agent) against registries that ask for Bearer tokens, in Docker. [docker_auth](https://github.com/cesanta/docker_auth) is the token service, with an account that pushes, a pull-only account for the device and anonymous pulls of `public/*`; two `registry:3` take its tokens: one with the realm on the token service's host, one behind nginx with the realm on its own host and port, as Harbor has it. With `QUAY=1`, Quay too (3.17, with Postgres and Redis, on port 9446, `QUAY_PORT`): an organization for each repository and, for the device, a robot account that reads `flightctl-mcu`; it takes about 1 GB of memory and a minute or two to start. Everything on a CA made for the run, on ports 9443–9445 (`REG_PORT`, `AUTH_PORT`, `PROXY_PORT`); the containers are removed at the end.

The release is pushed as the tools push it (`tools/fw-push.py`, `oras`, `tools/signing/sign.py`, through the same tokens), then pulled as a device, with the CA and `auth.json` where the fleet puts them, against each registry:

- a firmware from a two-chip index, its signature checked, the image byte for byte, with one token for the whole pull;
- a WASM layer, signed;
- a public repository with no credentials (an anonymous token), a private one with none, and one the account may not read: refused with `HTTP 401` and the reason;
- a wrong password (and a wrong robot token on Quay), a token service whose CA the device lacks;
- a token that runs out between the manifests and the blob: a second token, and the image intact. The tokens live 5 s and `registry:3` allows 60 s of clock skew, so this step waits 70 s (`QUICK=1` leaves it out).

No output may hold the password. Needs docker, oras, openssl, htpasswd (bcrypt), python3, cmake, ninja and mbedTLS 4.

```bash
tests/registry-bearer.sh
```

```bash
QUAY=1 tests/registry-bearer.sh
```

`registry:3` finds the key that signed a token by the token's `kid`, and docker_auth writes it in the form of libtrust, which `registry:3` no longer derives from a certificate: the script gives it the key as a JWKS with that `kid`.
