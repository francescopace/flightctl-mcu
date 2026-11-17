# Comparison with other device management for microcontrollers

flightctl-mcu next to the services and open-source projects that manage microcontrollers from more than one chip vendor. What is said of the others comes from their public documentation, read in November 2026.

✅ yes · 🟡 partly, or with limits · ❌ no · ➖ not documented: we did not find it there, which does not mean it cannot be built on top.

## At a glance

| | Self-hosted | Open source | ESP-IDF | Zephyr | Linux in the same fleets | Delta updates | Remote config | Crash reports | Apps without reflashing |
|---|---|---|---|---|---|---|---|---|---|
| **flightctl-mcu** | ✅ | ✅ | ✅ | 🟡 | ✅ | ✅ | ✅ | 🟡 | ✅ |
| **Golioth** | ❌ | 🟡 | ✅ | ✅ | 🟡 | ➖ | ✅ | ➖ | 🟡 |
| **Memfault** | ❌ | 🟡 | ✅ | ✅ | ✅ | ✅ | ➖ | ✅ | ❌ |
| **Mender MCU** | ❌ | 🟡 | ❌ | ✅ | ✅ | ❌ | ➖ | ➖ | ❌ |
| **AWS IoT Device Management** | ❌ | 🟡 | ✅ | 🟡 | ✅ | ➖ | ✅ | ➖ | ❌ |
| **Azure Device Update** | ❌ | 🟡 | 🟡 | ➖ | ✅ | ➖ | ✅ | ➖ | ❌ |
| **Eclipse hawkBit** | ✅ | ✅ | ➖ | ✅ | ✅ | ➖ | ❌ | ❌ | ❌ |
| **LwM2M (Leshan, Coiote)** | 🟡 | 🟡 | 🟡 | ✅ | ✅ | ➖ | ✅ | ➖ | ❌ |

"Open source" is ✅ only when both the device code and the server are. Golioth, Memfault, AWS and Azure publish their device SDKs, not their servers; Mender's server is open source, but its microcontroller support is on hosted Mender only.

## The others

- **Golioth**, hosted, part of Canonical since March 2026. It has the broadest firmware SDK (ESP-IDF, Zephyr, nRF Connect, ModusToolbox) and, beside OTA by cohorts, settings, RPC, logs, a data pipeline and Bluetooth devices through gateways. Its OTA ships in Canonical's Zephyr 26.04 LTS. The Linux SDK is experimental, and apps without reflashing exist as a [WebAssembly demo](https://blog.golioth.io/webassembly-on-zephyr/).
- **Memfault**, hosted, Nordic's since June 2025 and also sold as nRF Cloud. It is built around crashes: coredumps, metrics and traces, with OTA by cohorts and delta releases, on microcontrollers, Linux and Android.
- **Mender** has an open-source server for Linux devices. Its microcontroller client, `mender-mcu` (Apache 2.0), runs on Zephyr only, does OTA and inventory, and works with hosted Mender only, in a "Micro" tier: artifacts up to 5 MB, no end-to-end signatures.
- **AWS IoT Device Management** and **Azure Device Update** are parts of a cloud, to be put together with its other services. On AWS, OTA goes through IoT Jobs and the FreeRTOS OTA agent (on ESP32, Espressif's `esp-aws-iot`; on Zephyr, samples and nRF Connect's AWS FOTA library), configuration through device shadows, Linux through the Jobs agent and Greengrass. Azure has ESP32 samples (FreeRTOS, bare metal), an agent for Eclipse ThreadX, and device twins for configuration.
- **Eclipse hawkBit** is an open-source update server with rollouts, and does nothing else. Zephyr has a client built in; Linux devices use SWUpdate or RAUC.
- **LwM2M** is the OMA standard for device management, common on cellular devices, where firmware and configuration are objects of the protocol. The clients are Zephyr's own and AVSystem's Anjay, also on ESP-IDF; the servers, Eclipse Leshan (open source) and AVSystem Coiote (commercial).

## What only flightctl-mcu does

Of the eight, only flightctl-mcu:

- runs WebAssembly apps: logic changes on a running device without a firmware release, the same `.wasm` runs on every chip, and an app reaches only the hardware its fleet grants it ([APPS.md](APPS.md));
- takes firmware and apps from any OCI registry, signed with cosign as containers are, where the others keep them in their own storage and format;
- publishes an SBOM per release to Trustify, and Flight Control shows the CVEs on the devices that run it ([SBOM.md](SBOM.md));
- puts microcontrollers in Red Hat Edge Manager, with the same fleets, label selectors, rollouts and RBAC as bootc devices. Most of the others manage Linux too, each in its own way.

It is behind on crash reports, remote log and commands, and data from the apps (the 1.0 and 2.0 items below). WiFi is its only link on real boards, deep sleep waits on Flight Control, and support is the community's.

## Golioth, Memfault and Mender in detail

These three manage ESP-IDF or Zephyr devices as a product, and are the nearest to flightctl-mcu. What works today is verified in [RESULTS.md](RESULTS.md).

For flightctl-mcu, 🔜 is on the [roadmap](ROADMAP.md#next) for 1.0, ⏳ waits on a change in Flight Control ([limitations](RESULTS.md#flight-control-limitations-found-upstream-proposals)) and 💡 is proposed for 2.0 ([after 1.0](ROADMAP.md#after-10)). 

| | flightctl-mcu | Golioth | Memfault | Mender MCU |
|---|---|---|---|---|
| **Server you run yourself** | ✅ Flight Control on Kubernetes, OpenShift or Podman; Red Hat Edge Manager is its supported build | ❌ hosted | ❌ hosted (Memfault, nRF Cloud) | ❌ hosted only; on-premise planned |
| **ESP-IDF** | ✅ ESP32, S3, C3, C5, C6 | ✅ | ✅ | ❌ Zephyr only |
| **Zephyr** | 🟡 without the device protection of the ESP-IDF port · 🔜 adds it | ✅ | ✅ | ✅ ESP32-S3 as reference board |
| **Other chips** | 🟡 Arm Cortex-M3 in QEMU · 🔜 an ST Nucleo with Ethernet · 💡 Nordic nRF, RP2350, STM32U5 | ✅ Nordic, Espressif, Infineon, NXP and more | ✅ any Cortex-M or ESP32 | ✅ any Zephyr board with MCUboot |
| **Linux devices in the same fleets** | ✅ the Flight Control agent, bootc images and containers | 🟡 Linux SDK, experimental | ✅ Linux (Yocto) and Android SDKs | ✅ Mender's main client |
| **WebAssembly apps** | ✅ run by WAMR, installed and updated without a reboot or a firmware release; one `.wasm` for Xtensa, RISC-V and Arm | 🟡 a [demo](https://blog.golioth.io/webassembly-on-zephyr/) with WAMR, modules sent as settings; not a product feature | ❌ | ❌ |
| **Firmware and apps from an OCI registry** | ✅ any registry (TLS, password), signed with cosign, one index for every chip | ❌ its own storage | ❌ its own storage | ❌ its own artifacts |
| **Device identity** | ✅ key made on the device, mTLS certificate from the server, renewal | ✅ pre-shared key, or a certificate from your own PKI | 🟡 a project key shared by the devices, and their serial number | ✅ key pair, accepted on the server |
| **Firmware OTA with rollback** | ✅ A/B, confirmed only once the server accepts the new firmware's status | ✅ through MCUboot or ESP-IDF | ✅ | ✅ through MCUboot |
| **Signed firmware** | ✅ cosign signatures checked by the agent; on ESP-IDF also Secure Boot, verified on emulated chips | ✅ | ✅ through the bootloader | 🟡 MCUboot's; Mender's own signatures not on the Micro tier |
| **Delta updates** | ✅ | ➖ | ✅ | ❌ nor compression |
| **Staged rollouts** | ✅ Flight Control's batches with a failure threshold | ✅ cohorts | ✅ cohorts and percentages | 🟡 phased deployments, on paid plans |
| **Remote configuration** | ✅ files the firmware can reject | ✅ settings | ➖ | ➖ |
| **Health and alerts** | ✅ CPU, heap, storage against the fleet's rules | 🟡 device state and logs | ✅ metrics and alerts | 🟡 inventory |
| **Crash reports** | 🟡 after a restart, `resetReason` in the status; on Zephyr also `lastFatalError` (reason, thread, `pc`) · 🔜 `lastFatalError` on ESP-IDF · 💡 backtraces and coredumps off the device | ➖ | ✅ its core feature | ➖ |
| **Remote log** | ✅ the last lines, kept in RAM, in the UI's Logs tab and the console ([CONSOLE.md](CONSOLE.md#the-log)) | ✅ | ✅ with crashes | ➖ |
| **Remote commands** | 🔜 Flight Control's terminal, with a fixed set of commands | ✅ RPC | ➖ | ➖ |
| **Data from apps** | 🔜 MQTT to your broker, metrics as OTLP to Flight Control | ✅ LightDB Stream and pipelines | 🟡 metrics only | ➖ |
| **Peripherals from apps** | 🟡 GPIO, LEDs, timers on ESP-IDF · 🔜 I2C, SPI, UART, ADC, PWM, and GPIO on Zephyr | ➖ in the firmware | ➖ in the firmware | ➖ in the firmware |
| **SBOM and known vulnerabilities** | ✅ an SBOM per release, CVEs shown in Flight Control through Trustify | ➖ | ➖ | ➖ |
| **Links** | 🟡 WiFi; Ethernet in QEMU · 🔜 Ethernet on a board · 💡 cellular (LTE-M, NB-IoT) | ✅ WiFi, Ethernet, cellular | ✅ any, including BLE through a phone or gateway | ✅ what the Zephyr board has |
| **Devices without IP** | 💡 BLE or Thread devices behind a Linux gateway in the same fleet | ✅ Bluetooth through gateways (Pouch) | ✅ BLE through a phone or gateway | ➖ |
| **Battery and deep sleep** | ⏳ needs status without a long-poll | ✅ devices sync when they wake | ✅ devices upload when they wake | 🟡 polling, up to once a week |
| **Commercial support** | ❌ community | ✅ | ✅ | ✅ |

## Tied to one chip vendor

Not in the tables, since they run on one vendor's chips:

- **[ESP RainMaker](https://developer.espressif.com/blog/2026/08/open-sourcing-esp-rainmaker-neo/)** (Espressif) gives OTA, parameters, a phone app and a cloud on AWS. Since August 2026 the whole RainMaker Neo stack, cloud included, is open source (Apache 2.0) and runs in your own AWS account. Crash reports and metrics come from ESP Insights, in the enterprise plan.
- **[Toit](https://toit.io)** (ESP32) runs apps as containers that change without reflashing, as our WebAssembly apps do, but they are written in the Toit language. Its fleet manager, Artemis, pushes firmware, containers and configuration.

[AkiraOS](https://www.cnx-software.com/2026/06/06/docker-for-microcontrollers-akiraos-combines-zephyr-rtos-with-webassembly-wasm-applications/) also runs WebAssembly apps, on Zephyr; it is an OS, with no fleet management.

## Sources

- Golioth: [Firmware SDK](https://github.com/golioth/golioth-firmware-sdk), [OTA](https://docs.golioth.io/firmware/golioth-firmware-sdk/firmware-upgrade), [cohorts](https://blog.golioth.io/golioth-upgrades-ota), [device authentication](https://docs.golioth.io/reference/device-api/device-auth), [Bluetooth](https://blog.golioth.io/bluetooth-at-golioth-so-far-2025/), [part of Canonical](https://blog.golioth.io/?p=10529), [Zephyr LTS](https://canonical.com/blog/zephyr-lts-announcement)
- Memfault: [ESP-IDF integration](https://docs.memfault.com/docs/mcu/esp32-guide), [OTA, cohorts and delta releases](https://docs.memfault.com/docs/platform/ota), [Linux SDK](https://docs.memfault.com/docs/linux/introduction), [Nordic acquisition](https://embeddedcomputing.com/technology/iot/nordic-semiconductor-acquires-memfault-launches-chip-to-cloud-platform)
- Mender: [MCU client](https://docs.mender.io/operating-system-updates-zephyr/overview), [release and Micro tier](https://mender.io/blog/official-release-of-mender-for-microcontrollers-mcus-like-zephyr-and-the-micro-device-tier), [device tiers](https://docs.mender.io/overview/device-tiers), [device support](https://docs.mender.io/devices), [known limits](https://docs.mender.io/troubleshoot/mender-mcu), [phased rollout](https://mender.io/blog/managing-large-scale-deployments-with-phased-rollout-and-schedule-deployments)
- AWS: [Device Management features](https://aws.amazon.com/iot-device-management/features/), [FreeRTOS OTA](https://docs.aws.amazon.com/freertos/latest/userguide/ota-update-library.html), [OTA in the IoT Lens](https://docs.aws.amazon.com/wellarchitected/latest/iot-lens/change-management.html)
- Azure: [Device Update](https://learn.microsoft.com/azure/iot-hub-device-update/understand-device-update), [OTA for MCUs](https://techcommunity.microsoft.com/blog/iotblog/over-the-air-updates-for-mcus-with-azure-device-update/3592702)
- hawkBit: [features](https://eclipse.dev/hawkbit/features), [rollout management](https://eclipse.dev/hawkbit/concepts/rollout-management)
- LwM2M: [firmware updates with Zephyr](https://www.zephyrproject.org/remote-firmware-updates-for-iot-devices-using-lwm2m-and-zephyr-rtos/), [Coiote updates](https://iotdevzone.avsystem.com/docs/Coiote_IoT_DM/Single_update)
- ESP RainMaker: [RainMaker Neo open source](https://developer.espressif.com/blog/2026/08/open-sourcing-esp-rainmaker-neo/). Toit: [fleet management](https://docs.toit.io/getstarted/fleet)
