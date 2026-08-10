#!/usr/bin/env bash
# GC_STRESS parity: every case, both VM lanes, allocations forced to collect.
set -u
BIN="${1:-./build-dev/culebra}"
BIN="$(realpath "$BIN" 2>/dev/null || echo "$BIN")"
cd "$(dirname "$0")"
fail=0
for f in *.cul; do
  a="$("$BIN" "$f" 2>&1)"; a_rc=$?
  for lane in --vm --vm-llvm; do
    b="$(CULEBRA_GC_STRESS=1 "$BIN" "$lane" "$f" 2>&1)"; b_rc=$?
    if [[ "$a" == "$b" && "$a_rc" == "$b_rc" ]]; then
      echo "OK   $f $lane (GC_STRESS)"
    else
      echo "FAIL $f $lane (interp rc=$a_rc, stress rc=$b_rc)"
      diff <(printf '%s\n' "$a") <(printf '%s\n' "$b") | sed 's/^/     /'
      fail=1
    fi
  done
done
exit $fail
