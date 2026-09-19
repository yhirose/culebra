#!/usr/bin/env bash
# Detect reference-count leaks in the JIT, one allocation pattern at a time.
#
# An object that a *correct* refcount would free, but that survives because a
# codegen path leaked its release, stays alive with a non-zero refcount. The
# collector's quiescent-point audit (CULEBRA_GC_LEAK_ABORT=1, gc.h
# audit_inflated_rc) classifies exactly that: an object the conservative scan
# finds unreachable whose refcount still exceeds the references the heap holds
# to it — a phantom +1 — and aborts naming its birth site. Each pattern runs
# once under that audit, with the collector otherwise off (CULEBRA_GC_NEVER=1)
# so no background collection reclaims the leaked garbage before the audit
# sees it. Zero false positives: a reference cycle the pattern builds on
# purpose has no phantom count and is not reported.
#
# (This used to compare live counts between a conservative and a
# refcount-seeded GC.stat(); now that GC.stat() itself seeds from the
# refcounts the two runs would agree, and the audit is the direct measurement.)
#
# Usage:
#   gc_leak_check.sh                      # run the built-in pattern battery
#   gc_leak_check.sh path/to/program.cul  # audit one program
#   CULEBRA=./build-gate/culebra gc_leak_check.sh  # pick the binary (default build-dev)
#
# Pass a NO-LTO binary (build-dev/ or build-gate/): the audit rides the
# conservative scan's completeness, and LTO's altered stack layout aliases
# leaked objects as live and under-reports.
#
# Exit status: 0 = no leak detected, 1 = at least one leaking pattern.

set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CULEBRA="${CULEBRA:-$ROOT/build-dev/culebra}"
PATTERNS="$HERE/gc_leak_patterns.cul"
N="${N:-50000}"

if [[ ! -x "$CULEBRA" ]]; then
  echo "error: culebra binary not found at $CULEBRA (set CULEBRA=...)" >&2
  exit 2
fi

check_one() { # args: <label> <file> <pattern-args...>; prints a row, exits 1 on a leak
  local label="$1"; shift
  local err verdict="ok"
  # detect_leaks=0 as in the other audit runners: with the collector off,
  # LSan's exit-time report would turn a clean pattern into a non-zero exit.
  err=$(env CULEBRA_GC_NEVER=1 CULEBRA_GC_LEAK_ABORT=1 ASAN_OPTIONS=detect_leaks=0 \
        "$CULEBRA" --jit "$@" 2>&1 >/dev/null)
  local st=$?
  if grep -q '\[gc-leak-abort\]' <<< "$err"; then
    verdict="LEAK"
  elif (( st != 0 )); then
    verdict="error(exit=$st)"
  fi
  printf "%-20s audit=%s\n" "$label" "$verdict"
  if [[ "$verdict" == "LEAK" ]]; then
    # The birth sites the audit printed, indented under the row.
    sed -n '/\[gc-leak-abort\]/,$p' <<< "$err" | sed 's/^/    /'
  fi
  [[ "$verdict" == "ok" ]]
}

rc=0
if [[ $# -ge 1 ]]; then
  # Audit a user-supplied program.
  check_one "$(basename "$1")" "$@" || rc=1
else
  echo "GC leak check — N=$N (binary: $CULEBRA)"
  echo "--------------------------------------------------------------------"
  # Each pattern is an independent culebra run reporting only its own
  # process's audit, so the battery fans out across patterns with no
  # cross-talk. Serially the patterns dominate the gate (every run compiles
  # the whole battery before its loop); parallel they collapse to the slowest
  # single pattern. Per-pattern output is buffered and replayed in list
  # order; a leak/error drops a marker file collected afterward.
  work="$(mktemp -d "${TMPDIR:-/tmp}/culebra-leak.XXXXXX")" || { echo "error: mktemp -d failed" >&2; exit 2; }
  trap 'rm -rf "$work"' EXIT
  list="$work/patterns"
  "$CULEBRA" --jit "$PATTERNS" list 2>/dev/null | grep -v '^[[:space:]]*$' > "$list"
  if [[ ! -s "$list" ]]; then
    echo "error: pattern battery listed no patterns ($PATTERNS via $CULEBRA)" >&2
    exit 2
  fi
  jobs="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 8)}"
  export -f check_one
  export CULEBRA PATTERNS N work
  xargs -P "$jobs" -I '{}' bash -c '
      row=$(check_one "{}" "$PATTERNS" "{}" "$N"); st=$?
      printf "%s\n" "$row" > "$work/{}.row"
      [[ $st -eq 0 ]] || touch "$work/{}.bad"
    ' < "$list"
  while read -r pat; do
    [[ -s "$work/$pat.row" ]] && cat "$work/$pat.row"
  done < "$list"
  shopt -s nullglob
  bad=("$work"/*.bad)
  (( ${#bad[@]} > 0 )) && rc=1
  echo "--------------------------------------------------------------------"
  if (( rc == 0 )); then echo "no RC leaks detected"; else echo "RC leak(s) detected — see LEAK rows above"; fi
fi
exit $rc
