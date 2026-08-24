#!/usr/bin/env bash
# Check the bytecode VM's consumers against the frozen expected outputs on
# every case file: the executor (--vm) and, when present, the LLVM lowering
# (--jit).
#
# Usage: [STRESS=1] compare.sh <culebra-binary> [lane-flag]
#        compare.sh --freeze <culebra-binary> [case.cul ...]
# With no lane flag, both compiled lanes are asserted in one run. STRESS=1
# runs the lanes under CULEBRA_GC_STRESS=1 (allocations forced to collect).
#
# expected/ was frozen from the tree-walker while all three engines agreed
# byte-for-byte (Phase 4 B7-c) — the B3 move again: expectations written
# down while an independent implementation could still countersign them. An
# intended behavior change re-freezes the touched case (`--freeze`, which
# rewrites expected/<case>.out + its rc.tsv row from a --vm run and then
# asserts --jit agrees); the diff review is where that intent is checked
# (like release_diff's allowlist). stdout+stderr and the exit code are both
# asserted (a SEGV must not read as agreement).
#
# CULEBRA_CANVAS_HEADLESS is pinned here as well as in the gate: a freeze or
# check without it bakes/reads `_Canvas.windowed() == true` and diverges
# (measured). TZ is pinned unconditionally for the same reason: the _Time
# cases do local-time calendar arithmetic, and a golden frozen in one zone
# fails the check in another (measured: JST-frozen values failed CI's UTC by
# five hours; the old live-reference comparison ran both sides on one
# machine, so the zone cancelled out and could never be seen).
set -u
export CULEBRA_CANVAS_HEADLESS="${CULEBRA_CANVAS_HEADLESS:-1}"
export TZ=UTC

FREEZE=0
if [ "${1:-}" = --freeze ]; then FREEZE=1; shift; fi
BIN="${1:-./build-dev/culebra}"
BIN="$(realpath "$BIN" 2>/dev/null || echo "$BIN")"
if [ ! -x "$BIN" ]; then
  echo "compare.sh: binary not found or not executable: $BIN" >&2
  exit 1
fi
if [ $# -ge 2 ]; then shift; ARGS=("$@"); else ARGS=(); fi
STRESS="${STRESS:-}"
tag="${STRESS:+ (GC_STRESS)}"
cd "$(dirname "$0")"

if [ "$FREEZE" = 1 ]; then
  # Re-freeze the named cases (default: all) from a --vm run, then assert
  # --jit reproduces every frozen file — the two consumers must agree on the
  # new expectation before it is committed.
  files=("${ARGS[@]:-}")
  [ -n "${files[0]:-}" ] || files=(*.cul)
  mkdir -p expected
  for f in "${files[@]}"; do
    f=$(basename "$f")
    n="${f%.cul}"
    out="$("$BIN" --vm "$f" 2>&1)"; rc=$?
    printf '%s' "$out" > "expected/$n.out"
    grep -v "^$n	" expected/rc.tsv 2>/dev/null > expected/rc.tsv.new || true
    printf '%s\t%s\n' "$n" "$rc" >> expected/rc.tsv.new
    LC_ALL=C sort expected/rc.tsv.new > expected/rc.tsv
    rm -f expected/rc.tsv.new
    out="$("$BIN" --jit "$f" 2>&1)"; rc_j=$?
    if [ "$rc_j" != "$rc" ] || ! printf '%s' "$out" | cmp -s - "expected/$n.out"; then
      echo "FREEZE-FAIL $f: --jit disagrees with the fresh --vm expectation" >&2
      exit 1
    fi
    echo "froze $f (rc=$rc)"
  done
  exit 0
fi

if [ ${#ARGS[@]} -ge 1 ]; then LANES=("${ARGS[0]}"); else LANES=(--vm --jit); fi

# Each (case, lane) pair is an independent process, so the sweep runs them in
# parallel like the gate's other per-file phases. Serial, this was the gate's
# largest phase (177 cases x 2 lanes x 2 passes). JOBS=1 recovers the old
# behavior when debugging.
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 8)}"
work="${TMPDIR:-/tmp}/culebra-vmcases-$$"
rm -rf "$work" && mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

# One case on one lane. Writes its report to a per-pair file (replayed in case
# order below, so output is stable whatever the completion order) and marks a
# mismatch with a .fail file.
check_one() {
  local n="$1" lane="$2" want_rc="$3" log b b_rc
  log="$work/$n.${lane#--}.log"
  # The runtime checks CULEBRA_GC_STRESS for presence, not value, so only
  # set it when stressing.
  if [ -n "$STRESS" ]; then
    b="$(CULEBRA_GC_STRESS=1 "$BIN" "$lane" "$n.cul" 2>&1)"; b_rc=$?
  else
    b="$("$BIN" "$lane" "$n.cul" 2>&1)"; b_rc=$?
  fi
  if [ "$b_rc" = "$want_rc" ] && printf '%s' "$b" | cmp -s - "expected/$n.out"; then
    echo "OK   $n.cul $lane$tag" > "$log"
  else
    {
      echo "FAIL $n.cul (expected rc=$want_rc, $lane$tag rc=$b_rc)"
      diff "expected/$n.out" <(printf '%s' "$b") | sed 's/^/     /'
    } > "$log"
    : > "$work/$n.${lane#--}.fail"
  fi
}
export -f check_one
export work BIN STRESS tag

# What the sweep cannot answer per lane: a case with no frozen expectation, and
# one whose expectation is a parse error. Both reject the case itself, so they
# run here and keep it out of the job list.
fail=0
for f in *.cul; do
  n="${f%.cul}"
  if [ ! -f "expected/$n.out" ]; then
    echo "FAIL $f has no expected/$n.out — freeze it: compare.sh --freeze <bin> $f" \
      > "$work/$n.pre.log"
    fail=1; continue
  fi
  # A case whose expected output IS an uncaught SyntaxError agrees on every
  # lane while testing nothing (they all print the same parse error). Two
  # files raise one on purpose (a getter with parameters, a duplicate trait
  # method); both are named error_*, which is the exemption. A case that
  # catches one and prints it (`err=SyntaxError|…`) is testing the error.
  if grep -q '^SyntaxError: ' "expected/$n.out"; then
    case "$f" in
      error_*) ;;
      *) { echo "FAIL $f does not parse (a case that cannot run is not a test)"
           head -2 "expected/$n.out" | sed 's/^/     /'; } > "$work/$n.pre.log"
         fail=1; continue;;
    esac
  fi
  want_rc=$(awk -F'\t' -v n="$n" '$1==n{print $2}' expected/rc.tsv)
  if [ -z "$want_rc" ]; then
    echo "FAIL $f has no rc.tsv row — freeze it: compare.sh --freeze <bin> $f" \
      > "$work/$n.pre.log"
    fail=1; continue
  fi
  # The expected code travels with the job so the sweep reads rc.tsv once per
  # case rather than once per (case, lane).
  for lane in "${LANES[@]}"; do printf '%s %s %s\n' "$n" "$lane" "$want_rc"; done
done > "$work/jobs"

# `-r` is a GNU extension, so guard the empty job list here instead: every case
# was rejected above, and there is nothing to sweep.
if [ -s "$work/jobs" ]; then
  xargs -n3 -P "$JOBS" bash -c 'check_one "$1" "$2" "$3"' _ < "$work/jobs" || fail=1
fi

# A pair with no report was never checked — a job that could not start, a
# worker that was killed. Silence is not agreement, so say so and fail.
for f in *.cul; do
  n="${f%.cul}"
  if [ -f "$work/$n.pre.log" ]; then cat "$work/$n.pre.log"; continue; fi
  for lane in "${LANES[@]}"; do
    if [ -f "$work/$n.${lane#--}.log" ]; then
      cat "$work/$n.${lane#--}.log"
    else
      echo "FAIL $f $lane$tag: the check did not run"
      fail=1
    fi
  done
done
compgen -G "$work/*.fail" > /dev/null && fail=1
exit $fail
