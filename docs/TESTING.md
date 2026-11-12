# Tests and CI

GitHub Actions runs on every push that touches code. It builds once and tests what it built: the end-to-end jobs take the host agent, the firmware images and the WASM apps from the build jobs.

- **Builds**: the simulated device on Linux, the firmware for every supported chip in debug and release profiles (with `ccache`), and the example WASM apps. The firmware has no credentials in it.
- **Unit tests** ([ci/unit.sh](../ci/unit.sh), under a second): the C core on an in-memory port, with AddressSanitizer and UBSan (HTTP framing, OCI references, index and signatures, device identity against an independently computed name, rendered specs, certificate renewal answers, re-enrollment), and the deploy scripts' logic in Python (cosign payloads and signatures, the provisioning partition, the allow list).
- **Fuzzing** ([ci/fuzz.sh](../ci/fuzz.sh)): the HTTP response reader, the OCI manifest and signature handling, and the agent's handling of rendered specs and certificates. 30 seconds per target on every push, 20 minutes per target every night ([fuzz-nightly.yml](../.github/workflows/fuzz-nightly.yml)), with the corpus kept from one night to the next. Crashes found so far are kept as seed inputs in `tests/fuzz/corpus`.
- **End-to-end test** ([ci/e2e.sh](../ci/e2e.sh)): the job deploys Flight Control 1.4.0 on kind and a registry on the runner. The simulated device then goes through:
  - enrollment, approved by the allow-list script;
  - Online status;
  - inline config;
  - a signed WASM app install, and `cosign verify` on its signature;
  - an in-place update from v1 to v2;
  - a config change;
  - an unsigned app, which it must refuse;
  - an update while the registry is down, which must complete once it is back;
  - the app's removal, then the second example app;
  - the renewal of its management certificate (the server issues 2-minute certificates for the test), and a status accepted after the first one expired;
  - a server restart, after which it must reconnect and take a new spec;
  - the server down until its certificate expired, after which it must enroll again, under a new name, with its config.
- **Firmware in the emulator** ([ci/e2e-emu.sh](../ci/e2e-emu.sh)): the real firmware for the C3 and the C5 runs in Espressif's [esp-emu](https://github.com/espressif/esp-emulator), whose WiFi access point NATs to the runner, against Flight Control on kind. It provisions the debug image for the test cluster, then goes through enrollment, a signed update to the release image with a reboot into the other slot, a signed app and an unsigned one, certificate renewal, and the rollback of a broken image that cannot reach the server (built with a 30-second confirmation window instead of 150). The same runs for the `secure` variant (`SECURE=1`): the first boot burns the emulated eFuses and encrypts the flash, the device reports Secure Boot and flash encryption on, and a firmware signed with another Secure Boot key is refused even though its cosign signature is valid. Each device also refuses a valid firmware of the other variant: the plain one gets the secure build. Five runs in all: the C3 and the C5, plain and secure, and the C3 with its WiFi network given over BLE instead of SoftAP ([PROVISIONING.md](PROVISIONING.md#what-the-ci-checks)).
- **Zephyr** ([zephyr/build.sh](../zephyr/build.sh)): the Zephyr application built for `native_sim`, then the end-to-end test of the simulated device with that build as the device (`DEVICE_KIND=zephyr ci/e2e.sh`). The builds for boards are not in CI: they need the ARM and Xtensa toolchains.

You can run both tests against a cluster you already have:

```bash
SKIP_DEPLOY=1 ci/e2e.sh
```

```bash
SKIP_DEPLOY=1 CHIP=esp32c5 ci/e2e-emu.sh
```

The emulator test needs `esp-emu` on the PATH or in `~/.local/bin`. In CI it does not rebuild the firmware: it provisions the images the firmware job built. To try a build in the emulator by hand, provision its merged image for the emulator's access point and run it:

```bash
deploy/provision.py --image firmware/build-esp32c3/merged-binary.bin --wifi-ssid myssid --wifi-password mypassword
```

```bash
esp-emu --chip esp32c3 --firmware firmware/build-esp32c3/merged-binary.bin --net user
```

What is checked for a single feature is described with it: [firmware deltas](DELTA.md#what-the-tests-check), [provisioning](PROVISIONING.md#what-the-ci-checks), [the secure variant](SECURITY.md#what-the-ci-checks), [the Zephyr port](ZEPHYR.md#what-the-tests-check).
