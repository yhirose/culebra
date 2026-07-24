#!/usr/bin/env bash
# GAP5 smoke test (docs/gc_model.md §5): verify the loud inflated-RC leak
# detector. With CULEBRA_GC_LEAK_ABORT=1 the JIT audits for inflated-RC leaks
# at the teardown quiescent safepoint and aborts with the leaked object's
# birth site. This checks the detector itself — that it FIRES (with a birth
# site) on a real acyclic leak and STAYS QUIET on clean code and benign cycles.
#
# It is not the corpus regression gate (that is leak.sh, growth-based). It is a
# fixed 3-case guard so a change that breaks the detector — or its zero-false-
# positive property — fails the build.
#
# Pass a NO-LTO binary (build-dev/ or build-gate/). The audit rides the
# conservative scan's completeness (best-effort); LTO's altered stack layout
# lets the teardown scan alias leaked objects as live and under-report, so the
# fires-on-leak case would spuriously "pass as clean" under an LTO build.
#
# Usage: tools/difftest/leak_abort.sh [culebra-binary]
set -uo pipefail

# Default to the no-LTO dev build: the audit/growth oracles under-report on
# an LTO binary (stale-stack aliasing), so a stale ./build/ default has caused
# false-green runs twice. Pass build-gate/culebra explicitly from the gate.
CULEBRA="${1:-./build-dev/culebra}"
[ -x "$CULEBRA" ] || { echo "error: $CULEBRA not found/executable — pass a no-LTO culebra binary" >&2; exit 1; }
WORK="${LEAKFUZZ_WORK:-build/leakfuzz}"
mkdir -p "$WORK"
fail=0

# Run $1 (a .cul path) under the audit; echo the exit code and captured stderr.
run_audit() {
  CULEBRA_GC_NEVER=1 CULEBRA_GC_LEAK_ABORT=1 ASAN_OPTIONS=detect_leaks=0 \
    "$CULEBRA" --jit "$1" >/dev/null 2>"$WORK/abort.err"
  echo $?
}

# --- Case 1: a real acyclic leak must abort with a birth site ---------------
# The fires-on-leak fixture used to piggyback on a real open product leak and
# was swapped every time one closed (seven times: the lazy
# `.iter().map(f).collect()` exception path, the generator for-in CPS
# iterator, the `**dict`-splat kwarg, the eager borrowed-callback throw, the
# dropped-Shared property read, the `self`-named parameter shadow, then the
# heap-operand comparison chain). With those classes closed, the fixture is
# now self-contained: CULEBRA_GC_TEST_LEAK=1 (a debug-only knob in
# run_modules) mints eight deliberately orphaned +1 arrays at startup, which
# survive to the teardown audit under run_audit's CULEBRA_GC_NEVER=1. Eight is
# above the conservative-scan aliasing knee, so the abort is deterministic.
cat > "$WORK/abort_leak.cul" <<'EOF'
IO.inspect("fixture ran")
EOF
code=$(CULEBRA_GC_TEST_LEAK=1 run_audit "$WORK/abort_leak.cul")
if [ "$code" != 134 ]; then
  echo "leak-abort: FAIL — acyclic leak did not abort (exit=$code, want 134)" >&2
  fail=1
elif ! grep -q '\[gc-leak-abort\]' "$WORK/abort.err" \
   || ! grep -q 'born at:' "$WORK/abort.err"; then
  echo "leak-abort: FAIL — abort lacked the birth-site report" >&2
  cat "$WORK/abort.err" >&2
  fail=1
else
  echo "leak-abort: acyclic-leak abort + birth site OK"
fi

# --- Case 2: clean code must NOT abort (zero false positive) -----------------
cat > "$WORK/abort_clean.cul" <<'EOF'
fn work(n) { mut s = 0; for i in range(n) { s = s + i }; s }
mut t = 0
for j in range(200) { t = t + work(50) }
IO.inspect("clean total=" + t.to_string())
EOF
code=$(run_audit "$WORK/abort_clean.cul")
if [ "$code" != 0 ]; then
  echo "leak-abort: FAIL — clean program aborted (exit=$code, want 0)" >&2
  cat "$WORK/abort.err" >&2
  fail=1
else
  echo "leak-abort: clean-program no-false-positive OK"
fi

# --- Case 3: a benign reference cycle must NOT abort ------------------------
# A self-cycle leaks under RC (the backstop's job), but its residue is
# transitively-held (refcount balances the internal edge), not inflated — so
# the detector must classify it as a keep, not a leak.
cat > "$WORK/abort_cycle.cul" <<'EOF'
class Node { new() { self.link = nil } }
fn mkcycle() { let a = Node.new(); a.link = a }
mut i = 0
while i < 200 { mkcycle(); i = i + 1 }
IO.inspect("cycle done")
EOF
code=$(run_audit "$WORK/abort_cycle.cul")
if [ "$code" != 0 ]; then
  echo "leak-abort: FAIL — benign cycle aborted (exit=$code, want 0)" >&2
  cat "$WORK/abort.err" >&2
  fail=1
else
  echo "leak-abort: benign-cycle no-false-positive OK"
fi

# --- Case 4: in-flight +1 across a throwing sub-expression must NOT leak ----
# Pins the automatic unwind-temp window (docs/jit_ownership.md §4.8): a heap
# operand held while a LATER operand/argument/key throws used to strand on the
# unwind edge in every one of these shapes (binop lhs, call argument, index
# receiver, `==` lhs, method-call receiver, bitwise lhs). The window spills it
# around each may-throw call and the scope-chain cleanup pads release it.
cat > "$WORK/abort_unwind_temp.cul" <<'EOF'
fn boom() { throw "x" }
fn mk() { [1, 2, 3] }
fn g(a, b) { a }
fn h(o) { o }
fn shapes() {
  try { mk() + boom() } catch e {}
  try { g(mk(), boom()) } catch e {}
  try { mk()[boom()] } catch e {}
  try { if mk() == boom() { 1 } } catch e {}
  try { h(mk()).f(boom()) } catch e {}
  try { mk() ^ boom() } catch e {}
}
mut i = 0
while i < 50 { shapes(); i = i + 1 }
IO.inspect("unwind-temp done")
EOF
code=$(run_audit "$WORK/abort_unwind_temp.cul")
if [ "$code" != 0 ]; then
  echo "leak-abort: FAIL — unwind-temp window shapes leaked (exit=$code, want 0)" >&2
  cat "$WORK/abort.err" >&2
  fail=1
else
  echo "leak-abort: unwind-temp-window no-leak OK"
fi

# --- Case 5: comparison-chain and UFCS-kwargs ownership must stay closed ----
# Pins the two block-crossing ownership fixes: a 3+ comparison chain's heap
# operands live in owned region-scope slots (released on fall-through, the
# short-circuit edge, and throw — with the comparison helper sole owner of its
# own throw edges), and a UFCS-with-kwargs receiver rides a single +1 into the
# callee, which owns it on every exit (the old per-arm retain + merge release
# stranded the outer +1 whenever the callee threw).
cat > "$WORK/abort_chain_ufcs.cul" <<'EOF'
class V { new(x) { self.x = x }  __lt__(o) { self.x < o.x } }
class T { new(x) { self.x = x }  __lt__(o) { throw "lt!" } }
fn boom() { throw "x" }
fn validate(data, strict) { throw "invalid" }
fn shapes() {
  let ok = V.new(1) < V.new(2) < V.new(3)
  let sc = V.new(3) < V.new(1) < V.new(9)
  try { V.new(1) < V.new(2) < boom() } catch e {}
  try { T.new(1) < T.new(2) < T.new(3) } catch e {}
  try { V.new(1) < [1, 2] < V.new(3) } catch e {}
  try { [1, 2, 3].validate(strict: true) } catch e {}
}
mut i = 0
while i < 50 { shapes(); i = i + 1 }
IO.inspect("chain-ufcs done")
EOF
code=$(run_audit "$WORK/abort_chain_ufcs.cul")
if [ "$code" != 0 ]; then
  echo "leak-abort: FAIL — chain/UFCS-kwargs ownership leaked (exit=$code, want 0)" >&2
  cat "$WORK/abort.err" >&2
  fail=1
else
  echo "leak-abort: chain-and-ufcs-kwargs no-leak OK"
fi

# --- Case 6: block-pinned raws (§4.9) — argument pairs and compound keys ----
# Pins the raw-across-BB ENFORCE's Phase-2 strand fixes: a heap +1 held
# while a LATER argument's evaluation throws (slice bounds, obj.get /
# get_or_put keys, stdlib-extension argument pairs), and a compound-assign
# key crossing the object_get_any KeyError edge. Each used to strand because
# the first value had already left its Owned before the second compiled.
cat > "$WORK/abort_blockpin.cul" <<'EOF'
fn boom() { throw "x" }
fn mk() { [1, 2, 3] }
fn shapes() {
  let d = {a: 1}
  try { mk().slice([1], boom()) } catch e {}
  try { d.get(mk(), boom()) } catch e {}
  try { d.get_or_put(mk(), boom()) } catch e {}
  mut o = {a: 1}
  try { o[mk()] += 1 } catch e {}
  try { Math.pow(mk(), boom()) } catch e {}
  try { Math.min(mk(), boom(), 1) } catch e {}
  // Inlined-lambda reduce with a HEAP seed: the seed +1 crosses the
  // receiver-dispatch arms (merely compiling this pins §4.9 — the shape
  // used to abort codegen; the error arm releases it via hof_owned) and
  // the normal path absorbs it as the accumulator.
  try { (1).reduce(mk(), |a, b| a + b) } catch e {}
  let _r = [1, 2].reduce(mk(), |a, b| a)
  // Inline-HOF body scopes ride the ihof cleanup pad: a mid-iteration
  // body throw releases the param slots' heap +1s (elem, and reduce's
  // accumulator) — 3/iter for the map/for_each/filter trio and 1/iter
  // for reduce before the pad was wired.
  try { [1, 2].reduce(mk(), |a, b| a + to_long("x")) } catch e {}
  try { [[1], [2]].map(|x| to_long("x")) } catch e {}
  try { [[1], [2]].for_each(|x| to_long("x")) } catch e {}
  try { [[1], [2]].filter(|x| to_long("x")) } catch e {}
  // The kwarg-resolver's mid-merge splat TypeError must release the kw
  // slab +1s and splat operands (the throw fires before the spawn).
  try { Proc.run(["true"], env: {A: "1"}, **5) } catch e {}
}
mut i = 0
while i < 50 { shapes(); i = i + 1 }
IO.inspect("blockpin done")
EOF
code=$(run_audit "$WORK/abort_blockpin.cul")
if [ "$code" != 0 ]; then
  echo "leak-abort: FAIL — block-pinned raw shapes leaked (exit=$code, want 0)" >&2
  cat "$WORK/abort.err" >&2
  fail=1
else
  echo "leak-abort: block-pin no-leak OK"
fi

[ "$fail" = 0 ] && echo "leak-abort OK" || { echo "leak-abort FAILED" >&2; exit 1; }
