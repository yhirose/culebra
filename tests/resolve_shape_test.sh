#!/usr/bin/env bash
# What the compiler RESOLVED, read off the emitted bytecode.
#
# These are the facts no behavioural test can see: a resolution that stops
# happening produces the same output through the dynamic arm it falls back
# to, so the whole suite stays green while the optimization is gone. The
# regression that prompted this file did exactly that — a revocation trigger
# widened to cover the constructor grant, which made a class declaration
# revoke the grant it had just made, and every `C.new(...)` in the language
# quietly went back to a dynamic call with nothing to show for it.
#
# Usage: resolve_shape_test.sh <culebra-binary>
set -uo pipefail

BIN="${1:-}"
if [[ -z "$BIN" ]]; then
  echo "usage: resolve_shape_test.sh <culebra-binary>" >&2
  exit 2
fi

fail=0
tmp="${TMPDIR:-/tmp}/resolve_shape_$$.cul"
trap 'rm -f "$tmp"' EXIT

# check <name> <source> <grep-ERE> <expected-count>
check() {
  local name="$1" src="$2" pat="$3" want="$4" got
  printf '%s\n' "$src" >"$tmp"
  got=$("$BIN" --vm-dump "$tmp" 2>&1 | grep -cE -e "$pat")
  if [[ "$got" != "$want" ]]; then
    echo "FAIL [$name]: '$pat' matched $got, expected $want" >&2
    fail=1
  fi
}

# A class declaration's own store must not revoke the constructor grant it
# just made: `C.new(...)` after the declaration resolves to one chunk.
check "ctor resolves after its declaration" \
'class Ok {
  x: Float
  new(x: Float) { self.x = x }
}
Ok.new(1.0).x' \
'CallM.*-> chunk' 1

# A member constructing its own class resolves too, behind the guard its
# receiver-read name owes.
check "own-class construction is guarded" \
'class Ok {
  x: Float
  new(x: Float) { self.x = x }
  twin() { Ok.new(self.x) }
}
Ok.new(1.0).twin().x' \
'-> chunk .*\(guarded\)' 1

# An overload set makes the constructor a dispatcher rather than a chunk, so
# nothing resolves — the negative half, which is what keeps the positive
# checks from passing for the wrong reason.
check "overloaded ctor resolves nothing" \
'class Over {
  new() { self.a = 0 }
  new(a) { self.a = a }
}
Over.new().a' \
'CallM.*-> chunk' 0

# A flat `@value` chain compiles to slots: no instance is built at all, and
# the run shows up as named field slots.
check "flat @value chain unboxes" \
'@value
class V2 {
  x: Float
  y: Float
  new(x: Float, y: Float) { self.x = x; self.y = y }
  len() { Math.sqrt(self.x * self.x + self.y * self.y) }
}
V2.new(3.0, 4.0).len()' \
'\(V2\.x\)' 1

# ...and so does `V2(...)`, which calls the same constructor.
check "a call-form construction unboxes" \
'@value
class V2 {
  x: Float
  y: Float
  new(x: Float, y: Float) { self.x = x; self.y = y }
  len() { Math.sqrt(self.x * self.x + self.y * self.y) }
}
V2(3.0, 4.0).len()' \
'\(V2\.x\)' 1

# A method whose body only reads its parameter as a run takes its argument
# as one, and a chain on a local ending in a run is an operand like the
# local itself: the loop builds no instance and calls nothing.
spin='@value
class V2 {
  x: Float
  y: Float
  new(x: Float, y: Float) { self.x = x; self.y = y }
  __add__(o) { V2.new(self.x + o.x, self.y + o.y) }
  __mul__(k) { V2.new(self.x * k, self.y * k) }
  rot(o) { V2.new(self.x * o.x - self.y * o.y, self.x * o.y + self.y * o.x) }
}
fn spin(n) {
  mut p = V2.new(1.0, 0.0)
  let b = V2.new(0.6, 0.8)
  for _ in 0..n { p += p.rot(b) * 0.5 }
  p.x
}'
check "a method argument stays a run" "$spin" \
  ' (ValueBox|CallM|Call) .*; 1[0-4]:' 0

# The negative half: a parameter the body reads bare gets an instance.
check "a parameter read bare reboxes its argument" \
'@value
class V2 {
  x: Float
  new(x: Float) { self.x = x }
  keep(o) { take(o).x + self.x }
}
fn take(v) { v }
fn f() {
  let p = V2.new(1.0)
  let b = V2.new(2.0)
  p.keep(b)
}
println(f())' \
' ValueBox ' 1

# A `let` built from another local of the same list, and an operand that is
# itself an operator over another run, stay runs; `/`, `%` and `@` splice.
# The step on lines 11-14 builds no instance and dispatches no operator.
steps='@value
class V2 {
  x: Float
  y: Float
  new(@x, @y) {}
  __add__(o) { V2(self.x + o.x, self.y + o.y) }
  __div__(k) { V2(self.x / k, self.y / k) }
  __mod__(k) { V2(self.x % k, self.y % k) }
  __matmul__(o) { V2(self.x * o.x, self.y * o.y) }
}
fn ok() {
  let w = V2(1.0, 2.0)
  let w2 = w + V2(w.y, 0.5)
  (w2 / 4.0 @ w % 3.0 + w2).x
}'
check "a step built from runs of one list stays unboxed" "$steps" \
  ' (ValueBox|Call|CallM|Add|Div|Mod|MatMul) .*; 1[1-4]:' 0

# A write whose RHS starts at another local of the same list.
check "a write headed by another local stays unboxed" \
'@value
class V2 {
  x: Float
  new(@x) {}
  __add__(o) { V2(self.x + o.x) }
  __mul__(k) { V2(self.x * k) }
}
fn f(n) {
  let w = V2(0.5)
  mut v = V2(0.0)
  for _ in 0..n { v = w + v * 0.5 }
  v.x
}' \
' (ValueBox|Call|CallM|Add|Mul) .*; 1[0-3]:' 0

# An operator whose left operand is itself an operator splices too.
check "an operator headed by an operator stays unboxed" \
'@value
class V2 {
  x: Float
  new(@x) {}
  __add__(o) { V2(self.x + o.x) }
  __mul__(k) { V2(self.x * k) }
}
fn f(n) {
  mut p = V2(1.0)
  let v = V2(0.5)
  for _ in 0..n { p = p * 0.5 + v * 0.1 }
  p.x
}' \
' (ValueBox|Call|CallM|Add|Mul) .*; 1[0-3]:' 0

# A number on the left (`k * v`) splices `v`'s reflected dunder: a
# literal, a `let` of one, arithmetic of those, a parameter annotated as one.
reflected='@value
class V2 {
  x: Float
  new(@x) {}
  __add__(o) { V2(self.x + o.x) }
  __mul__(k) { V2(self.x * k) }
  __rmul__(k) { V2(self.x * k) }
}
fn f(n, h: Float) {
  let dt = 0.5
  let half = dt * 0.5
  mut p = V2(1.0)
  let v = V2(0.5)
  for _ in 0..n { p = p + dt * v + 2 * v + half * v + h * v }
  p.x
}'
check "a number times a run splices the reflected dunder" "$reflected" \
  ' (ValueBox|Call|CallM|Add|Mul) .*; 1[3-6]:' 0
# ...and a number the compiler cannot count on (a `mut`) keeps the
# operator, dispatched at run time.
check "a mut number times a run stays boxed" \
'@value
class V2 {
  x: Float
  new(@x) {}
  __add__(o) { V2(self.x + o.x) }
  __rmul__(k) { V2(self.x * k) }
}
fn f(n) {
  mut k = 0.5
  mut p = V2(1.0)
  let v = V2(0.5)
  for _ in 0..n { p = p + k * v }
  p.x
}' \
' Mul .*; 12:' 1

# A `let` in a loop body that consumes a run of the enclosing list is
# decided with it, so neither is boxed.
check "a nested let consuming an outer run stays unboxed" \
'@value
class V2 {
  x: Float
  new(@x) {}
  __add__(o) { V2(self.x + o.x) }
  __mul__(k) { V2(self.x * k) }
}
fn f(n) {
  mut p = V2(1.0)
  let v = V2(0.5)
  for _ in 0..n {
    let d = v * 0.5
    p = p + d
  }
  p.x
}' \
' (ValueBox|Call|CallM|Add|Mul) .*; 1[1-4]:' 0

# A nested `let` of a name the round also binds outside that body does not
# join the round, and costs the outer binding nothing.
check "a nested let of a shared name leaves the outer run unboxed" \
'@value
class V2 {
  x: Float
  new(@x) {}
  __add__(o) { V2(self.x + o.x) }
  __mul__(k) { V2(self.x * k) }
}
fn f(n) {
  mut p = V2(1.0)
  let v = V2(0.5)
  for _ in 0..n {
    let d = v * 0.5
    IO.inspect(d)
  }
  let d = V2(3.0)
  for _ in 0..n { p = p + d }
  p.x
}' \
' (ValueBox|Call|CallM) .*; 1[5-6]:' 0

# A typed field whose annotation names a class with typed fields reads
# those fields typed down the chain, as through a `let`.
check "a typed field's class follows a chain" \
'class P {
  x: Float
  new(@x) {}
}
class S {
  pos: P
  new(@pos) {}
}
fn f(s: S) { s.pos.x }' \
'PropVal .* 7 +; 9:.*\.x' 1

# ...and a class the layout rules refuse keeps the boxed form. Without this
# the check above would pass just as well if everything unboxed.
check "nested-@value field stays boxed" \
'@value
class In {
  a: Long
  new(a: Long) { self.a = a }
}
@value
class Out {
  inner: In
  k: Long
  new(inner: In, k: Long) { self.inner = inner; self.k = k }
}
Out.new(In.new(1), 2).k' \
'\(Out\.inner\)' 0

# A write that ends an arm of an `if` written as a statement is read by
# nobody: the run stays a run, and the loop around it builds no instance.
check "an unread arm's write is not reboxed" \
'@value
class V2 {
  new(@x: Float, @y: Float) {}
  __add__(o) { V2.new(self.x + o.x, self.y + o.y) }
}
fn f(n) {
  mut a = V2.new(0.0, 0.0)
  for i in 0..n {
    if i > 0 {
      a = a + V2.new(1.0, 2.0)
    } else {
      a += V2.new(0.5, 0.5)
    }
  }
  a.x
}
println(f(3))' \
' ValueBox ' 0

# ...and the same write ending the body itself is the body's value, which
# its caller reads as an instance.
check "a body's last write is reboxed" \
'@value
class V2 {
  new(@x: Float, @y: Float) {}
  __add__(o) { V2.new(self.x + o.x, self.y + o.y) }
}
fn f(k) {
  mut a = V2.new(0.0, 0.0)
  if k {
    a = a + V2.new(1.0, 2.0)
  }
}
println(f(true))' \
' ValueBox ' 1

# A stdlib module's class has no binding to read its class object from, and
# its run is reboxed at a boundary all the same.
check "a stdlib class's run is reboxed at a boundary" \
'fn take(o) { o }
fn f() {
  mut v = Vector2.new(1.0, 2.0)
  v = v + Vector2.new(0.5, 0.5)
  take(v)
}
println(f())' \
' ValueBox ' 1

# A destructuring statement with a literal on both sides builds no Tuple: the
# elements go to the targets directly, the last statement of an `if` arm
# included.
check "literal destructure builds no Tuple" \
'mut a = 1
mut b = 2
(a, b) = (b, a)
let (c, d) = (a, b)
[a, b] = [d, c]
if a < b {
  (a, b) = (b, a)
}
println(a + b + c + d)' \
' (TupleNew|ArrayNew|SeqChk) ' 0

# ...and the forms that still need it: a `_` target, whose element the Tuple
# releases in its own order, and a statement whose value is read (a
# function's last one is its result).
check "a sink or a read value keeps the Tuple" \
'let (x, _) = (1, 2)
fn f() {
  mut p = 1
  mut q = 2
  (p, q) = (q, p)
}
println([x, f()])' \
' TupleNew ' 2

if [[ $fail -eq 0 ]]; then echo "resolve_shape_test OK"; exit 0; fi
exit 1
