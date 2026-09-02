#!/usr/bin/env bash
# examples/mini-js/mini_js.cul against Node: every samples/*.js is an ES6
# program both run, so Node is the compiler's oracle. Each sample prints
# through examples/mini-js/fmt.js (prepended on both sides), and its stdout
# must be identical under the executor and --jit.
#
# As in mini_culebra_test.sh, all samples go through one mini invocation per
# lane (the compiler's own parse and, under --jit, its compile are paid once
# per lane), with a marker line before each program; the oracle side is the
# per-sample Node runs joined by the same markers. A sample that exits
# non-zero under Node is a test bug, not a difference.
#
# Usage: mini_js_test.sh <path-to-culebra>
set -u

CULEBRA="${1:?usage: mini_js_test.sh <culebra>}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MINI="$ROOT/examples/mini-js/mini_js.cul"
FMT="$ROOT/examples/mini-js/fmt.js"
SAMPLES="$ROOT/examples/mini-js/samples"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if ! command -v node >/dev/null 2>&1; then
  echo "SKIP: node is not installed (the oracle)"
  exit 0
fi

: >"$TMP/want"
for path in "$SAMPLES"/*.js; do
  cat "$FMT" "$path" >"$TMP/in.js"
  {
    echo "-----8<----- $(basename "$path")"
    node "$TMP/in.js" 2>/dev/null
    rc=$?
    if [ "$rc" != 0 ]; then echo "node exited $rc"; fi
  } >>"$TMP/want"
done

fail=0
for flag in --vm --jit; do
  "$CULEBRA" "$flag" "$MINI" --lib "$FMT" "$SAMPLES"/*.js >"$TMP/got$flag" 2>"$TMP/err$flag"
  if ! diff -u "$TMP/want" "$TMP/got$flag" >"$TMP/diff$flag"; then
    echo "FAIL [$flag]: mini_js differs from node"
    head -40 "$TMP/diff$flag"
    head -5 "$TMP/err$flag"
    fail=1
  else
    echo "ok [$flag] ($(ls "$SAMPLES"/*.js | wc -l | tr -d ' ') samples)"
  fi
done

exit $fail
