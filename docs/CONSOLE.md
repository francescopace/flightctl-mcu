# Remote console

Flight Control's console reaches the device as it reaches a Linux one: `flightctl console device/<name>`, or the UI's terminal. A microcontroller has no shell, so the device answers a closed set of commands instead:

| Command | What it gives |
|---|---|
| `help` | the commands |
| `status` | the device's name, system, memory, storage, and its `customInfo` (heap, firmware, network) |
| `apps` | the WASM apps, their status and the state each reports |
| `reboot` | restarts the device, once the session has ended |
| `exit` | ends the session (so does Ctrl-D on an empty line) |

One command per session:

```bash
flightctl console device/<name> -- status
```

Or a prompt, in a terminal (`--tty`, and the UI's terminal), with the line edited on the device: Backspace, Ctrl-C to drop the line, Ctrl-U; arrow keys are ignored. An unknown command answers so and, as a session's one command, exits with status 127.

## How it works

Flight Control puts the session in the device's rendered spec (`spec.consoles`), and the device opens a gRPC stream for it, `flightctl.v1.RouterService/Stream`, on the agent's own HTTP/2 connection: the agent endpoint serves gRPC on the same port, so the console costs no TLS connection of its own. The server joins it to the user's side. The bytes on it are Kubernetes' remote command protocol (`v5.channel.k8s.io`): stdin comes in, stdout and the exit status go out.

While a session is listed, the server answers the spec's long-poll at once with the version the device has; the device then only handles the console, and waits 5 s before asking again. It remembers the last 4 sessions, across restarts, so a `reboot` is not asked again after the restart.

## Limits

- **One session at a time.** A second gets "another console session is open on this device" and fails.
- **A session ends** after 10 minutes without input, and when the device needs its connection for a download (a firmware or an app takes the connection's turn, [REGISTRY.md](REGISTRY.md#memory)).
- **Input**: up to 4 KB not yet read by the device; more at once (a large paste) ends the session.
- **RAM**: about 12 KB of heap while a session lasts (its thread's stack, the stream's buffer, the session), none otherwise.
- **No log yet.** The UI's log tab runs `journalctl` through the console; the device does not keep its log ([ROADMAP.md](ROADMAP.md)).
- **Access** is Flight Control's: whoever may open a console on the device may also restart it.

Tried against Flight Control 1.4.0 on the classic ESP32, C5, C6 and S3 (ESP-IDF), and in the end-to-end test on the simulated device and the Zephyr builds for native_sim and the emulated Cortex-M3. Not yet on a board with Zephyr.
