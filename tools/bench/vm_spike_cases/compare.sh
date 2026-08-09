#!/usr/bin/env bash
# Diff the spike lanes against the interpreter on every case file.
# Usage: compare.sh <culebra-binary> [lane-flag]   (default lane: --vm-spike)
set -u
BIN="${1:-./build-dev/culebra}"
LANE="${2:---vm-spike}"
cd "$(dirname "$0")"
fail=0
for f in *.cul; do
  a="$("$BIN" "$f" 2>&1)"; a_rc=$?
  b="$("$BIN" "$LANE" "$f" 2>&1)"; b_rc=$?
  if [[ "$a" == "$b" && "$a_rc" == "$b_rc" ]]; then
    echo "OK   $f"
  else
    echo "FAIL $f (interp rc=$a_rc, $LANE rc=$b_rc)"
    diff <(printf '%s\n' "$a") <(printf '%s\n' "$b") | sed 's/^/     /'
    fail=1
  fi
done
exit $fail
