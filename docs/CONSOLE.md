# Remote console

Flight Control's console reaches the device as it reaches a Linux one: `flightctl console device/<name>`, or the UI's terminal. A microcontroller has no shell, so the device answers a closed set of commands instead:

| Command | What it gives |
|---|---|
| `help` | the commands |
| `status` | the device's name, system, memory, storage, and its `customInfo` (heap, firmware, network) |
| `apps` | the WASM apps, their status and the state each reports |
| `log` | the device's log: the last 50 lines, or `-n N`; `-p err` (or `warning`, `info`) and above; `-f` follows it until Ctrl-C |
| `reboot` | restarts the device, once the session has ended |
| `exit` | ends the session (so does Ctrl-D on an empty line) |

One command per session:

```bash
flightctl console device/<name> -- status
```

A session can also read its commands on stdin, as `bash -s` would (`flightctl console device/<name> -- /bin/bash -s`, or no command and no terminal): each line runs as it comes, and the session ends when stdin does.

Or a prompt, in a terminal (`--tty`, and the UI's terminal), with the line edited on the device: Backspace, Ctrl-C to drop the line, Ctrl-U; arrow keys are ignored. An unknown command answers so and, as a session's one command, exits with status 127.

## The log

The device keeps its last log lines in RAM: the agent's and the apps' (an app's are `app <name>: ...`), as the serial port shows them, in a ring of 4 KB reserved at build time (about 40 to 80 lines; the oldest go first). Debug lines are kept only in a build that prints them. Only this boot: the ring is not kept across a restart. The lines of ESP-IDF and Zephyr themselves (WiFi, TLS) are not in it, only on the serial port.

```
Nov 16 21:07:04 I fc: enrolled, starting management loop
Nov 16 21:12:30 W fc: app alarm: 31.5 C over the threshold
```

The time is UTC; before the device has set its clock, a line shows its uptime instead.

**The UI's Logs tab.** Flight Control has no API for a device's log: the UI's Logs tab opens a console session with `/bin/bash -s` and writes on its stdin what a Linux device would run, `journalctl` with the tab's filters piped to `tail -n`, then a line that prints an end marker with the exit status. The device answers those commands from its RAM log:

| The tab's filter | What the device shows |
|---|---|
| Agent (`-u flightctl-agent.service`) | every line (the apps run inside the agent) |
| System, with a unit | the lines of an app when the unit is the app's name (`alarm` or `alarm.service`); every line for `flightctl-agent` or a glob that takes it; none for another unit |
| Level (`-p`) | E is `err`, W `warning`, I `info`, D `debug`; the level chosen and above |
| Current boot (`-b 0`), the last hour, day, week, a range of dates | as asked; the previous boot is empty |
| Live | the lines kept, then the new ones as they are written, until the tab stops |
| File path (under `/var/log`) | not found: the device has no files to show |

For this, the console takes the little of a shell's that the tab sends: quotes, `;`, `&&`, `||`, a pipe to `tail -n`, `sudo`, `bash -c`, `printf`, `echo`, `[[ ]]` and `journalctl`'s options. It is not a shell: nothing else runs, and a command it does not know answers so (exit status 127). If the UI changes the commands it sends, the tab may stop working with these devices; the commands are the same in the UI of Flight Control 1.4.0 and on its main branch of 16 November 2026.

## How it works

Flight Control puts the session in the device's rendered spec (`spec.consoles`), and the device opens a gRPC stream for it, `flightctl.v1.RouterService/Stream`, on the agent's own HTTP/2 connection: the agent endpoint serves gRPC on the same port, so the console costs no TLS connection of its own. The server joins it to the user's side. The bytes on it are Kubernetes' remote command protocol (`v5.channel.k8s.io`): stdin comes in, stdout and the exit status go out.

While a session is listed, the server answers the spec's long-poll at once with the version the device has; the device then only handles the console, and waits 5 s before asking again. It remembers the last 16 sessions, across restarts, so a `reboot` is not asked again after the restart, and a session nobody joins within a minute ends (Flight Control 1.4 keeps listing a session whose user left before the device came: [limitation 15](RESULTS.md#flight-control-limitations-found-upstream-proposals)); while only such sessions are listed, the device asks for its spec every 15 s instead of 5.

## Limits

- **One session at a time.** A second gets "another console session is open on this device" and fails.
- **A session ends** after 10 minutes without input (unless it follows the log: `log -f`, the Logs tab's live mode), and when the device needs its connection for a download (a firmware or an app takes the connection's turn, [REGISTRY.md](REGISTRY.md#memory)).
- **Input**: up to 4 KB not yet read by the device; more at once (a large paste) ends the session. A line of a session reading stdin is up to 2 KB, one typed at the prompt 160 characters.
- **RAM**: the log's ring, 4 KB, always; while a session lasts, its thread's stack (8 KB, of which the Logs tab's commands used 5.2 KB at most on the ESP32, C5, C6 and S3), the session with its 2 KB input buffer, the stream's buffer, a 1 KB output buffer while a command writes, and 2 KB more for a session reading stdin; none otherwise.
- **Access** is Flight Control's: whoever may open a console on the device may also restart it.

Tried against Flight Control 1.4.0 on the classic ESP32, C3, C5, C6 and S3 (ESP-IDF), on the ESP32-S3 with Zephyr, and in the end-to-end test on the simulated device and the Zephyr builds for native_sim and the emulated Cortex-M3. The Logs tab was looked at in Flight Control 1.4.0's UI.
