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

# A program blocked in IO.read_all is interruptible by a single Ctrl+C, not
# just the force-killing second press (read_all polls the flag while waiting).
cat > "$TMP/stdin.cul" <<'EOF'
defer { IO.eprint("DEFER\n") }
let s = IO.read_all()
IO.eprint("GOT {s.size()}\n")
EOF

# check_stdin <desc> -- <command...>
# Runs the command with a stdin that stays open but never delivers data (so it
# blocks in read_all), then sends SIGINT. Expects an uncaught Interrupted: the
# defer runs and it exits 130 — *without* reading any input ("GOT" never prints).
check_stdin() {
  local desc="$1"; shift
  [ "$1" = "--" ] && shift
  local fifo="$TMP/sfifo"
  rm -f "$fifo"; mkfifo "$fifo"
  sleep 30 > "$fifo" &              # writer holds the fifo open, sends nothing
  local wpid=$!
  "$@" < "$fifo" > "$TMP/out" 2>&1 &
  local pid=$!
  sleep 1.5
  kill -INT "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  local code=$?
  kill "$wpid" 2>/dev/null; rm -f "$fifo"
  local out; out="$(tr '\n' '|' < "$TMP/out")"
  if [ "$code" = 130 ] && [ "$out" = 'DEFER|interrupted|' ]; then
    echo "ok [$desc]"
  else
    echo "FAIL [$desc]: exit=$code out='$out' (want exit=130 out='DEFER|interrupted|')"
    fail=1
  fi
}

# --- interpreter ---
check "interp uncaught" 130 "$UNCAUGHT_OUT" -- "$CULEBRA" "$TMP/uncaught.cul"
check "interp caught"     0 "$CAUGHT_OUT"   -- "$CULEBRA" "$TMP/caught.cul"
check_stdin "interp stdin" -- "$CULEBRA" "$TMP/stdin.cul"

# --- JIT ---
check "jit uncaught" 130 "$UNCAUGHT_OUT" -- "$CULEBRA" --jit "$TMP/uncaught.cul"
check "jit caught"     0 "$CAUGHT_OUT"   -- "$CULEBRA" --jit "$TMP/caught.cul"
check_stdin "jit stdin" -- "$CULEBRA" --jit "$TMP/stdin.cul"

# --- AOT (skip if this build can't produce binaries) ---
if "$CULEBRA" build "$TMP/uncaught.cul" -o "$TMP/uncaught_aot" >/dev/null 2>&1 \
   && "$CULEBRA" build "$TMP/caught.cul" -o "$TMP/caught_aot" >/dev/null 2>&1 \
   && "$CULEBRA" build "$TMP/stdin.cul" -o "$TMP/stdin_aot" >/dev/null 2>&1; then
  check "aot uncaught" 130 "$UNCAUGHT_OUT" -- "$TMP/uncaught_aot"
  check "aot caught"     0 "$CAUGHT_OUT"   -- "$TMP/caught_aot"
  check_stdin "aot stdin" -- "$TMP/stdin_aot"
else
  echo "skip [aot] (culebra build unavailable)"
fi

if [ "$fail" = 0 ]; then echo "signal_test OK"; fi
exit $fail
