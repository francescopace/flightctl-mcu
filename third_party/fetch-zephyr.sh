#!/usr/bin/env bash
# Fetches Zephyr at a pinned release, with only the modules the port needs,
# into third_party/zephyr-ws (a west workspace, git-ignored). Needs west: this
# machine's, or the one in the Zephyr build image (zephyr/build.sh uses the
# same image).
# v4.5 has mbedTLS 4.1.1, the same as ESP-IDF 6.0.3: the agent core is
# written for mbedTLS 4, whose cryptography is PSA only (TF-PSA-Crypto, a
# module of its own).
set -euo pipefail
ZEPHYR_TAG="${ZEPHYR_TAG:-v4.5.0}"
ZEPHYR_COMMIT="${ZEPHYR_COMMIT:-41d10b59730406bbab4ca9f1fbbe9e96aafb4966}"
ZEPHYR_IMAGE="${ZEPHYR_IMAGE:-ghcr.io/zephyrproject-rtos/ci-base:v0.29.1}"
# mbedtls: TLS. littlefs: storage. mcuboot, zcbor: firmware slots. The rest:
# the Raspberry Pi Pico W (RP2040, CYW43439 WiFi).
MODULES="${ZEPHYR_MODULES:-mbedtls tf-psa-crypto littlefs mcuboot zcbor cmsis cmsis_6 hal_rpi_pico hal_infineon picolibc}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WS="$ROOT/third_party/zephyr-ws"
if [[ -d "$WS/zephyr/.git" && "$(git -C "$WS/zephyr" rev-parse HEAD)" == "$ZEPHYR_COMMIT" && -f "$WS/.fetched" ]]; then
  echo "Zephyr $ZEPHYR_TAG already in $WS"
  exit 0
fi
if ! command -v west >/dev/null; then
  exec docker run --rm -v "$ROOT:/project" -w /project \
    -e ZEPHYR_TAG="$ZEPHYR_TAG" -e ZEPHYR_COMMIT="$ZEPHYR_COMMIT" -e ZEPHYR_MODULES="$MODULES" \
    "$ZEPHYR_IMAGE" third_party/fetch-zephyr.sh
fi
git config --global --add safe.directory '*' 2>/dev/null || true
rm -rf "$WS"
west init -m https://github.com/zephyrproject-rtos/zephyr --mr "$ZEPHYR_TAG" --clone-opt=--depth=1 "$WS"
# A tag can be moved: the commit cannot.
got="$(git -C "$WS/zephyr" rev-parse HEAD)"
if [[ "$got" != "$ZEPHYR_COMMIT" ]]; then
  echo "Zephyr $ZEPHYR_TAG is commit $got, expected $ZEPHYR_COMMIT" >&2
  rm -rf "$WS"
  exit 1
fi
cd "$WS"
filter="-.*"
for m in $MODULES; do filter="$filter,+$m"; done
west config manifest.project-filter -- "$filter"
west update --narrow -o=--depth=1
touch "$WS/.fetched"
echo "Zephyr $ZEPHYR_TAG in $WS"
