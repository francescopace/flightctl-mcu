#!/usr/bin/env bash
set -euo pipefail
exec "$(dirname "$0")/sign-arm.sh" pico2w "$@"
