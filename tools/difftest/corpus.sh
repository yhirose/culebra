#!/usr/bin/env bash
# Building the generated corpus, shared by the two gates that consume it:
# run.sh diffs one binary's engines against each other, release_diff.sh diffs
# this binary's default engine against the previous release's. What they run
# differs; how the corpus is built must not, or the two gates stop talking
# about the same programs.
#
# Sourced, not executed. Every function is prefixed `corpus_`.

CORPUS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Generate the corpus into $work/cases.cul; print the case count.
corpus_generate() {
  local bin="$1" work="$2" cases
  mkdir -p "$work"
  # Which engine writes the corpus is not a detail: this is the program every
  # other lane is then compared over, so it is spelled rather than defaulted
  # (docs/internals/vm.md §13.1).
  if ! "$bin" --vm "$CORPUS_DIR/gen.cul" > "$work/cases.cul"; then
    echo "corpus: FAIL — generator gen.cul did not run cleanly" >&2
    return 1
  fi
  cases=$(grep -c '^_p(' "$work/cases.cul")
  # Sanity floor: the generator must emit a substantial corpus. A near-empty
  # count means gen.cul silently degraded (a swept dimension stopped emitting).
  if [ "$cases" -lt 1000 ]; then
    echo "corpus: FAIL — only $cases cases generated (expected >= 1000)" >&2
    return 1
  fi
  printf '%s\n' "$cases"
}

# Split the corpus into runnable chunk programs and leave their paths in the
# CORPUS_CHUNKS array. Each chunk file `c.xx` gets a sibling `c.xx.cul` of
# preamble + fixtures + cases; callers name the chunk, not the program.
#
# The JIT compiles each chunk as a single LLVM module and compile time grows
# super-linearly in module size — one 5000-case module takes ~2min where the
# same cases in chunks take a fraction. Chunking also gives the parallel
# runners independent units of work. Each `_p` case is self-contained, so a
# chunk never splits one, and every lane runs the identical chunk file, so
# error-record line numbers stay chunk-local but consistent across lanes.
# ~400 is the empirical sweet spot on an 8-core box (smaller modules compile
# faster and balance the pool better, but below ~256 per-process startup starts
# to dominate). Tunable.
corpus_chunk() {
  local work="$1" size="${2:-400}" chunkdir cf
  chunkdir="$work/chunks"
  rm -rf "$chunkdir"; mkdir -p "$chunkdir"
  split -l "$size" "$work/cases.cul" "$chunkdir/c."
  # Capture the bare chunk list before the .cul derivatives land beside them —
  # the glob must see only the split outputs.
  CORPUS_CHUNKS=( "$chunkdir"/c.* )
  for cf in "${CORPUS_CHUNKS[@]}"; do
    cat "$CORPUS_DIR/preamble.cul" "$CORPUS_DIR/canvas_fixtures.cul" "$cf" > "$cf.cul"
  done
}
