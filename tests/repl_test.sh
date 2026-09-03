#!/usr/bin/env bash
# Regression test for the REPL (include/cli/repl.h): what a session echoes and
# what state it carries across lines. Driving it needs a process with a piped
# stdin, so no .cul sweep can reach any of this. Each session asserts its own
# expected output — the cross-engine diff retired with the tree-walker's
# oracle duty (B7-c).
# Usage: repl_test.sh <path-to-culebra>
set -u
CULEBRA="${1:?usage: repl_test.sh <culebra-binary>}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

# Never touch the developer's real history file.
export CULEBRA_HISTFILE="$TMP/history"

# Feed lines to one REPL session and capture everything it wrote.
session() { printf '%s\n' "$@" | "$CULEBRA" --vm 2>&1; }

# What a line echoes: a nil result is suppressed (`println` writes its own
# line and evaluates to nil — echoing that doubled every line of output), a
# non-nil result prints in its display form.
out=$(session "println('hello')" "nil" "if false { 1 }" "1 + 2" "'str'" \
              "[1, 2]" "{'k': 1}"); rc=$?
[[ $rc -eq 0 && "$out" == "hello"$'\n'"3"$'\n'"'str'"$'\n'"[1, 2]"$'\n'"{k: 1}" ]] ||
  { echo "FAIL echo: rc=$rc out=$out"; fail=1; }

# Multi-line input accumulates until the openers close, then evaluates once —
# and the `fn` declaration itself stays quiet.
out=$(session "fn g(a) {" "  a + 1" "}" "g(1)")
[[ "$out" == "2" ]] || { echo "FAIL continuation: $out"; fail=1; }

# Bindings outlive the line they were made on.
out=$(session "let x = 1" "x + 1" "mut m = 5" "m = m + 1" "m")
[[ "$out" == "1"$'\n'"2"$'\n'"5"$'\n'"6"$'\n'"6" ]] ||
  { echo "FAIL bindings: $out"; fail=1; }

# A redeclaration lands in the same binding, so a closure made before it reads
# the new value — one persistent session environment, entry for entry.
out=$(session "mut c = 0" "let bump = fn () {" "  c = c + 1" "}" \
              "bump()" "mut c = 100" "bump()" "c")
[[ "$out" == *$'\n'"101" ]] || { echo "FAIL redeclare: $out"; fail=1; }

# ...and a closure can name a binding no line has made yet.
out=$(session "let h = fn () { later }" "h()" "let later = 5" "h()")
[[ "$out" == *"NameError"*$'\n'"5"$'\n'"5" ]] ||
  { echo "FAIL forward ref: $out"; fail=1; }

# A closure carries the cells for the session names it reads, so it means the
# same thing on a thread that has neither the session nor the Runtime those
# cells were minted in — an earlier line's binding, and a stdlib name whose
# session cell is still unbound (the worker resolves that one for itself).
out=$(session "let helper = fn (x) { x * 2 }" \
              "Parallel.map([1, 2], |x| helper(x))" \
              "Isolate.spawn(|| helper(5)).join()" \
              "Parallel.map([0 - 1, 0 - 2], |x| Math.abs(x))")
[[ "$out" == *$'\n'"[2, 4]"$'\n'"10"$'\n'"[1, 2]" ]] ||
  { echo "FAIL cross-thread session names: $out"; fail=1; }

# Mutability is the session's: a `let` over a `mut` takes it back, and a bare
# assignment to a name nothing holds declares it without one.
out=$(session "mut v = 1" "v = 2" "let v = 3" "v = 4" "w = 7" "w = 8")
[[ "$out" == "1"$'\n'"2"$'\n'"3"$'\n'*"ImmutableError"*$'\n'"7"$'\n'*"ImmutableError"* ]] ||
  { echo "FAIL mutability: $out"; fail=1; }

# `fn` overloads accumulate across lines the way they do down a statement list,
# and a same-arity redeclaration replaces just that arm.
out=$(session "fn f() { 1 }" "fn f(a) { 2 }" "f()" "f(1)" \
              "fn f() { 9 }" "f()" "f(1)")
[[ "$out" == "1"$'\n'"2"$'\n'"9"$'\n'"2" ]] || { echo "FAIL multifn: $out"; fail=1; }

# Classes, enums and the lazy stdlib namespaces, each first named on its own
# line — the session registers a namespace's builder once, when a line first
# mentions it.
out=$(session "class P { new(a) { self.a = a } }" "let p = P.new(21)" "p.a * 2" \
              "enum E { A, B }" "E.A")
[[ "$out" == *"42"*"E"* ]] || { echo "FAIL classes: $out"; fail=1; }
out=$(session "Math.abs(-3)" "to_string(42)" "assert_eq(1, 1)" \
              "Time.now().class" "[1, 2].map(|v| v * 2)")
[[ "$out" == "3"$'\n'"'42'"$'\n'"'Instant'"$'\n'"[2, 4]" ]] ||
  { echo "FAIL stdlib: $out"; fail=1; }

# An error leaves the session usable, and a failed declaration leaves the name
# undeclared rather than half-made.
out=$(session "1 + 'a'" "undefined_name" "let z = 1" "z = 2" "z" \
              "let q = undefined_thing" "q = 5" "q")
[[ "$out" == *"TypeError"*"NameError"*"1"*"ImmutableError"*"1"*"NameError"*"5"$'\n'"5" ]] ||
  { echo "FAIL errors: $out"; fail=1; }

# The echoed value's own lifetime: a resource bound to nothing drops right
# after the echo, not one line later.
out=$(session "class R {" "  new(n) { self.n = n }" "  drop() { println('DROP') }" "}" \
              "R.new(9)" "println('after')")
[[ "$out" == *"DROP"$'\n'"after" ]] || { echo "FAIL drop: $out"; fail=1; }

# A session name reached from two arms of the same function. The slot holding
# the session's cell is filled by an instruction the compiler emits where the
# name is first mentioned, and that is inside whichever arm mentioned it — so
# a call taking the other arm used to read an empty slot and segfault.
out=$(session "let helper = fn () { 7 }" \
      "let pick = fn (n) { if n == 2 { return helper() }; if n == 1 { return helper() + 1 }; 0 }" \
      "pick(1)" "pick(2)")
[[ "$out" == *$'\n'"8"$'\n'"7" ]] ||
  { echo "FAIL session branches: $out"; fail=1; }

# A bare write in a function is its own local unless the session declared the
# name (the second `z` used to be an ImmutableError on the session's).
out=$(session "let t1 = fn () { z = 5; z + 1 }" "let t2 = fn () { z = 6; z + 2 }" \
              "t1()" "t2()" "mut k = 1" "let bump = fn () { k = k + 1 }" "bump()" "k")
[[ "$out" == *$'\n'"6"$'\n'"8"$'\n'"1"$'\n'*$'\n'"2"$'\n'"2" ]] ||
  { echo "FAIL function-local bare decl: $out"; fail=1; }

# Out of the VM's slice is a report the session survives, not a crash.
out=$(session "let t = Tensor.from([[1.0]])" "1 + 1")
[[ "$out" == *$'\n'"2" ]] || { echo "FAIL slice recovery: $out"; fail=1; }

if [[ $fail -eq 0 ]]; then echo "repl_test OK"; exit 0; fi
echo "repl_test FAILED"; exit 1
