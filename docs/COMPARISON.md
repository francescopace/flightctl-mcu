# Comparison with other device management for microcontrollers

The table compares the MCU integrations documented in November 2026. ✓ means supported, ◐ means partial or limited to the integration described, ✗ means unavailable in that integration, and – means the cited sources leave the capability open. Open source covers the device and server. “Apps without reflashing” means replacing an application independently of the MCU firmware.

| Platform | Self-hosted | Open source | ESP-IDF | Zephyr | Linux in the same fleets | Firmware deltas | Remote config | Crash reports | Apps without reflashing |
|---|---|---|---|---|---|---|---|---|---|
| **flightctl-mcu** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| **AkiraOS with AkiraHub** | ◐ | ◐ | ✗ | ✓ | – | ✗ (v1.6.7) | – | ◐ | ✓ |
| **Atym with Ocre** | ✓ | ◐ | – | ✓ | ✓ | – | ✓ | – | ✓ |
| **Golioth** | ✗ | ◐ | ✓ | ✓ | ◐ | – | ✓ | – | ◐ (Zephyr demo) |
| **Memfault** | ✗ | ◐ | ✓ | ✓ | ✓ | ✓ | – | ✓ | – |
| **Mender MCU** | ✗ | ◐ | – | ✓ | ✓ | ✗ | – | – | – |
| **AWS IoT Device Management** | ✗ | ◐ | ✓ | ◐ | ✓ | – | ✓ | – | – |
| **Azure Device Update** | ✗ | ◐ | ◐ | – | ✓ | ◐ (SWUpdate reference client) | ✓ | – | – |
| **Eclipse hawkBit** | ✓ | ✓ | – | ✓ | ✓ | – | – | – | – |
| **Eclipse Leshan** | ✓ | ✓ | – | – | – | – | ✓ (LwM2M resource writes) | – | – |
| **Coiote with Anjay** | ✓ | ◐ | ✓ | ✓ | ◐ | – | ✓ | – | – |
| **ESP RainMaker Neo** | ✓ | ✓ | ✓ | – | ◐ | – | ✓ | ◐ | – |
| **Toit with Artemis** | ✓ | ✓ | ✓ | – | ◐ | – | ✓ | – | ✓ |

For a team already using Flight Control, the same rollout tools can manage Linux gateways and MCU sensors. On both ESP-IDF and Zephyr, the agent can replace a signed WASM app while the firmware keeps running and can apply a firmware delta when the release includes one for the installed image. [App updates](APPS.md#how-an-app-runs) · [Firmware deltas](DELTA-UPDATES.md#on-the-device)

## Sources

- **flightctl-mcu** — [Control plane and fleets](../README.md#what-maps-to-what) · [Supported hardware](DEVICES.md#hardware-compatibility) · [Measured deltas](DELTA-UPDATES.md#what-was-verified)
- **AkiraOS with AkiraHub** — [Hub and hosting](https://akiraos.dev/) · [OS release notes](https://docs.akiraos.dev/resources/release-notes) · [v1.6.7 delta decoder](https://github.com/ArturR0k3r/AkiraOS/blob/v1.6.7/src/lib/akira_delta.c)
- **Atym with Ocre** — [Ocre runtime and Hub hosting](https://www.atym.io/post/webassembly-for-containerization-of-embedded-software-comparing-diy-open-source-and-enterprise-im) · [WASM containers on Zephyr and Linux](https://docs.prod.atym.io/overview/containers) · [Fleet configuration and campaigns](https://docs.prod.atym.io/reference/cli/commands/campaign)
- **Golioth** — [Firmware SDK and platforms](https://docs.golioth.io/reference/firmware/golioth-firmware-sdk/) · [WASM deployment demo](https://blog.golioth.io/webassembly-on-zephyr/)
- **Memfault** — [ESP-IDF integration](https://docs.memfault.com/docs/mcu/espressif-esp32-guide) · [OTA and delta releases](https://docs.memfault.com/docs/platform/ota) · [Linux SDK](https://docs.memfault.com/docs/linux/introduction)
- **Mender MCU** — [Device tiers](https://docs.mender.io/overview/device-tiers) · [MCU client limitations](https://docs.mender.io/troubleshoot/mender-mcu)
- **AWS IoT Device Management** — [Fleet management and jobs](https://aws.amazon.com/iot-device-management/features/) · [Device shadows](https://docs.aws.amazon.com/iot/latest/developerguide/iot-device-shadows.html)
- **Azure Device Update** — [Device Update](https://learn.microsoft.com/en-us/azure/iot-hub-device-update/understand-device-update) · [Supported delta format](https://learn.microsoft.com/en-us/azure/iot-hub-device-update/delta-updates) · [IoT Hub configuration](https://learn.microsoft.com/en-us/azure/iot-hub/iot-hub-automatic-device-management)
- **Eclipse hawkBit** — [Rollout server](https://hawkbit.eclipse.dev/) · [Zephyr client](https://docs.zephyrproject.org/latest/samples/subsys/mgmt/hawkbit/README.html)
- **Eclipse Leshan** — [Java server and client libraries](https://eclipse.dev/leshan/) · [Server demo and source](https://github.com/eclipse-leshan/leshan)
- **Coiote with Anjay** — [Server features and Anjay platforms](https://avsystem.com/products-iot/) · [On-premises deployment](https://avsystem.com/hubfs/AVSystem_UC_Anjay_Coiote_IOT0224.pdf?hsLang=en)
- **ESP RainMaker Neo** — [Server and device SDK](https://developer.espressif.com/blog/2026/08/open-sourcing-esp-rainmaker-neo/) · [POSIX support](https://docs.neo.rainmaker.espressif.com/docs/firmware/get-started/setup-posix)
- **Toit with Artemis** — [Fleet and application updates](https://docs.toit.io/getstarted/fleet/) · [Artemis server](https://github.com/toitware/artemis)
