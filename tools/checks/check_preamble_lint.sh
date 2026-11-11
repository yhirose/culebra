#!/usr/bin/env bash
# Preamble lint gate (src/preambles/*.cul).
# A preamble is compiled when the build bakes it, near the end of a `just dev`
# (4 to 5 minutes in), and a name that shadows an outer one (a local `held`
# beside a function `held`, which happened three times in a day) stops the
# build there. `culebra lint` finds the same ShadowError, and any other static
# error, from the source in a fraction of a second, so it runs here, in the
# check that takes 20 seconds, ahead of the build. Warnings are the style
# checks' and are left to `culebra lint`; only an error fails.
#
# Usage: tools/checks/check_preamble_lint.sh <culebra binary>
set -euo pipefail

bin="${1:?usage: check_preamble_lint.sh <culebra binary>}"
cd "$(dirname "$0")/../.."

# lint exits non-zero for warnings too, so read the errors from its output.
out=$("$bin" lint src/preambles 2>&1 || true)
errors=$(printf '%s\n' "$out" | grep ': error: ' || true)
if [[ -n "$errors" ]]; then
  printf '%s\n' "$errors" >&2
  echo "check_preamble_lint FAIL: a preamble does not compile (above); the build would stop baking it" >&2
  exit 1
fi
count=$(find src/preambles -name '*.cul' | wc -l | tr -d ' ')
echo "preamble-lint OK ($count preambles, no static error)"
