#!/usr/bin/env bash
# Run a Linux wiring fixture against the emulator and parts from one release.
set -euo pipefail
fixture=$1
firmware=$(realpath "$2")
runner=$(realpath "$(dirname "$0")/run.py")
version=${ESP_EMU_VERSION:?set ESP_EMU_VERSION to the pinned release}
base="https://github.com/espressif/esp-emulator/releases/download/v$version"
emu="esp-emu-$version-$(uname -m)-unknown-linux-gnu"
parts="esp-emu-$version-examples"
temp=$(mktemp -d)
trap 'rm -rf "$temp"' EXIT
cd "$temp"
wget -nv "$base/SHA256SUMS"
for name in "$emu" "$parts"; do
    wget -nv "$base/$name.tar.gz"
    grep -F " $name.tar.gz" SHA256SUMS | sha256sum -c -
    tar xzf "$name.tar.gz"
done
python3 "$runner" "$firmware" "$parts" --emulator "$PWD/$emu/esp-emu" --fixture "$fixture"
