#!/usr/bin/env bash
# Builds the libFuzzer targets (tests/fuzz) and runs each for a while.
# Needs clang with libFuzzer (Linux: apt clang; macOS: brew install llvm) and
# mbedTLS 4. Usage: [FUZZ_SECONDS=60] [FUZZ_JOBS=1] [CC=clang] tests/fuzz.sh [cmake args...]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
SECONDS_EACH="${FUZZ_SECONDS:-60}"
CC="${CC:-$( [[ -x /opt/homebrew/opt/llvm/bin/clang ]] && echo /opt/homebrew/opt/llvm/bin/clang || echo clang )}"
# Run in bounded batches (also on macOS Bash 3, which lacks wait -n).
jobs="${FUZZ_JOBS:-1}"
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "FUZZ_JOBS must be a positive integer" >&2; exit 2; }
cmake -S . -B build/fuzz -G Ninja -DFC_FUZZ=ON -DCMAKE_C_COMPILER="$CC" "$@" >/dev/null
cmake --build build/fuzz --target fuzz_http fuzz_oci fuzz_agent fuzz_delta fuzz_console
cd build/fuzz

run_target() {
  local t="$1" out="findings/$1"
  mkdir -p "$out" "corpus_$t"
  echo "== fuzz_$t (${SECONDS_EACH}s)"
  # New inputs go to build/fuzz/corpus_<t>; the seeds in tests/fuzz/corpus
  # (including past crashes) are replayed first.
  if "./fuzz_$t" -max_total_time="$SECONDS_EACH" -max_len=16384 -print_final_stats=1 \
       -artifact_prefix="$out/" "corpus_$t" "$ROOT/tests/fuzz/corpus/$t" > "$out/run.log" 2>&1; then
    grep -E '^Done|stat::number_of_executed_units' "$out/run.log" | sed "s/^/fuzz_$t: /" || true
  else
    tail -60 "$out/run.log" >&2
    echo "fuzz_$t failed; full log and any reproducer in build/fuzz/$out" >&2
    return 1
  fi
}

status=0
targets=(http oci agent delta console)
for ((first=0; first<${#targets[@]}; first+=jobs)); do
  pids=()
  for t in "${targets[@]:first:jobs}"; do
    run_target "$t" &
    pids+=("$!")
  done
  for pid in "${pids[@]}"; do
    wait "$pid" || status=1
  done
done
exit "$status"
