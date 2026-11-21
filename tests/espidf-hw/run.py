"""Run the ESP-IDF and Zephyr wiring fixtures with esp-emu and its release examples."""
import argparse
import json
import re
import socket
import subprocess
import tempfile
from contextlib import contextmanager
from pathlib import Path


# Calibrated GPIO3 readings at the potentiometer's 50% and 60% positions.
ADC_MV = {"esp32c3": (1640, 1962), "esp32s3": (1650, 1980)}


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


def send_input(port, part, event):
    if control(port, f"part {part} input {event}") != "ok":
        raise RuntimeError(f"{part} refused {event}")


def console(emu):
    for line in emu.stdout:
        print(line, end="", flush=True)
        yield line.strip()
    raise RuntimeError(f"emulator exited before the expected result (status {emu.wait()})")


@contextmanager
def emulate(args, chip, board, *, zephyr=False):
    with tempfile.TemporaryDirectory() as temp:
        for part in board["parts"].values():
            part["module"] = str((args.parts / part["module"]).resolve(strict=True))
        board_file = Path(temp) / "board.json"
        board_file.write_text(json.dumps(board))
        firmware = args.firmware.resolve()
        if zephyr:
            # Simple Boot at offset zero, with erased flash for LittleFS.
            flash = Path(temp) / "flash.bin"
            flash.write_bytes(firmware.read_bytes().ljust(8 * 1024 * 1024, b"\xff"))
            firmware = flash
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        cmd = [args.emulator, "--chip", chip, "--firmware", str(firmware),
               "--cosim-board", str(board_file), "--control-tcp", f"127.0.0.1:{port}",
               "--timeout", "60s"]
        if zephyr:
            cmd.extend(["--psram-size", "8M"])
        with subprocess.Popen(cmd, cwd=temp, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, errors="replace") as emu:
            try:
                yield console(emu), port
            finally:
                if emu.poll() is None:
                    emu.terminate()


def check_adc(lines, port, chip):
    for stage, applied, expected in zip(("first", "second"), (1650, 1980), ADC_MV[chip]):
        line = next(line for line in lines if line.startswith("ADC "))
        if line != f"ADC {stage} {expected}":
            raise RuntimeError(f"expected {stage} reading of {expected} mV, got {line}")
        state = control(port, "part knob state")
        if f"millivolts={applied}" not in state.split():
            raise RuntimeError(f"expected {applied} mV on GPIO3, potentiometer reports {state}")
        print(f"GPIO3: {state}", flush=True)
        if stage == "first":
            send_input(port, "knob", "more")


def run_s3(args, negative=False):
    board = json.loads(Path(__file__).with_name("board.json").read_text())
    if negative:
        board["parts"]["thermometer"]["pins"]["CS"] = 14
    with emulate(args, "esp32s3", board) as (lines, port):
        events = []
        for line in lines:
            if line.startswith("WIRING FAIL:"):
                if negative and line == "WIRING FAIL: TMP123 sample":
                    print("Disconnected SPI chip-select detected.")
                    return
                raise RuntimeError(line)
            if line in ("WIRING press", "WIRING release"):
                event = line.split()[1]
                check_pulse(port, 1000 if event == "press" else 2000)
                pixel = "0=#123456" if event == "press" else "0=#000000"
                state = control(port, "part strip state")
                if not {pixel, "pixels=1"}.issubset(state.split()):
                    raise RuntimeError(f"expected a single pixel {pixel} on GPIO48, strip reports {state}")
                print(f"GPIO48: {state}", flush=True)
                send_input(port, "button", event)
                if event == "release":
                    check_adc(lines, port, "esp32s3")
                events.append(event)
            if line == "WIRING PASS":
                if negative or events != ["press", "release"]:
                    raise RuntimeError("unexpected wiring pass")
                return


def run_adc(args):
    board = {"parts": {"knob": {"module": "analog/potentiometer.wasm", "pins": {"W": 3}}}}
    with emulate(args, "esp32c3", board) as (lines, port):
        check_adc(lines, port, "esp32c3")


def run_zephyr(args):
    board = {"parts": {"button": {"module": "button/pushbutton.wasm", "pins": {"A": 5}}}}
    with emulate(args, "esp32s3", board, zephyr=True) as (lines, port):
        expected = [f"BUTTON {name} {action}" for name in ("button", "fleet-button")
                    for action in ("press", "release")] + ["BUTTON PASS"]
        for line in lines:
            if line.startswith("BUTTON "):
                if line != expected.pop(0):
                    raise RuntimeError(f"unexpected button test result: {line}")
                if not expected:
                    return
                send_input(port, "button", line.split()[-1])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("firmware", type=Path)
    parser.add_argument("parts", type=Path, help="extracted esp-emu examples directory")
    parser.add_argument("--emulator", default="esp-emu")
    parser.add_argument("--fixture", choices=("s3", "adc", "zephyr"), default="s3")
    args = parser.parse_args()
    if args.fixture == "adc":
        run_adc(args)
    elif args.fixture == "zephyr":
        run_zephyr(args)
    else:
        run_s3(args)
        run_s3(args, negative=True)
