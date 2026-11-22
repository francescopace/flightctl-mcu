#!/usr/bin/env python3
"""Materialize the C5 board from the common Espressif port in NuttX 13.1.0."""

import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BOARD_PATHS = (
    "arch/risc-v/src/esp32c6",
    "arch/risc-v/include/esp32c6",
    "boards/risc-v/esp32c6/common",
    "boards/risc-v/esp32c6/esp32c6-devkitc",
)


def c5(text):
    return text.replace("esp32c6", "esp32c5").replace("ESP32C6", "ESP32C5").replace("ESP32-C6", "ESP32-C5")


def apply(repo, filename):
    patch = HERE / filename
    cmd = ["git", "-C", str(repo), "apply"]
    if subprocess.run([*cmd, "--reverse", "--check", patch], stderr=subprocess.DEVNULL).returncode:
        subprocess.run([*cmd, patch], check=True)


def board_files(repo):
    paths = subprocess.check_output(["git", "ls-files", *BOARD_PATHS], cwd=repo, text=True)
    return [p for p in paths.splitlines() if "/configs/" not in p or "/configs/wifi/" in p]


def write_changed(path, text):
    if not path.exists() or path.read_text() != text:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)


def kernel(repo):
    for path in board_files(repo):
        target = repo / c5(path)
        if not target.exists():
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(c5((repo / path).read_text()))
    apply(repo, "nuttx.patch")
    apply(repo, "clock.patch")


def sdk_defaults(hal):
    template = c5((hal / "nuttx/esp32c6/include/sdkconfig.h").read_text())
    # Espressif generates both Kconfig files directly from the chip capability headers.
    for prefix, kconfig in [
        ("SOC_", "components/soc/esp32c5/include/soc/Kconfig.soc_caps.in"),
        ("ESP_ROM_", "components/esp_rom/esp32c5/Kconfig.soc_caps.in"),
    ]:
        definitions = []
        for symbol, value in re.findall(r"^config (\w+)\n\s+(?:bool|int|hex)\n\s+default (\S+)\s*$",
                                        (hal / kconfig).read_text(), re.M):
            if value != "n":
                definitions.append(f"#define CONFIG_{symbol} {1 if value == 'y' else value}")
        if not definitions:
            raise ValueError(f"missing capabilities in {kconfig}")
        template = re.sub(r"^#define CONFIG_" + prefix + r"[^\n]+\n", "", template, flags=re.M)
        template = template.replace("#pragma once\n", "#pragma once\n\n" + "\n".join(definitions) + "\n", 1)
    return template


def interrupts(hal):
    template = (hal / "nuttx/esp32c6/include/irq.h").read_text()
    header = (hal / "components/soc/esp32c5/include/soc/interrupts.h").read_text()
    numbers = {}
    current = -1
    for symbol, value in re.findall(r"^\s*ETS_(\w+)\s*(?:=\s*(\w+))?\s*,", header, re.M):
        if value:
            current = numbers[value[4:]] if value.startswith("ETS_") else int(value, 0)
        else:
            current += 1
        numbers[symbol] = current
    count = numbers.pop("MAX_INTR_SOURCE")
    defines = [f"#define {name} {number}" for name, number in numbers.items()]
    defines += [f"#define SYSTIMER_TARGET{i}_EDGE_INTR_SOURCE SYSTIMER_TARGET{i}_INTR_SOURCE" for i in range(3)]
    first = template.index("#define WIFI_MAC_INTR_SOURCE")
    last = template.index("#define ESP_NSOURCES")
    template = template[:first] + "\n".join(defines) + "\n\n" + template[last:]
    return re.sub(r"(#define ESP_NSOURCES\s+)\d+", lambda m: m[1] + str(count), template)


def platform(hal):
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=hal, text=True).strip()
    if revision != "b90b1837cb5ad24747deb4c895246037cc206ce5":
        raise ValueError(f"unexpected C5 HAL revision {revision}")
    output = hal / "nuttx/esp32c5/include"
    config = output / "sdkconfig.h"
    if not config.exists():
        write_changed(config, sdk_defaults(hal))
    write_changed(output / "irq.h", interrupts(hal))
    apply(hal, "hal.patch")


if __name__ == "__main__":
    {"kernel": kernel, "hal": platform}[sys.argv[1]](Path(sys.argv[2]).resolve())
