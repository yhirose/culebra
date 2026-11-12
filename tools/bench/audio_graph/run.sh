#!/usr/bin/env bash
# Audio.Graph's engine, now and at a git ref, side by side (bench.cc says
# what it reads). Each side is its own bench.cc built against its own
# include/ (git archive), at the release build's compiler and level, so the
# two compile whatever the engine's C++ calls were then. unicodelib, which
# base/shared.h includes and the engine does not use, is this checkout's for
# both (git archive leaves submodules out).
#
#   run.sh            the table for this checkout
#   run.sh <ref>      this checkout against <ref>: per graph, whether the
#                     samples are the same, and the time a sample takes now
#                     over then (the best of several rounds, the two taking
#                     turns); against itself it shows the noise floor
#   run.sh --check    builds without optimizing and renders each graph
#                     briefly (check-generated: the bench still builds, so a
#                     later version can be compared with this one)
#
# Samples compare on one machine only: libm's sin and exp differ in their
# last bits between systems.
set -euo pipefail
cd "$(dirname "$0")/../../.."

CXX="${CXX:-c++}"
BENCH=tools/bench/audio_graph/bench.cc
INCLUDES=(-I vendor/cpp-unicodelib)
ROUNDS=4

work="$(mktemp -d "${TMPDIR:-/tmp}/bench-audio-graph.XXXXXX")"
trap 'rm -rf "$work"' EXIT

# build <out> <include dir> <bench.cc> <flags...>: compiled through ccache
# when there is one, as _gen-blob-tool is, so the check is warm after the
# first run.
build() {
  local out="$1" include="$2" src="$3" ccache
  shift 3
  ccache="$(command -v ccache || true)"
  $ccache "$CXX" -std=c++23 "$@" -Wall -Wextra -I "$include" "${INCLUDES[@]}" -c "$src" -o "$out.o"
  "$CXX" "$out.o" -o "$out"
}

# Warnings fail only here, on this checkout's headers: a ref's, built with a
# newer compiler, may warn where it did not, and must still build.
if [[ "${1:-}" == "--check" ]]; then
  build "$work/bench" include "$BENCH" -O0 -Werror
  "$work/bench" --quick
  exit 0
fi

echo "compiler: $("$CXX" --version | sed -n 1p)"
build "$work/now" include "$BENCH" -O3 -DNDEBUG

if [[ $# -eq 0 ]]; then
  "$work/now" --time 5 | column -t -s $'\t'
  exit 0
fi

ref="$1"
if ! git cat-file -e "$ref:$BENCH" 2>/dev/null; then
  echo "bench-audio-graph: $ref has no $BENCH (the bench starts with the commit that added it)" >&2
  exit 1
fi
mkdir -p "$work/ref"
git archive "$ref" include "$BENCH" | tar -x -C "$work/ref"
build "$work/then" "$work/ref/include" "$work/ref/$BENCH" -O3 -DNDEBUG

mkdir -p "$work/samples_now" "$work/samples_then"
"$work/now" --samples "$work/samples_now"
"$work/then" --samples "$work/samples_then"

# The two take turns, so a machine growing busier weighs on both.
for round in $(seq "$ROUNDS"); do
  "$work/now" --time 3 > "$work/times_now.$round"
  "$work/then" --time 3 > "$work/times_then.$round"
done

# Per graph, the best ns a sample over the rounds, of one side's tables.
best() {
  awk -F'\t' 'FNR > 1 { if (!($1 in b) || $4 < b[$1]) b[$1] = $4 } END { for (g in b) print g "\t" b[g] }' "$@"
}
best "$work"/times_now.* | sort > "$work/now.best"
best "$work"/times_then.* | sort > "$work/then.best"
"$work/now" --compare "$work/samples_then" "$work/samples_now" | sort > "$work/samples"

echo "this checkout against $ref ($(git rev-parse --short "$ref"))"
{
  printf 'graph\tns a sample now\tthen\tnow/then\tsamples\n'
  join -t $'\t' -a 1 -e '-' -o 0,1.2,2.2 "$work/now.best" "$work/then.best" |
    awk -F'\t' '{ r = ($3 == "-" || $3 == 0) ? "-" : sprintf("%.3f", $2 / $3); print $1 "\t" $2 "\t" $3 "\t" r }' |
    join -t $'\t' -a 1 -e '-' -o 0,1.2,1.3,1.4,2.2 - <(grep -v '^graph' "$work/samples")
} | column -t -s $'\t'
