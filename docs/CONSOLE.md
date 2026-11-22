# Remote console

Open the UI terminal or run `flightctl console device/<name>`. The MCU accepts these commands.

| Command | What it gives |
|---|---|
| `help` | The commands |
| `status` | The device's name, system, memory, storage, and its `customInfo` (heap, firmware, active boot slot where available, network) |
| `apps` | The WASM apps, their status and the state each reports |
| `coredump info` | Size in bytes of the saved crash dump, or zero if none is available |
| `coredump read` | The complete native dump on binary stdout in a non-interactive CLI session |
| `log` | The last 50 lines of [the device's log](#the-log) |
| `log -n N` | The last N lines |
| `log -p err` | Lines at that level and above. Also `warning` and `info`. |
| `log -f` | Follows the log until Ctrl-C |
| `reboot` | Restarts the device, once the session has ended |
| `exit` | Ends the session. So does Ctrl-D on an empty line. |

Run one command directly, or pass `--tty` for a prompt. An unknown command exits with status 127. In the prompt, Backspace edits, Ctrl-C or Ctrl-U clears the line, and Ctrl-D on an empty line exits. For a script, use `-- /bin/bash -s` and send one supported command per line on stdin.

```bash
flightctl console device/<name> -- status
```

![Output of status on an ESP32-C6 in the UI terminal](img/console-status.png)

## Download a crash dump

After reboot, `status.systemInfo.customInfo.coredumpBytes` gives the size of a valid dump in flash. An operator with console access can download the dump while the device is online. It can contain sensitive task memory.

1. In a local terminal, set `device` to the device name and save the bytes in a new private file.

   ```bash
   umask 077
   flightctl console --notty "device/$device" -- coredump info
   flightctl console --notty "device/$device" -- coredump read > crash.bin
   wc -c < crash.bin
   ```

   Check that the byte count matches `coredump info`. If the connection drops, repeat the download. Reading leaves the dump in flash. A later crash can replace it, and decommissioning erases it.

2. Use the ELF for the platform and release that **crashed**, including when the device rolled back after the fault. Set `release` to that release's registry reference by digest and `platform` to its architecture in the index.

   ```bash
   tools/fw-elf.py "$release" "$platform" firmware.elf
   ```

3. Decode the dump. With the ESP-IDF tools active, run `esp-coredump --chip esp32c3 info_corefile -t raw -c crash.bin firmware.elf`, substituting the device's chip. With Zephyr, start the target GDB (`arm-zephyr-eabi-gdb firmware.elf` for Arm) and connect it to Zephyr's coredump GDB server.

   ```gdb
   target remote | python3 /path/to/zephyr/scripts/coredump/coredump_gdbserver.py --pipe firmware.elf crash.bin
   info registers pc
   bt
   ```

ESP-IDF captures registers and task snapshots. Zephyr on Arm captures registers and thread stacks. The Xtensa backend on the ESP32-S3 captures the faulting thread's registers and stack.

| Layout | Dump partition (KiB) | Measured dump (B) | Measured on |
|---|---:|---:|---|
| ESP-IDF factory layout | 64 | 11,200 to 14,080 | Emulated C3 with zero or two apps running |
| Zephyr ESP32-S3 DevKitC with 8 MiB of flash | 128 | 8,535 | Physical board running `alarm` |

The ESP-IDF factory layout reserves 576 KiB for the agent's data. An OTA update preserves the installed partition table. Devices with the older 640 KiB data partition still report `lastFatalError`. Saving a native dump requires provisioning with the new factory layout.

## The log

A RAM ring holds the log ([size](#limits)). It includes agent messages, app logs, ESP-IDF errors and warnings, and the Zephyr system log. ESP-IDF keeps agent info messages even in quiet release builds. Most other ESP-IDF info messages and bare printk output are serial-only.

After a warm restart, use boot `-1` to read the previous boot. After a crash its final `fatal:` line identifies the fault, task and program counter, also reported in `lastFatalError`. Power loss or invalid retained RAM clears the ring.

```
Nov 16 21:07:04 I fc: enrolled, starting management loop
Nov 16 21:07:05 W wifi:Password length matches WPA2 standards, authmode threshold changes from OPEN to WPA2
Nov 16 21:12:30 W fc: app alarm: 31.5 C over the threshold
```

Timestamps are UTC once the clock synchronizes. Earlier lines show uptime.

The UI's **Logs** tab runs a filtered `journalctl` request through the console. The device serves it from the ring.

| The tab's filter | What the device shows |
|---|---|
| Agent (`-u flightctl-agent.service`) | The agent's and the apps' lines. The apps run inside the agent. |
| System, without a unit | Every line, the system's too |
| System, unit named after an app (`alarm` or `alarm.service`) | That app's lines |
| System, unit `flightctl-agent` or a glob that takes it | The agent's and the apps' lines |
| System, another unit | No lines |
| The kernel's (`-k`, from a command) | The system's lines |
| Level (`-p`) | E is `err`, W `warning`, I `info`, D `debug`. The level chosen and above. |
| Current boot (`-b 0`) | The current boot's lines |
| Previous boot (`-b -1`) | The previous boot's lines |
| An older boot | No lines |
| No boot chosen | Both boots' lines |
| The last hour, day, week or a range of dates | The lines in that period |
| Live | The lines kept, then the new ones as they are written, until the tab stops |
| File path (under `/var/log`) | Not found: the device has no files to show |

![The Logs tab](img/console-logs.png)
*The Logs tab on the ESP32-S3 with Zephyr: the agent's lines of the current boot, here the app `alarm` changing state.*

The console recognizes the subset of shell syntax used by the Flight Control 1.4 Logs tab. Arbitrary shell commands are unsupported.

## How it works

When a session appears in `spec.consoles`, the device opens a gRPC stream (`flightctl.v1.RouterService/Stream`) on its existing HTTP/2 connection. The server forwards stdin, stdout and exit status using `v5.channel.k8s.io`.

A failed stream open is retryable. Once opened, the agent remembers the session before executing its command. The last 64 IDs survive reboot, so a listed command cannot replay. The device polls faster while a session is active, and slows down once only remembered sessions have been listed for a minute. [Limits](#limits) · [Stale console sessions](FLIGHTCTL-LIMITATIONS.md#14-a-console-session-whose-user-left-before-the-device-came-stays-listed)

## Limits

| Limit | Value | Unit | Notes |
|---|---:|---|---|
| Sessions at a time | 1 | session | A second receives `another console session is open on this device`. |
| Session without input | 10 | minutes | Then it closes, except while following logs. A firmware or app download also closes the session. |
| Unjoined session | 1 | minute | Then it closes. |
| Last session IDs remembered across reboots | 64 | IDs | |
| Poll interval with an active session | 5 | s | |
| Poll interval with only remembered sessions listed | 15 | s | |
| Unread input | 4 | KB | A larger paste ends the session. |
| Script line | 2 | KiB | |
| Interactive line | 160 | characters | |
| Log ring in RAM | 4 | KiB | Roughly 40–80 lines |
| Thread stack of an open session | 8 | KiB | [Measured heap](ESP-IDF.md#runtime-memory) |
| Log staging on Zephyr | 0.9 | KB | |

Console access includes reboot and crash dump retrieval.
