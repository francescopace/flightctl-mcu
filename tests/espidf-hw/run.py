"""Run the ESP32-S3 wiring fixture with esp-emu and its release examples."""
import argparse
import json
import re
import socket
import subprocess
import tempfile
from pathlib import Path


def control(port, command):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        sock.sendall((command + "\n").encode())
        with sock.makefile("r") as reply:
            return reply.readline().strip()


def check_pulse(port, width_us):
    state = control(port, "part servo state")
    match = re.search(r"driven=true pulse_us=(\d+)", state)
    # LEDC uses 14-bit duty at 50 Hz, so each step is about 1.22 us.
    if not match or abs(int(match[1]) - width_us) > 2:
        raise RuntimeError(f"expected {width_us} us on GPIO6, servo reports {state}")
    print(f"GPIO6: {state}", flush=True)


def run(args, negative=False):
    with tempfile.TemporaryDirectory() as temp:
        board = json.loads(Path(__file__).with_name("board.json").read_text())
        for part in board["parts"].values():
            part["module"] = str((args.parts / part["module"]).resolve(strict=True))
        if negative:
            board["parts"]["thermometer"]["pins"]["CS"] = 14
        board_file = Path(temp) / "board.json"
        board_file.write_text(json.dumps(board))
        # The control server needs a concrete port in its command line.
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        cmd = [args.emulator, "--chip", "esp32s3", "--firmware", str(args.firmware.resolve()),
               "--cosim-board", str(board_file), "--control-tcp", f"127.0.0.1:{port}",
               "--timeout", "60s"]
        with subprocess.Popen(cmd, cwd=temp, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, errors="replace") as emu:
            try:
                events = []
                for line in emu.stdout:
                    print(line, end="", flush=True)
                    line = line.strip()
                    if line.startswith("WIRING FAIL:"):
                        if negative and line == "WIRING FAIL: TMP123 sample":
                            print("Disconnected SPI chip-select detected.")
                            return
                        raise RuntimeError(line)
                    if line in ("WIRING press", "WIRING release"):
                        event = line.split()[1]
                        check_pulse(port, 1000 if event == "press" else 2000)
                        if control(port, f"part button input {event}") != "ok":
                            raise RuntimeError(f"button refused {event}")
                        events.append(event)
                    if line == "WIRING PASS":
                        if negative or events != ["press", "release"]:
                            raise RuntimeError("unexpected wiring pass")
                        return
                raise RuntimeError(f"emulator exited before the expected result (status {emu.wait()})")
            finally:
                if emu.poll() is None:
                    emu.terminate()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("firmware", type=Path)
    parser.add_argument("parts", type=Path, help="extracted esp-emu examples directory")
    parser.add_argument("--emulator", default="esp-emu")
    args = parser.parse_args()
    run(args)
    run(args, negative=True)
