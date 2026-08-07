#!/usr/bin/env bash
# Detect reference-count leaks in the JIT by differential collection.
#
# An object that a *correct* refcount would free, but that survives because a
# codegen path leaked its release, stays alive with a non-zero refcount. We
# surface that by collecting two ways and comparing the live-object count:
#
#   conservative (default) : marks from real reachability (stack + globals).
#                            Frees everything truly unreachable, regardless of
#                            refcount — so leaked garbage is reclaimed and the
#                            live count stays flat.
#   CULEBRA_GC_REFS=1       : seeds collection purely from reference counts.
#                            Trusts the refcount, so leaked garbage (refcount
#                            stuck > 0) is retained — the live count balloons.
#
# A pattern whose gc_refs live count far exceeds its conservative count has an
# RC leak in the operation it exercises.
#
# Usage:
#   gc_leak_check.sh                      # run the built-in pattern battery
#   gc_leak_check.sh path/to/program.cul  # audit one program (must end by
#                                         # printing `... live=<N>` via GC.stat)
#   CULEBRA=./build/culebra gc_leak_check.sh   # pick the binary (default build-dev)
#
# Exit status: 0 = no leak detected, 1 = at least one leaking pattern.

set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CULEBRA="${CULEBRA:-$ROOT/build-dev/culebra}"
PATTERNS="$HERE/gc_leak_patterns.cul"
N="${N:-50000}"
# Ratio above which gc_refs/conservative counts a leak. Real garbage keeps both
# counts tiny and flat; a leak makes the gc_refs count grow with the loop, so
# the ratio is enormous (>100x) — 4x is a safe, noise-proof threshold.
THRESHOLD="${THRESHOLD:-4}"

if [[ ! -x "$CULEBRA" ]]; then
  echo "error: culebra binary not found at $CULEBRA (set CULEBRA=...)" >&2
  exit 2
fi

live_of() { # args: <env...> -- <file> <pattern-args...>; echoes the live count
  local out
  out=$("$@" 2>/dev/null | sed -n 's/.* live=\([0-9][0-9]*\).*/\1/p' | tail -1)
  echo "${out:-?}"
}

check_one() { # args: <label> <file> <pattern-args...>
  local label="$1"; shift
  local cons refs
  cons=$(live_of "$CULEBRA" --jit "$@")
  refs=$(live_of env CULEBRA_GC_REFS=1 CULEBRA_GC_MULT=1 "$CULEBRA" --jit "$@")
  local verdict="ok"
  if [[ "$cons" =~ ^[0-9]+$ && "$refs" =~ ^[0-9]+$ ]]; then
    if (( cons == 0 )); then cons=1; fi
    if (( refs > cons * THRESHOLD && refs - cons > 100 )); then verdict="LEAK"; fi
  else
    verdict="error"
  fi
  printf "%-20s conservative=%-8s gc_refs=%-8s  %s\n" "$label" "$cons" "$refs" "$verdict"
  [[ "$verdict" == "ok" ]]
}

rc=0
if [[ $# -ge 1 ]]; then
  # Audit a user-supplied program.
  check_one "$(basename "$1")" "$@" || rc=1
else
  echo "GC leak check — N=$N, threshold=${THRESHOLD}x (binary: $CULEBRA)"
  echo "--------------------------------------------------------------------"
  # Each pattern is an independent pair of culebra runs (conservative vs
  # gc_refs), and each run reports only its own process's live count — so the
  # battery fans out across patterns with no cross-talk. Serially the ~39
  # patterns dominate the gate (~5 s each); parallel they collapse to the
  # slowest single pattern. Per-pattern output is buffered and replayed in list
  # order; a leak/error drops a marker file collected afterward.
  work="$(mktemp -d)" || { echo "error: mktemp -d failed" >&2; exit 2; }
  trap 'rm -rf "$work"' EXIT
  list="$work/patterns"
  "$CULEBRA" --jit "$PATTERNS" list 2>/dev/null | grep -v '^[[:space:]]*$' > "$list"
  if [[ ! -s "$list" ]]; then
    echo "error: pattern battery listed no patterns ($PATTERNS via $CULEBRA)" >&2
    exit 2
  fi
  jobs="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 8)}"
  export -f check_one live_of
  export CULEBRA PATTERNS N THRESHOLD work
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
