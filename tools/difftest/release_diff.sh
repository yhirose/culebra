#!/usr/bin/env bash
# Release-to-release differential: run the generated corpus under the previous
# release's binary and under this one, and report every case whose behaviour
# changed.
#
# What it is for. The executor and the LLVM lowering consume bytecode from one
# compiler, so a bug in that compiler makes both lanes give the same wrong
# answer and run.sh stays green (docs/internals/vm.md §10.3). A released
# binary is the one oracle that cannot drift with the working tree: it is a
# second implementation, frozen — for the language the last release could
# already express, an independent implementation keeps answering here,
# release after release.
#
# Both sides run on their DEFAULT engine, with no flag. That is deliberate and
# it is the only place in the repo that leaves the choice implicit: the
# question is what changed for someone who types `culebra prog.cul`, so the
# default is the subject rather than an oversight, and it is why
# CULEBRA_REQUIRE_EXPLICIT_ENGINE is unset for these runs (vm.md §2).
#
# Differences are expected — a release adds built-ins the older binary has
# never heard of. Each one has to be named in release_diff_allow.txt as a glob
# over the case label, which makes that file the draft of the release notes.
# Anything not named there fails the gate.
#
# Usage: tools/difftest/release_diff.sh <baseline-binary> [head-binary]
#        (head binary defaults to ./build/culebra)
set -uo pipefail

BASELINE="${1:-}"
CULEBRA="${2:-./build/culebra}"
if [ -z "$BASELINE" ]; then
  echo "usage: release_diff.sh <baseline-binary> [head-binary]" >&2
  exit 2
fi
[ -x "$BASELINE" ] || { echo "release-diff: $BASELINE is not executable" >&2; exit 2; }
[ -x "$CULEBRA" ] || { echo "release-diff: $CULEBRA is not executable" >&2; exit 2; }

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/corpus.sh"
WORK="${RELEASE_DIFF_WORK:-build/release-diff}"
ALLOW="$HERE/release_diff_allow.txt"

# Under a second, and it means a green run below is a comparison that was
# checked rather than one that merely printed OK.
selftest=$("$HERE/release_diff_selftest.sh" 2>&1) || {
  echo "release-diff: the comparator's own smoke test failed" >&2
  printf '%s\n' "$selftest" >&2
  exit 1
}

cases=$(corpus_generate "$CULEBRA" "$WORK") || exit 1
corpus_chunk "$WORK" "${DIFFTEST_CHUNK:-400}"
chunks=( "${CORPUS_CHUNKS[@]}" )

JOBS="${DIFFTEST_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}"

# One (chunk × side) job. Streams are captured apart and concatenated, never
# merged live — see run.sh for the ALSA-mid-word story; the same rearrangement
# on both sides keeps the comparison exact.
run_one() {
  local cf="$1" side="$2" bin
  case "$side" in b) bin="$BASELINE" ;; *) bin="$CULEBRA" ;; esac
  env -u CULEBRA_REQUIRE_EXPLICIT_ENGINE "$bin" "$cf.cul" \
    > "$cf.$side" 2> "$cf.$side.err"
  cat "$cf.$side.err" >> "$cf.$side"
  rm -f "$cf.$side.err"
}

# A case written in syntax the baseline predates does not fail alone: the chunk
# it sits in fails to parse, and every record after it disappears with it. Rerun
# that chunk one process per case, in parallel — the preamble is the only cost
# and there are cores. A case that still produces no record is emitted as
# `<label> ::: unsupported`, which keeps the two sides aligned record for record
# and tells the report exactly what the older binary could not express.
fallback_chunk() {
  local cf="$1" side="$2" bin d i=0 line label
  case "$side" in b) bin="$BASELINE" ;; *) bin="$CULEBRA" ;; esac
  d="$cf.$side.cases"; rm -rf "$d"; mkdir -p "$d"
  while IFS= read -r line; do
    i=$((i + 1))
    printf '%s\n' "$line" > "$d/$i.case"
    cat "$HERE/preamble.cul" "$HERE/canvas_fixtures.cul" "$d/$i.case" \
      > "$d/$i.cul"
  done < <(grep '^_p(' "$cf")
  printf '%s\n' "$d"/*.cul | xargs -P "$JOBS" -I '{}' \
    env -u CULEBRA_REQUIRE_EXPLICIT_ENGINE bash -c \
      '"$0" "$1" > "$1.out" 2>&1' "$bin" '{}'
  : > "$cf.$side"
  local n=$i
  for ((i = 1; i <= n; i++)); do
    if grep -q ' ::: ' "$d/$i.cul.out" 2>/dev/null; then
      cat "$d/$i.cul.out" >> "$cf.$side"
    else
      label=$(sed "s/^_p('\(.*\)', fn .*/\1/" "$d/$i.case")
      printf '%s ::: unsupported\n' "$label" >> "$cf.$side"
    fi
  done
  rm -rf "$d"
}

export -f run_one
export BASELINE CULEBRA
for cf in "${chunks[@]}"; do printf '%s\tb\n%s\th\n' "$cf" "$cf"; done \
  | xargs -P "$JOBS" -L1 bash -c 'run_one "$1" "$2"' _

# Completion guard, and the trigger for the fallback above: `_p` prints exactly
# one record per case, so a side with fewer records than the chunk has cases did
# not run to completion.
#
# RELEASE_DIFF_FORCE_FALLBACK takes that path for every chunk. The condition
# that reaches it otherwise — a release that predates a syntax now in the
# corpus — cannot be arranged on demand, and a path that has never run is a
# claim; with this it can be exercised against any baseline (~2 min).
out_base="$WORK/out_baseline.txt"; : > "$out_base"
out_head="$WORK/out_head.txt"; : > "$out_head"
for cf in "${chunks[@]}"; do
  cc=$(grep -c '^_p(' "$cf")
  # If either side needs the fallback, both take it. An error record carries
  # the line and column the throw came from, and those are positions in the
  # chunk program — rerunning one side per case renumbers every error record
  # on that side alone, which would invent a difference in every case that
  # threw. Only the baseline ever fails to parse a chunk, so this is exactly
  # the case that would have gone wrong.
  if [ "${RELEASE_DIFF_FORCE_FALLBACK:-0}" = 1 ] ||
     [ "$(grep -c ' ::: ' "$cf.b")" -lt "$cc" ] ||
     [ "$(grep -c ' ::: ' "$cf.h")" -lt "$cc" ]; then
    echo "release-diff: chunk $(basename "$cf") incomplete, rerunning per case" >&2
    fallback_chunk "$cf" b
    fallback_chunk "$cf" h
  fi
  cat "$cf.b" >> "$out_base"; cat "$cf.h" >> "$out_head"
done

"$HERE/release_diff.py" --baseline "$out_base" --head "$out_head" \
  --allow "$ALLOW" --cases "$cases" \
  --baseline-name "$(env -u CULEBRA_REQUIRE_EXPLICIT_ENGINE "$BASELINE" --version 2>&1 | head -1)" \
  --head-name "$(env -u CULEBRA_REQUIRE_EXPLICIT_ENGINE "$CULEBRA" --version 2>&1 | head -1)"
