#!/usr/bin/env bash
# Suite-wide GAP5 gate: run the whole difftest corpus under the loud inflated-RC
# audit (CULEBRA_GC_NEVER=1 CULEBRA_GC_LEAK_ABORT=1) and fail on any acyclic RC
# leak whose case label is not in tools/difftest/leak_abort_allow.txt.
#
# This is the throw-path-aware complement to leak.sh. leak.sh is growth-based:
# its `_p` runs a warmup call and DROPS any case that throws, so it structurally
# never measures throw-path leaks. Here every case runs once (result discarded)
# under the teardown inflated-RC audit, which fires whether or not the thunk
# threw — closing that blind spot. (leak.sh stays: it confirms an absolute-zero
# per-iteration leak on the non-throwing subset via N-proportional growth; this
# gate confirms absolute-zero inflated-RC over throw-paths too.)
#
# Because the audit aborts the process at the first teardown that finds a leak,
# a whole chunk aborts if any case in it leaks and we can't read which. So we
# scan in two phases: run each chunk (fast, most are clean), then for the few
# chunks that abort, re-run each of their cases solo to attribute the leak to a
# label. A label is either allowlisted (a known structural wall — see the
# allowlist) or a NEW leak that fails the gate.
#
# Pass a NO-LTO binary (build-dev/ or build-gate/). The audit rides the
# conservative scan's completeness; LTO's altered stack layout aliases leaked
# objects as live and under-reports (build/ shows 0). The `just test` gate runs
# this against build-gate.
#
# Usage:
#   tools/difftest/leak_abort_suite.sh [culebra-binary]           # gate
#   tools/difftest/leak_abort_suite.sh [culebra-binary] --update-allowlist
set -uo pipefail

# Default to the no-LTO dev build: the audit/growth oracles under-report on
# an LTO binary (stale-stack aliasing), so a stale ./build/ default has caused
# false-green runs twice. Pass build-gate/culebra explicitly from the gate.
CULEBRA="${1:-./build-dev/culebra}"
[ -x "$CULEBRA" ] || { echo "error: $CULEBRA not found/executable — pass a no-LTO culebra binary" >&2; exit 1; }
[ "${2:-}" = --update-allowlist ] && UPDATE=1 || UPDATE=0
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${LEAKFUZZ_WORK:-build/leakabort-suite}"
ALLOW="$HERE/leak_abort_allow.txt"
PREAMBLE="$HERE/leak_abort_preamble.cul"
mkdir -p "$WORK"

# Phase 0: the bare fixtures. Every corpus case is a thunk `_p` calls inside a
# try, and a thunk is an ordinary function with its own cleanup pad — so the
# corpus cannot express "an uncaught error leaves the top-level frame". These
# run as whole programs, with no preamble and no wrapper. A fixture leaks only
# if leak_abort_bare_allow.txt says it may; one that stops leaking is reported
# so the entry can be dropped. Both backends run: the audit is JIT-side, but a
# divergence in what the two print is a bug either way.
BARE_ALLOW="$HERE/leak_abort_bare_allow.txt"
bare_fail=0
for bf in "$HERE"/leak_abort_bare.d/*.cul; do
  [ -e "$bf" ] || break
  name=$(basename "$bf")
  grep -qx "$name" "$BARE_ALLOW" 2>/dev/null && allowed=1 || allowed=0
  bi=$(CULEBRA_GC_NEVER=1 CULEBRA_GC_LEAK_ABORT=1 ASAN_OPTIONS=detect_leaks=0 \
         "$CULEBRA" --tree "$bf" 2>&1); bi_rc=$?
  bj=$(CULEBRA_GC_NEVER=1 CULEBRA_GC_LEAK_ABORT=1 ASAN_OPTIONS=detect_leaks=0 \
         "$CULEBRA" --jit "$bf" 2>&1); bj_rc=$?
  # SIGABRT (134) is the audit firing; anything else is the program's own exit.
  if [ "$bj_rc" = 134 ] || [ "$bi_rc" = 134 ]; then
    if [ "$allowed" = 0 ]; then
      echo "leak-abort-suite: FAIL — $name leaked (interp rc=$bi_rc jit rc=$bj_rc)" >&2
      printf '%s\n' "$bj" | sed 's/^/    /' >&2
      bare_fail=1
    fi
  elif [ "$allowed" = 1 ]; then
    echo "leak-abort-suite: $name no longer leaks — drop it from" \
         "$(basename "$BARE_ALLOW")"
  elif [ "$bi" != "$bj" ] || [ "$bi_rc" != "$bj_rc" ]; then
    echo "leak-abort-suite: FAIL — $name differs between backends" >&2
    echo "  interp (rc=$bi_rc): $bi" >&2
    echo "  jit    (rc=$bj_rc): $bj" >&2
    bare_fail=1
  fi
done
[ "$bare_fail" = 0 ] || exit 1

# Generate cases, then chunk. Chunk size balances JIT compile time against the
# per-case fallback cost (a bigger chunk clears more clean cases per process but
# means more solo re-runs when it aborts). 471 keeps ~12 chunks over the corpus.
if ! "$CULEBRA" --tree "$HERE/gen.cul" > "$WORK/cases.cul"; then
  echo "leak-abort-suite: FAIL — generator gen.cul did not run cleanly" >&2; exit 1
fi
cases=$(grep -c '^_p(' "$WORK/cases.cul")
if [ "$cases" -lt 1000 ]; then
  echo "leak-abort-suite: FAIL — only $cases cases generated (expected >= 1000)" >&2; exit 1
fi

CHUNK="${LEAKABORT_CHUNK:-471}"
chunkdir="$WORK/chunks"; rm -rf "$chunkdir"; mkdir -p "$chunkdir"
split -l "$CHUNK" "$WORK/cases.cul" "$chunkdir/c."
chunks=( "$chunkdir"/c.* )
for cf in "${chunks[@]}"; do cat "$PREAMBLE" "$HERE/canvas_fixtures.cul" "$cf" > "$cf.cul"; done

JOBS="${LEAKABORT_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}"

# Phase 1: run each chunk under the audit. A non-zero exit (SIGABRT=134) means
# the chunk holds at least one leaking case. Emit the aborting chunk basenames.
run_chunk() {
  local cf="$1"
  CULEBRA_GC_NEVER=1 CULEBRA_GC_LEAK_ABORT=1 ASAN_OPTIONS=detect_leaks=0 \
    "$CULEBRA" --jit "$cf.cul" >/dev/null 2>/dev/null
  [ "$?" != 0 ] && echo "$cf"
  return 0
}
export -f run_chunk; export CULEBRA
printf '%s\n' "${chunks[@]}" \
  | xargs -P "$JOBS" -I{} bash -c 'run_chunk "$@"' _ {} \
  | LC_ALL=C sort > "$WORK/aborting_chunks.txt"

# Phase 2: for each aborting chunk, split into one file per case (preserving the
# whole line — labels contain spaces/commas/quotes, so pass files by name, never
# case bodies as xargs args) and run each solo to attribute the leak to a label.
casedir="$WORK/cases_solo"; rm -rf "$casedir"; mkdir -p "$casedir"
: > "$WORK/casemap.txt"
n=0
while IFS= read -r cf; do
  [ -n "$cf" ] || continue
  while IFS= read -r line; do
    case "$line" in _p\(*) ;; *) continue ;; esac
    f="$casedir/c_$n.cul"
    cat "$PREAMBLE" "$HERE/canvas_fixtures.cul" > "$f"
    printf '%s\n' "$line" >> "$f"
    label=$(printf '%s\n' "$line" | sed "s/^_p('\([^']*\)'.*/\1/")
    printf '%s\t%s\n' "$f" "$label" >> "$WORK/casemap.txt"
    n=$((n + 1))
  done < "$cf"
done < "$WORK/aborting_chunks.txt"

run_solo() {
  local f="$1"
  CULEBRA_GC_NEVER=1 CULEBRA_GC_LEAK_ABORT=1 ASAN_OPTIONS=detect_leaks=0 \
    "$CULEBRA" --jit "$f" >/dev/null 2>/dev/null
  [ "$?" != 0 ] && echo "$f"
  return 0
}
export -f run_solo
if [ -s "$WORK/casemap.txt" ]; then
  cut -f1 "$WORK/casemap.txt" \
    | xargs -P "$JOBS" -I{} bash -c 'run_solo "$@"' _ {} \
    | while IFS= read -r f; do grep -F "$(printf '%s\t' "$f")" "$WORK/casemap.txt" | cut -f2-; done \
    | LC_ALL=C sort -u > "$WORK/current_leaks.txt"
else
  : > "$WORK/current_leaks.txt"
fi
now=$(grep -c '' "$WORK/current_leaks.txt")

# A chunk that aborts but whose cases all pass solo is an unattributed leak (a
# cross-case interaction the two-phase scan can't localize) — fail loudly rather
# than pass silently.
naborting=$(grep -c '' "$WORK/aborting_chunks.txt")
if [ "$naborting" -gt 0 ] && [ "$now" -eq 0 ]; then
  echo "leak-abort-suite: FAIL — $naborting chunk(s) aborted but no case leaked solo (unattributed)." >&2
  cat "$WORK/aborting_chunks.txt" >&2
  exit 1
fi

# Strip comments/blanks from the allowlist to a bare sorted label set.
grep -v '^[[:space:]]*#' "$ALLOW" | grep -v '^[[:space:]]*$' \
  | LC_ALL=C sort -u > "$WORK/allow_sorted.txt"
allow=$(grep -c '' "$WORK/allow_sorted.txt")

if [ "$UPDATE" = 1 ]; then
  # Replace with a fresh header + the freshly-scanned labels; the per-class
  # "why" comments are re-added by hand afterwards.
  tmp="$WORK/allow_new.txt"
  {
    echo "# Allowlist for the suite-wide GAP5 gate. Regenerated by"
    echo "# \`just leak-abort-suite-update\` — re-add the per-class \"why\" comments"
    echo "# by hand (see git history)."
    cat "$WORK/current_leaks.txt"
  } > "$tmp"
  cp "$tmp" "$ALLOW"
  echo "leak-abort-suite: allowlist updated — $now leaking label(s) recorded"
  exit 0
fi

new_leaks=$(LC_ALL=C comm -23 "$WORK/current_leaks.txt" "$WORK/allow_sorted.txt")
fixed=$(LC_ALL=C comm -13 "$WORK/current_leaks.txt" "$WORK/allow_sorted.txt")

echo "leak-abort-suite: $cases cases, $now leaking label(s) (allowlist $allow)"
if [ -n "$fixed" ]; then
  echo
  echo "leak-abort-suite: $(printf '%s\n' "$fixed" | grep -c '') allowlisted leak(s) NO LONGER abort — shrink the allowlist:"
  printf '  fixed: %s\n' "$fixed"
  echo "  → remove them from tools/difftest/leak_abort_allow.txt"
fi
if [ -n "$new_leaks" ]; then
  echo
  echo "leak-abort-suite: FAIL — $(printf '%s\n' "$new_leaks" | grep -c '') NEW inflated-RC leak(s) not in the allowlist:" >&2
  printf '  NEW LEAK: %s\n' "$new_leaks" >&2
  echo "  Localize with: just leak-abort '<a .cul repro>'  (birth-site backtrace)" >&2
  exit 1
fi
echo "leak-abort-suite: no new leaks ✓"
exit 0
