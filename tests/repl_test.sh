#!/usr/bin/env bash
# Regression test for what the REPL echoes (include/repl.h), and for the two
# tier-0 engines answering a session identically (include/vm_repl.h). Driving
# it needs a process with a piped stdin, so no .cul sweep can reach any of this.
# Usage: repl_test.sh <path-to-culebra>
set -u
CULEBRA="${1:?usage: repl_test.sh <culebra-binary>}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

# Never touch the developer's real history file.
export CULEBRA_HISTFILE="$TMP/history"

# Feed lines to the REPL and capture everything it wrote.
repl() { printf '%s\n' "$@" | "$CULEBRA" --tree 2>&1; }

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

# ---------------------------------------------------------------------------
# The VM executor is the other tier-0 engine (`culebra --vm` with no script).
# Every session below has to read the same on both, which is what makes the
# bytecode lane a REPL and not just a script runner: bindings that outlive the
# line they were made on, closures that see a name a later line declares, and
# the mutability of a name as something the session — not the line — carries.
# ---------------------------------------------------------------------------
parity() {
  local label=$1; shift
  local a b
  a=$(printf '%s\n' "$@" | "$CULEBRA" --tree 2>&1)
  b=$(printf '%s\n' "$@" | "$CULEBRA" --vm 2>&1)
  if [[ "$a" != "$b" ]]; then
    echo "FAIL vm parity [$label]"
    diff <(printf '%s\n' "$a") <(printf '%s\n' "$b") | sed 's/^/    /'
    fail=1
  fi
  # A wrong-but-equal answer would pass the diff above, so every session ends
  # on a line whose echo the caller can also assert.
  printf '%s' "$a"
}

out=$(parity echo "println('hello')" "nil" "1 + 2" "'str'" "[1, 2]" "{'k': 1}")
[[ "$out" == "hello"$'\n'"3"$'\n'"'str'"$'\n'"[1, 2]"$'\n'"{k: 1}" ]] ||
  { echo "FAIL vm echo: $out"; fail=1; }

out=$(parity bindings "let x = 1" "x + 1" "mut m = 5" "m = m + 1" "m")
[[ "$out" == "1"$'\n'"2"$'\n'"5"$'\n'"6"$'\n'"6" ]] ||
  { echo "FAIL vm bindings: $out"; fail=1; }

# A redeclaration lands in the same binding, so a closure made before it reads
# the new value — the interp's one persistent Environment, entry for entry.
out=$(parity redeclare "mut c = 0" "let bump = fn () {" "  c = c + 1" "}" \
                       "bump()" "mut c = 100" "bump()" "c")
[[ "$out" == *$'\n'"101" ]] || { echo "FAIL vm redeclare: $out"; fail=1; }

# ...and a closure can name a binding no line has made yet.
out=$(parity forward "let h = fn () { later }" "h()" "let later = 5" "h()")
[[ "$out" == *"NameError"*$'\n'"5"$'\n'"5" ]] ||
  { echo "FAIL vm forward ref: $out"; fail=1; }

# Mutability is the session's: a `let` over a `mut` takes it back, and a bare
# assignment to a name nothing holds declares it without one.
parity mutability "mut v = 1" "v = 2" "let v = 3" "v = 4" "w = 7" "w = 8" >/dev/null

# `fn` overloads accumulate across lines the way they do down a statement list,
# and a same-arity redeclaration replaces just that arm.
out=$(parity multifn "fn f() { 1 }" "fn f(a) { 2 }" "f()" "f(1)" \
                     "fn f() { 9 }" "f()" "f(1)")
[[ "$out" == "1"$'\n'"2"$'\n'"9"$'\n'"2" ]] || { echo "FAIL vm multifn: $out"; fail=1; }

# Classes, enums and the lazy stdlib namespaces, each first named on its own
# line — the VM session registers a namespace's builder once, when a line
# first mentions it.
parity classes "class P { new(a) { self.a = a } }" "let p = P.new(21)" "p.a * 2" \
               "enum E { A, B }" "E.A" >/dev/null
parity stdlib "Math.abs(-3)" "to_string(42)" "assert_eq(1, 1)" \
              "Time.now().class" "[1, 2].map(|v| v * 2)" >/dev/null

# An error leaves the session usable, and a failed declaration leaves the name
# undeclared rather than half-made.
parity errors "1 + 'a'" "undefined_name" "let z = 1" "z = 2" "z" \
              "let q = undefined_thing" "q = 5" "q" >/dev/null

# The echoed value's own lifetime: a resource bound to nothing drops right
# after the echo, not one line later.
parity drop "class R {" "  new(n) { self.n = n }" "  drop() { println('DROP') }" "}" \
            "R.new(9)" "println('after')" >/dev/null

# Out of the VM's slice is a report the session survives, not a crash.
out=$(printf '%s\n' "let t = Tensor.from([[1.0]])" "1 + 1" | "$CULEBRA" --vm 2>&1)
[[ "$out" == *$'\n'"2" ]] || { echo "FAIL vm slice recovery: $out"; fail=1; }

if [[ $fail -eq 0 ]]; then echo "repl_test OK"; exit 0; fi
echo "repl_test FAILED"; exit 1
