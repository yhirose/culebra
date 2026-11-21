#!/usr/bin/env bash
# `for i in range(...)` walks a Long counter, as `for i in a..b` does.
#
# A call to the unshadowed built-in `range` with one or two positional
# arguments and an optional `step:` is compiled to ForPrep/ForLoop
# (vm.h, range_call_bounds), so the loop opens no iterator: no Range object,
# no protocol closures, no dispose. Missing it costs nothing a test can see
# but time — about 0.6 us each time such a loop starts, on both engines,
# which a short loop in a hot function pays on every call.
#
# The probe's shapes must leave no for-protocol call in the emitted IR, and
# the control (the same loop over a Range held in a variable) must leave
# some, so the gate cannot pass by measuring nothing.
set -euo pipefail
cd "$(dirname "$0")/../.."

# The binary under test: the caller's, or the one the inner loop just built.
BIN="${1:-build-dev/culebra}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/culebra-counted-range.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/call.cul" <<'CUL'
fn hot(n) {
  mut acc = 0
  for i in range(n) {
    acc += i
  }
  for i in range(1, n) {
    acc += i
  }
  for i in range(n, 0, step: -2) {
    acc += i
  }
  for _ in range(n, step: 3) {
    acc += 1
  }
  acc
}

println(hot(4))
CUL

cat > "$TMP/held.cul" <<'CUL'
fn hot(n) {
  mut acc = 0
  let r = range(n)
  for i in r {
    acc += i
  }
  acc
}

println(hot(4))
CUL

# A probe that emits no IR must fail here, not read as zero opens.
protocol_calls() {  # $1 = probe basename
  if ! "$BIN" --jit --emit-llvm "$TMP/$1.cul" > "$TMP/$1.ll" 2>"$TMP/$1.err"; then
    echo "counted-range FAIL: $1 could not emit IR" >&2
    sed 's/^/    /' "$TMP/$1.err" >&2
    exit 1
  fi
  grep -cE '(call|invoke) .*@culebra_runtime_shared_for_open' "$TMP/$1.ll" || true
}

call=$(protocol_calls call) || exit 1
held=$(protocol_calls held) || exit 1

if [ "$held" -eq 0 ]; then
  echo "counted-range FAIL: the control (a loop over a held Range) opens no" >&2
  echo "  iterator either, so this gate measures nothing. Rewrite the probe" >&2
  echo "  around a call that still opens the for protocol." >&2
  exit 1
fi

if [ "$call" -ne 0 ]; then
  echo "counted-range FAIL: \`for i in range(...)\` still opens the for" >&2
  echo "  protocol $call time(s); it should compile to a counted loop" >&2
  echo "  (vm.h compile_for, range_call_bounds)." >&2
  exit 1
fi

echo "counted-range OK (range calls 0, held Range $held protocol open(s))"
