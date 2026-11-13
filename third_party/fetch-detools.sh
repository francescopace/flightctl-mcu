#!/usr/bin/env bash
# Fetches detools (https://github.com/eerimoq/detools) at a pinned release into
# third_party/detools (git-ignored). The agent applies firmware deltas with
# its C library and the heatshrink decoder that comes with it
# (core/src/fc_delta.c); tools/fw-push.py makes them with the Python
# package of the same version.
set -euo pipefail
DETOOLS_TAG="${DETOOLS_TAG:-0.53.1}"
DETOOLS_COMMIT="${DETOOLS_COMMIT:-a7cc5383e60959ba5d3f87da9bd7aa04aa39e3c8}"
DIR="$(cd "$(dirname "$0")" && pwd)/detools"
if [[ -d "$DIR/.git" && "$(git -C "$DIR" rev-parse HEAD)" == "$DETOOLS_COMMIT" ]]; then
  echo "detools already in $DIR ($DETOOLS_TAG)"
  exit 0
fi
rm -rf "$DIR"
git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$DETOOLS_TAG" https://github.com/eerimoq/detools "$DIR"
# A tag can be moved: the commit cannot.
got="$(git -C "$DIR" rev-parse HEAD)"
if [[ "$got" != "$DETOOLS_COMMIT" ]]; then
  echo "detools $DETOOLS_TAG is commit $got, expected $DETOOLS_COMMIT" >&2
  rm -rf "$DIR"
  exit 1
fi
echo "detools $DETOOLS_TAG in $DIR"
