#!/usr/bin/env bash
# Applies this repository's fixes to the fetched Zephyr tree
# (third_party/patches/<project>-*.patch, each with its reason), once.
# fetch-zephyr.sh and zephyr/build.sh run it.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WS="$ROOT/third_party/zephyr-ws"
for p in "$ROOT"/third_party/patches/*.patch; do
  [[ -f "$p" ]] || continue
  name="$(basename "$p")"
  case "$name" in
    hal_rpi_pico-*) dir="$WS/modules/hal/rpi_pico" ;;
    *) echo "$name: no tree to apply it to" >&2; exit 1 ;;
  esac
  [[ -d "$dir" ]] || continue
  if git -C "$dir" -c safe.directory='*' apply --reverse --check "$p" 2>/dev/null; then
    continue # already applied
  fi
  git -C "$dir" -c safe.directory='*' apply "$p"
  echo "applied $name"
done
