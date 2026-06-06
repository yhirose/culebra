#!/usr/bin/env bash
# Cooperative Ctrl+C (SIGINT) behaviour, asserted identically across interp,
# JIT, and AOT. Signal delivery can't be expressed in a plain .cul test (the
# harness compares deterministic output), so this drives the binary, sends a
# SIGINT mid-run, and checks the exit code + output.
#
# Contract:
#   * a tight loop is interruptible (even a single-statement body);
#   * the throw is a catchable `Interrupted` — `defer` runs, and a `catch`
#     resumes execution (one-shot, Python KeyboardInterrupt model);
#   * an uncaught interrupt runs top-level defers and exits 130.
#
# Usage: signal_test.sh <path-to-culebra>
set -u

CULEBRA="${1:?usage: signal_test.sh <culebra>}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/uncaught.cul" <<'EOF'
defer { IO.eprint("DEFER\n") }
mut i = 0
while true { i = i + 1 }
EOF

cat > "$TMP/caught.cul" <<'EOF'
fn work() {
  defer { IO.eprint("DEFER\n") }
  mut i = 0
  while true { i = i + 1 }
}
try { work() } catch e { IO.eprint("CAUGHT {e.kind} {e.line}:{e.col}\n") }
IO.eprint("CONT\n")
EOF

fail=0

# check <desc> <want-exit> <want-output> -- <command...>
# Runs the command, sends SIGINT after a beat, and compares exit + output
# (newlines folded to '|' for a one-line compare).
check() {
  local desc="$1" want_exit="$2" want_out="$3"; shift 3
  [ "$1" = "--" ] && shift
  "$@" > "$TMP/out" 2>&1 &
  local pid=$!
  sleep 1
  kill -INT "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  local code=$?
  local out; out="$(tr '\n' '|' < "$TMP/out")"
  if [ "$code" != "$want_exit" ] || [ "$out" != "$want_out" ]; then
    echo "FAIL [$desc]: exit=$code out='$out'"
    echo "            want exit=$want_exit out='$want_out'"
    fail=1
  else
    echo "ok [$desc]"
  fi
}

UNCAUGHT_OUT='DEFER|interrupted|'
CAUGHT_OUT='DEFER|CAUGHT Interrupted 0:0|CONT|'

# --- interpreter ---
check "interp uncaught" 130 "$UNCAUGHT_OUT" -- "$CULEBRA" "$TMP/uncaught.cul"
check "interp caught"     0 "$CAUGHT_OUT"   -- "$CULEBRA" "$TMP/caught.cul"

# --- JIT ---
check "jit uncaught" 130 "$UNCAUGHT_OUT" -- "$CULEBRA" --jit "$TMP/uncaught.cul"
check "jit caught"     0 "$CAUGHT_OUT"   -- "$CULEBRA" --jit "$TMP/caught.cul"

# --- AOT (skip if this build can't produce binaries) ---
if "$CULEBRA" build "$TMP/uncaught.cul" -o "$TMP/uncaught_aot" >/dev/null 2>&1 \
   && "$CULEBRA" build "$TMP/caught.cul" -o "$TMP/caught_aot" >/dev/null 2>&1; then
  check "aot uncaught" 130 "$UNCAUGHT_OUT" -- "$TMP/uncaught_aot"
  check "aot caught"     0 "$CAUGHT_OUT"   -- "$TMP/caught_aot"
else
  echo "skip [aot] (culebra build unavailable)"
fi

if [ "$fail" = 0 ]; then echo "signal_test OK"; fi
exit $fail
