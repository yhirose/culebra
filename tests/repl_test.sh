#!/usr/bin/env bash
# Regression test for what the REPL echoes (include/repl.h). Driving it needs a
# process with a piped stdin, so no .cul sweep can reach any of this.
# Usage: repl_test.sh <path-to-culebra>
set -u
CULEBRA="${1:?usage: repl_test.sh <culebra-binary>}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

# Never touch the developer's real history file.
export CULEBRA_HISTFILE="$TMP/history"

# Feed lines to the REPL and capture everything it wrote.
repl() { printf '%s\n' "$@" | "$CULEBRA" 2>&1; }

# A nil result is not echoed. `println` writes its own line and evaluates to
# nil; echoing that doubled every line of output ("hello" then "nil").
out=$(repl "println('hello')")
[[ "$out" == "hello" ]] || { echo "FAIL println echo: $out"; fail=1; }

# Same rule for the other common nil producers, and for a literal nil. A `fn`
# declaration is covered by the session-state case below, which asserts the
# whole output is just the call's result.
for src in "nil" "if false { 1 }"; do
  out=$(repl "$src")
  [[ -z "$out" ]] || { echo "FAIL nil echo [$src]: $out"; fail=1; }
done

# A non-nil result is still echoed — suppression is about nil, not about
# turning the echo off. EOF on stdin also ends the session cleanly.
out=$(repl "1 + 2"); rc=$?
[[ $rc -eq 0 && "$out" == "3" ]] || { echo "FAIL value echo: rc=$rc out=$out"; fail=1; }

# Session state survives, and the declaration itself stays quiet.
out=$(repl "fn f() { 2 }" "f()")
[[ "$out" == "2" ]] || { echo "FAIL session state: $out"; fail=1; }

# Multi-line input accumulates until the openers close, then evaluates once.
out=$(repl "fn g(a) {" "  a + 1" "}" "g(1)")
[[ "$out" == "2" ]] || { echo "FAIL continuation: $out"; fail=1; }

# An error is reported and the session keeps going.
out=$(repl "undefined_name" "1 + 1")
[[ "$out" == *"NameError"* ]] || { echo "FAIL error report: $out"; fail=1; }
[[ "$out" == *$'\n'"2" ]] || { echo "FAIL error recovery: $out"; fail=1; }

if [[ $fail -eq 0 ]]; then echo "repl_test OK"; exit 0; fi
echo "repl_test FAILED"; exit 1
