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

# A program blocked in IO.stdin().read() is interruptible by a single Ctrl+C,
# not just the force-killing second press (the read polls the flag while
# waiting).
cat > "$TMP/stdin.cul" <<'EOF'
defer { IO.eprint("DEFER\n") }
let s = IO.stdin().read()
IO.eprint("GOT {s.size()}\n")
EOF

# check_stdin <desc> -- <command...>
# Runs the command with a stdin that stays open but never delivers data (so it
# blocks in the read), then sends SIGINT. Expects an uncaught Interrupted: the
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

# A request blocked waiting on a server that accepts but never responds is
# interruptible by a single Ctrl+C, not just the force-killing second press (a
# watcher thread calls Client::stop() to shut the in-flight socket). Needs
# python3 for a hang-server; skipped otherwise.
HAVE_PY=0
command -v python3 >/dev/null 2>&1 && HAVE_PY=1

cat > "$TMP/hang_server.py" <<'EOF'
import socket, sys, time
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 0))
s.listen(1)
with open(sys.argv[1], "w") as f:
    f.write(str(s.getsockname()[1]))   # report the chosen port to the parent
c, _ = s.accept()                       # accept, then hang (never reply)
time.sleep(60)
EOF

cat > "$TMP/http.cul" <<'EOF'
defer { IO.eprint("DEFER\n") }
let port = IO.stdin().read().trim()
let resp = Http.get("http://127.0.0.1:{port}/", timeout: 60000)
IO.eprint("GOT {resp.status}\n")
EOF

# check_http <desc> -- <command...>
# Starts a server that accepts then hangs, runs the program (port fed on stdin)
# so it blocks in Http.get, then sends SIGINT. Expects an uncaught Interrupted:
# the defer runs and it exits 130 — without ever getting a response ("GOT").
check_http() {
  local desc="$1"; shift
  [ "$1" = "--" ] && shift
  if [ "$HAVE_PY" != 1 ]; then echo "skip [$desc] (no python3)"; return; fi
  local portf="$TMP/port"
  rm -f "$portf"
  python3 "$TMP/hang_server.py" "$portf" & local spid=$!
  local i; for i in $(seq 1 50); do [ -s "$portf" ] && break; sleep 0.1; done
  if [ ! -s "$portf" ]; then
    echo "FAIL [$desc]: hang-server never reported a port"; fail=1
    kill "$spid" 2>/dev/null; return
  fi
  echo "$(cat "$portf")" | "$@" > "$TMP/out" 2>&1 & local pid=$!
  sleep 1.5
  kill -INT "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  local code=$?
  kill "$spid" 2>/dev/null; rm -f "$portf"
  local out; out="$(tr '\n' '|' < "$TMP/out")"
  if [ "$code" = 130 ] && [ "$out" = 'DEFER|interrupted|' ]; then
    echo "ok [$desc]"
  else
    echo "FAIL [$desc]: exit=$code out='$out' (want exit=130 out='DEFER|interrupted|')"
    fail=1
  fi
}

# A program blocked in Proc.run is interruptible by a single Ctrl+C, not just
# the force-killing second press: run_command's poll loop wakes periodically to
# honor the flag, SIGKILLs the child, and raises the cooperative Interrupted.
# The child here IGNORES SIGINT, so the process-group signal alone won't stop it
# — only the active poll-loop kill does. This also pins the interp/JIT symmetry:
# before the fix the JIT had no post-call safepoint and ran on past Proc.run.
cat > "$TMP/proc.cul" <<'EOF'
defer { IO.eprint("DEFER\n") }
Proc.run(["sh", "-c", "trap '' INT; sleep 30"])
IO.eprint("GOT\n")
EOF

# check_proc <desc> -- <command...>
# Runs a program blocked in Proc.run on a SIGINT-ignoring child, sends one
# SIGINT, and expects an uncaught Interrupted: the defer runs and it exits 130
# without reaching the line after Proc.run ("GOT" never prints).
check_proc() {
  local desc="$1"; shift
  [ "$1" = "--" ] && shift
  "$@" > "$TMP/out" 2>&1 &
  local pid=$!
  sleep 1
  kill -INT "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  local code=$?
  local out; out="$(tr '\n' '|' < "$TMP/out")"
  if [ "$code" = 130 ] && [ "$out" = 'DEFER|interrupted|' ]; then
    echo "ok [$desc]"
  else
    echo "FAIL [$desc]: exit=$code out='$out' (want exit=130 out='DEFER|interrupted|')"
    fail=1
  fi
}

# Signal.notify (Go's signal.Notify): a registered channel receives the signal
# instead of the program throwing, so Ctrl+C wakes a blocked rx.recv() and the
# program shuts down on its own terms (exit 0). We assert the delivery + clean
# shutdown — the symmetric guarantee — not the received value's text (a separate
# pre-existing JIT bug renders deserialized values' display, but delivery works).
cat > "$TMP/signotify.cul" <<'EOF'
let (tx, rx) = Channel.new(1)
Signal.notify(tx)
tx.drop()                  # notify keeps its own sender; the channel stays open
IO.eprint("READY\n")
rx.recv()                  # blocks until Ctrl+C is delivered to the channel
IO.eprint("SHUTDOWN\n")
EOF

# check_signotify <desc> -- <command...>
# Runs the program, waits until it's armed ("READY"), sends one SIGINT, and
# expects a graceful exit 0 with SHUTDOWN printed (the signal reached recv()).
check_signotify() {
  local desc="$1"; shift
  [ "$1" = "--" ] && shift
  "$@" > "$TMP/out" 2>&1 & local pid=$!
  ( sleep 12; kill -9 "$pid" 2>/dev/null ) & local wd=$!
  local i; for i in $(seq 1 100); do grep -q READY "$TMP/out" 2>/dev/null && break; sleep 0.1; done
  kill -INT "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null; local code=$?
  kill "$wd" 2>/dev/null
  local out; out="$(tr '\n' '|' < "$TMP/out")"
  if [ "$code" = 0 ] && [ "$out" = 'READY|SHUTDOWN|' ]; then
    echo "ok [$desc]"
  else
    echo "FAIL [$desc]: exit=$code out='$out' (want exit=0 out='READY|SHUTDOWN|')"
    fail=1
  fi
}

# REPL: Ctrl+C interrupts the running eval and returns to the prompt instead of
# killing the session. The program (fed from a plain file) evals `mut i=0`, then
# blocks in the tight loop while `puts(42)` waits unread; a SIGINT interrupts the
# loop, the REPL reads `puts(42)`, prints it, hits EOF, and exits 0. We assert
# the survival ("42" runs after the interrupt) — robust across the two backends'
# differing value-echo formatting.
cat > "$TMP/repl.cul" <<'EOF'
mut i=0
while true { i=i+1 }
puts(42)
EOF

# check_repl <desc> <warmup> -- <command...>
check_repl() {
  local desc="$1" warm="$2"; shift 2
  [ "$1" = "--" ] && shift
  "$@" < "$TMP/repl.cul" > "$TMP/out" 2>&1 & local pid=$!
  ( sleep 12; kill -9 "$pid" 2>/dev/null ) & local wd=$!
  sleep "$warm"
  kill -INT "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null; local code=$?
  kill "$wd" 2>/dev/null
  local out; out="$(tr '\n' '|' < "$TMP/out")"
  if [ "$code" = 0 ] && printf '%s' "$out" | grep -q 42; then
    echo "ok [$desc]"
  else
    echo "FAIL [$desc]: exit=$code out='$out' (want exit=0, output containing 42)"
    fail=1
  fi
}

# --- interpreter ---
check "interp uncaught" 130 "$UNCAUGHT_OUT" -- "$CULEBRA" "$TMP/uncaught.cul"
check "interp caught"     0 "$CAUGHT_OUT"   -- "$CULEBRA" "$TMP/caught.cul"
check_stdin "interp stdin" -- "$CULEBRA" "$TMP/stdin.cul"
check_http  "interp http"  -- "$CULEBRA" "$TMP/http.cul"
check_proc  "interp proc"  -- "$CULEBRA" "$TMP/proc.cul"
check_signotify "interp signotify" -- "$CULEBRA" "$TMP/signotify.cul"
check_repl  "interp repl" 1.5 -- "$CULEBRA"

# --- JIT ---
check "jit uncaught" 130 "$UNCAUGHT_OUT" -- "$CULEBRA" --jit "$TMP/uncaught.cul"
check "jit caught"     0 "$CAUGHT_OUT"   -- "$CULEBRA" --jit "$TMP/caught.cul"
check_stdin "jit stdin" -- "$CULEBRA" --jit "$TMP/stdin.cul"
check_http  "jit http"  -- "$CULEBRA" --jit "$TMP/http.cul"
check_proc  "jit proc"  -- "$CULEBRA" --jit "$TMP/proc.cul"
check_signotify "jit signotify" -- "$CULEBRA" --jit "$TMP/signotify.cul"
# The JIT REPL is pre-existingly non-functional (it aborts/hangs at startup
# compiling its stdlib preamble), so its interrupt can't be exercised end-to-end
# here. The wiring is identical to the JIT script-mode loop cases above (same
# install_sigint_handler + global-flag safepoint), which DO run. See the Task.
echo "skip [jit repl] (JIT REPL pre-existingly broken at startup)"

# --- AOT (skip if this build can't produce binaries) ---
# (REPL has no AOT form; it is an interp/JIT driver only.)
if "$CULEBRA" build "$TMP/uncaught.cul" -o "$TMP/uncaught_aot" >/dev/null 2>&1 \
   && "$CULEBRA" build "$TMP/caught.cul" -o "$TMP/caught_aot" >/dev/null 2>&1 \
   && "$CULEBRA" build "$TMP/stdin.cul" -o "$TMP/stdin_aot" >/dev/null 2>&1 \
   && "$CULEBRA" build "$TMP/http.cul" -o "$TMP/http_aot" >/dev/null 2>&1 \
   && "$CULEBRA" build "$TMP/proc.cul" -o "$TMP/proc_aot" >/dev/null 2>&1 \
   && "$CULEBRA" build "$TMP/signotify.cul" -o "$TMP/signotify_aot" >/dev/null 2>&1; then
  check "aot uncaught" 130 "$UNCAUGHT_OUT" -- "$TMP/uncaught_aot"
  check "aot caught"     0 "$CAUGHT_OUT"   -- "$TMP/caught_aot"
  check_stdin "aot stdin" -- "$TMP/stdin_aot"
  check_http  "aot http"  -- "$TMP/http_aot"
  check_proc  "aot proc"  -- "$TMP/proc_aot"
  check_signotify "aot signotify" -- "$TMP/signotify_aot"
else
  echo "skip [aot] (culebra build unavailable)"
fi

if [ "$fail" = 0 ]; then echo "signal_test OK"; fi
exit $fail
