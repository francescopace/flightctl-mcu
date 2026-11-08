#!/usr/bin/env bash
# Builds the libFuzzer targets (tests/fuzz) and runs each for a while.
# Needs clang with libFuzzer (Linux: apt clang; macOS: brew install llvm) and
# mbedTLS 3.6. Usage: [FUZZ_SECONDS=60] [CC=clang] ci/fuzz.sh [cmake args...]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
SECONDS_EACH="${FUZZ_SECONDS:-60}"
CC="${CC:-$( [[ -x /opt/homebrew/opt/llvm/bin/clang ]] && echo /opt/homebrew/opt/llvm/bin/clang || echo clang )}"
cmake -S . -B build/fuzz -G Ninja -DFC_FUZZ=ON -DCMAKE_C_COMPILER="$CC" "$@" >/dev/null
cmake --build build/fuzz --target fuzz_http fuzz_oci fuzz_agent
status=0
for t in http oci agent; do
  mkdir -p "build/fuzz/corpus_$t"
  echo "== fuzz_$t (${SECONDS_EACH}s)"
  # New inputs go to build/fuzz/corpus_<t>; the seeds in tests/fuzz/corpus
  # (including past crashes) are replayed first.
  if ! (cd build/fuzz && "./fuzz_$t" -max_total_time="$SECONDS_EACH" -max_len=16384 -print_final_stats=1 \
        "corpus_$t" "$ROOT/tests/fuzz/corpus/$t" 2>&1 | grep -E '^Done|ERROR|SUMMARY|runtime error|stat::number_of_executed_units'); then
    :
  fi
  if ls build/fuzz/crash-* build/fuzz/leak-* build/fuzz/timeout-* >/dev/null 2>&1; then
    echo "fuzz_$t found a problem: input saved in build/fuzz" >&2
    status=1
  fi
done
exit $status
