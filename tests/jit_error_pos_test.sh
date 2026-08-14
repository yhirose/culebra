#!/usr/bin/env bash
# Regression test for error position symmetry between the interp and the
# JIT — mostly compile-time throws, plus runtime checks the JIT emits from
# compile-time position state (e.g. the for-in counted-range fast path).
# The interp stamps the deepest eval()'s AST location onto any CulebraError
# thrown with line==0; the JIT's compile() wrapper mirrors this for
# compile-time throws, so a compile_* site that omits ast.line/column still
# yields the same `... at L:C.` as the interp. This guards against the
# recurring "JIT forgot the position" divergence. Usage: <path-to-culebra>
set -u
CULEBRA="${1:?usage: jit_error_pos_test.sh <culebra-binary>}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

# Each case must print byte-identical diagnostics (kind + message +
# position) on both backends.
check_same() {
  local name="$1" prog="$2"
  printf '%s\n' "$prog" > "$TMP/t.cul"
  local out_i out_j
  out_i=$("$CULEBRA" "$TMP/t.cul" 2>&1)
  out_j=$("$CULEBRA" --jit "$TMP/t.cul" 2>&1)
  if [[ "$out_i" != "$out_j" ]]; then
    echo "FAIL [$name]: interp/jit diverge"
    echo "  interp: $out_i"
    echo "  jit:    $out_j"
    fail=1
  elif [[ "$out_i" != *" at "*":"*"."* ]]; then
    echo "FAIL [$name]: no position in diagnostic: $out_i"
    fail=1
  fi
}

# Like check_same but without the position requirement — for diagnostics that
# carry no `at L:C.` (e.g. an uncaught top-level `throw`).
check_eq() {
  local name="$1" prog="$2"
  printf '%s\n' "$prog" > "$TMP/t.cul"
  local out_i out_j
  out_i=$("$CULEBRA" "$TMP/t.cul" 2>&1)
  out_j=$("$CULEBRA" --jit "$TMP/t.cul" 2>&1)
  if [[ "$out_i" != "$out_j" ]]; then
    echo "FAIL [$name]: interp/jit diverge"
    echo "  interp: $out_i"
    echo "  jit:    $out_j"
    fail=1
  fi
}

# compound-declare and `??=` throw in the JIT without an explicit position;
# the compile() wrapper must backfill the ASSIGNMENT node's line/col.
check_same "compound declare"   'let x += 1'
check_same "??= non-simple"     'o.a ??= 1'
# These pass an explicit (often more specific) position; symmetry must hold.
check_same "non-default param"  'fn f(a = 1, b) { a }'
check_same "*args not last"     'fn f(*xs, y) { y }'
# break/continue outside loop is hoisted to the shared lint pass, but the
# interp/jit must still agree on the SyntaxError + position.
check_same "break outside loop" 'break'
check_same "continue in fn"     'fn f() { continue }'

# A yield outside a `fn name(...)` declaration (class method, object
# property fn, fn expression, top level) is never generator-transformed;
# it used to run silently with backend-dependent results (interp '' vs
# JIT last yield value). Now a shared parse-time SyntaxError.
check_same "yield top level"      'yield 1'
check_same "yield in class method" 'class B { m() { yield 1 } }
B().m()'
check_same "yield in object prop" 'let o = {g: fn () { yield 7 }}
o.g()'
check_same "yield in fn expr"     'let g = fn () { yield 9 }
g()'

# `self` in a generator's body used to silently resolve to the synthesized
# state object (never the enclosing receiver). Now a shared parse-time
# SyntaxError pointing at the self token. Labels (keys, kwargs, `x.self`) stay
# legal — see test_generator_self.cul.
check_same "self in generator"        'fn g() { yield self }
g()'
check_same "self.prop in generator"   'fn g() { yield self.name }
g()'
check_same "self write in generator"  'fn g() { self.x = 1
yield 1 }
g()'
check_same "let self in generator"    'fn g() { let self = 1
yield self }
g()'
# The rule reaches nested functions: a closure defined in the body reads the
# state object through the body method's own `self`, so it is refused where it
# is written. A function that is an object property is exempt — there `self` is
# the dynamic receiver (test_generator_self.cul asserts that side).
check_same "self in nested fn"        'fn g() { yield (fn () { self })() }
g()'
# An effect fn body is the same shape — a named declaration whose lowering
# leaves no receiver — and refuses identically. A handle body is NOT: there the
# enclosing method's receiver survives (test_effects.cul asserts it).
check_same "self in effect fn"        'effect fn ask()
effect fn e() { let s = self
let x = perform ask()
s }
handle { e() } with ask(resume) { resume(1) }'

# UFCS calls to the unary global builtins (to_string/hash/type_of/to_long/
# to_float). The JIT used to fall through to a property-get TypeError on a
# wrong-arity call (and lacked hash/to_float entirely); now it raises the
# same ArityError as the interp. `to_string` is special — a String/StringView
# value-method (0-arg) vs the arity-1 global elsewhere — so cover both.
check_same "to_string extra (Long)"   '(1).to_string(9)'
check_same "to_string extra (String)" '"x".to_string(9)'
check_same "to_string extra (Array)"  '[1].to_string(9)'
check_same "hash extra (Long)"        '(1).hash(9)'
check_same "hash extra (String)"      '"x".hash(9)'
check_same "type_of extra"            '(5).type_of(9)'
check_same "to_long extra"            '"5".to_long(9)'
check_same "to_float extra"           '(5).to_float(9)'

# A bad UFCS conversion attributes its type/parse error to the call's arg
# list (matching interp's eval_ufcs_call, which invokes with args_ast's
# position) — not the receiver. Both kind+message and position must agree.
check_same "to_long bad type"         'true.to_long()'
check_same "to_long bad type (Array)" '[1].to_long()'
check_same "to_float bad type"        'true.to_float()'
check_same "to_long bad string"       '"abc".to_long()'

# A `**` type error carries the operator's position — compile_power once
# called num_pow without line/col args, so the JIT printed garbage there.
check_same "pow type error"           'let x = 1 ** "a"'
check_same "pow zero neg exponent"    'let x = 0 ** -1'

# Compound assignment on a builtin global reads the (immutable) root-env
# binding: the step's TypeError surfaces (the JIT once said NameError),
# and `??=` keeps the never-nil builtin without error.
check_same "compound on builtin"      'to_string += 1'
check_eq   "coalesce on builtin"      '(println ??= 1)("kept")'

# An uncaught top-level `throw` prints `uncaught: <value>`. A thrown string
# prints raw (it's the message), matching the interp's str_display — the JIT
# once quoted it (`uncaught: 'boom'`). Non-strings already agreed.
check_eq "uncaught throw string"      'throw "boom"'
check_eq "uncaught throw int"         'throw 42'
check_eq "uncaught throw object"      'throw {code: 1, msg: "x"}'
check_eq "uncaught throw array"       'throw [1, "a"]'

# get_or_put's unhashable-key TypeError is raised from inside the runtime
# store, positionless — the JIT once left it at whatever an unrelated call
# (a lazy `init` thunk's own body) last published, or at 0:0 with no thunk
# at all. Both forms (eager and lazy init) must anchor at the receiver.
check_same "get_or_put unhashable (eager init)" 'mut d = {}
d.get_or_put([1, 2], 42)'
check_same "get_or_put unhashable (lazy init)"  'mut d = {}
d.get_or_put([1, 2], fn () { 99 })'

# A non-Function passed to an iterator/array higher-order method reports
# "parameter '<name>' expects Function" at the ARGUMENT's position, with the
# method's actual param name (map/for_each/... → 'f', filter/find/any/all/
# take_while → 'p'). The JIT once hardcoded 'f' and pointed at 1:1.
check_same "non-fn to eager map"      '[1, 2, 3].map(99)'
check_same "non-fn to eager filter"   '[1, 2, 3].filter(42)'
check_same "non-fn to lazy map"       '[1, 2, 3].iter().map({a: 1}).collect()'
check_same "non-fn to lazy filter"    '[1, 2, 3].iter().filter(42).collect()'
check_same "non-fn to lazy find"      '[1, 2, 3].iter().find(42).collect()'
check_same "non-fn to lazy take_while" '[1, 2, 3].iter().take_while(42).collect()'
check_same "non-fn to sort_by"        '[1, 2, 3].sort_by(42)'
check_same "non-fn to reduce"         '[1, 2, 3].reduce(0, 42)'

# RUNTIME errors from value-neutral lib helpers (tensor.h) are thrown without
# a position. The interp backfills it at its eval boundary; the JIT now mirrors
# this by publishing the op position (emit_set_op_pos) before the fallible call
# and backfilling at the runtime exception boundaries. Without it these printed
# location-less on the JIT/AOT while the interp carried `at L:C`.
check_same "tensor reshape mismatch" 'Tensor.zeros([6]).reshape([4])'
check_same "tensor reshape neg dim"  'Tensor.zeros([6]).reshape([-2])'
check_same "tensor sum bad axis"     'Tensor.zeros([2, 3]).sum(9)'
check_same "tensor slice oob"        'Tensor.zeros([3]).slice(5, 9)'
check_same "tensor reshape nested"   'let x = 1 + Tensor.zeros([6]).reshape([4]).sum()'

# A named function definition inside a generator body has no CPS lowering (the
# JIT didn't bind it → NameError, while the interp ran it — a divergence). The
# generator transform now rejects it uniformly before eval, like yield-in-try.
# Anonymous fn / lambda VALUES inside a generator still work (not rejected).
check_same "fn-def in generator"        'fn g() { fn h(x) { x * 3 }; yield h(2) }
g().collect()'
check_same "nested generator-def"       'fn g() { fn inner() { yield 1 }; for x in inner() { yield x } }
g().collect()'
check_same "fn-def in if in generator"  'fn g() { if true { fn h() { 1 } }; yield 1 }
g().collect()'

# Reading an unknown member of a builtin namespace raises AttributeError at the
# access site (naming the member), instead of silently yielding nil and failing
# later at the call. Interp (eval_property) and JIT (object_get_ic) must agree
# on kind + message + position — the position is the receiver expression, not
# the member token. `keys`/`has` (dict builtins) still dispatch; only genuinely
# absent members raise. Also covers a builtin method name that is NOT a dict
# builtin (`push`) and the bare-value read form.
check_same "ns missing member call"   'IO.read_all()'
check_same "ns missing member bare"   'let x = IO.read_all'
check_same "ns missing (FS)"          'FS.typo("x")'
check_same "ns array-method name"     'IO.push(1)'
check_same "ns via bound value"       'let x = IO
x.read_all()'

# Same for the culebra-source namespaces (src/preambles/*.cul). These are built
# by a different path on each backend — the interp's lazy binder vs the JIT's
# builder thunk — so the position agreement is worth pinning separately. One
# module (the cheapest) covers it: naming another only re-splices a second
# preamble into the JIT entry module.
check_same "lazy ns missing call"     'Time.time()'
check_same "lazy ns missing bare"     'let x = Time.typo'
check_same "lazy ns via bound value"  'let x = Time
x.typo'

# The well-known property contract (drop/iter/has_next/next must be a 0-arg
# Function) throws from four places. Three carried a position already; the
# method-template check inside build_class_meta printed location-less under
# the JIT because nothing published the op position before the call, while
# the interp stamped the CLASS_DECL node.
check_same "wk contract proto method"  'class P { new() { self.x = 1 }
  next(n) { n } }'
check_same "wk contract overload set"  'class P { drop() { nil }
  drop(a) { a } }'
check_same "wk contract ctor slot"     'class P { new() { self.drop = 42 } }
P.new()'
check_same "wk contract trait default" 'trait P { tag() -> Long
  next(x) { x } }'

# for-in's counted-range fast path type-checks the endpoints and step inline
# (value_to_long) instead of building a Range object. These are runtime
# errors; the JIT used to report them at the `for` keyword because compile()'s
# PosGuard had already restored the position, while the interp (and the JIT's
# own expression-context compile_range) report at the range expression.
check_same "for range float step"     'for x in 0..5 by 2.5 { }'
check_same "for range float lo"       'for x in 0.0..5 { }'
check_same "for range float hi"       'for x in 0..5.0 { }'
check_same "for range var float step" 'let s = 2.5
for x in 0..5 by s { }'
check_same "for range nested"         'fn f() { for x in 0..5 by 2.5 { } }
f()'

# The `.iter()` method on a range routes through the same runtime builder;
# the JIT used to hand it position 0:0, printing a location-less ValueError.
check_same "range iter zero step"     'for i in (0..5 by 0).iter() { }'
check_same "range iter unbounded"     'let x = (0..).iter()'

# A native handle method is entered with no line/col (the thunk ABI carries
# none), so its entry point backfills the published call site — one case per
# wrapped entry point. Every case fails offline (refused / unbound / bad path).
# Skipped when the binary was built without Http.
printf 'Http\n' > "$TMP/probe.cul"
if "$CULEBRA" "$TMP/probe.cul" >/dev/null 2>&1; then
  check_same "client transport"      'let c = Http.client("http://127.0.0.1:1")
c.get("/")'
  # `into` is checked by a helper shared with the Http.* adapters, which throw
  # positionless on purpose — only the entry-point backfill positions this one.
  check_same "client into type"      'let c = Http.client("http://127.0.0.1:1")
c.get("/", into: 5)'
  check_same "client with body"      'let c = Http.client("http://127.0.0.1:1")
c.post("/", "x")'
  check_same "client request"        'let c = Http.client("http://127.0.0.1:1")
c.request("GET", "/")'
  check_same "server serve unbound"  'let s = Http.server()
s.serve()'
  check_same "server bind refused"   'let s = Http.server()
s.bind(-1)'
  check_same "server listen refused" 'let s = Http.server()
s.listen(-1)'
  check_same "server static bad dir" 'let s = Http.server()
s.static("/x", "/no/such/dir/here")'
fi

# A UFCS call reports at its ARGUMENTS node, as the interp always has. The JIT
# was using the chain root its enclosing walk installed, which names the
# receiver instead — on the positional branch and on the kwargs one.
check_same "ufcs dispatch no args"    'fn g(a, b) { a }
let x = 1
x.g()'
check_same "ufcs dispatch extra args" 'fn g(a, b) { a }
let x = 1
x.g(1, 2)'
check_same "ufcs dispatch typed"      'fn g(a: String) { a }
let x = 1
x.g()'
# The kwargs UFCS branch reports through a different call than the positional
# one, and was still naming the receiver after the latter was fixed.
check_same "ufcs kwarg bad type"      'fn g(a, k: Long = 1) { a }
let x = 1
x.g(k: "bad")'
check_same "ufcs kwarg unknown"       'fn g(a, k: Long = 1) { a }
let x = 1
x.g(zz: 1)'

# A wrapped C++ class (wrap.h) reaches its methods through that same ABI, so
# the exception its body raises — a RuntimeError with no location — is
# positioned by the thunk. One case per thunk shape.
check_same "wrap method body"      'let c = __Foreign.Counter.new(10)
c.divide(0)'
check_same "wrap borrowed body"    'let b = __Foreign.Box.new(5)
b.slot(1)'

# A File handle method is entered through a thunk ABI with no line/col, so it
# hands the published call site to the same helper the interp calls with its
# own — one case per method that can fail. lines()/chunks() report where the
# ITERATOR was built (the position the interp's closure captured), not at the
# loop pulling it, so those two cases put the two on different lines.
printf 'a\nb\n' > "$TMP/in.txt"
check_same "file read closed"   "let f = File.open('$TMP/in.txt')
f.close()
f.read()"
check_same "file write rdonly"  "let f = File.open('$TMP/in.txt')
f.write('x')"
check_same "file flush closed"  "let f = File.open('$TMP/in.txt')
f.close()
f.flush()"
check_same "file seek whence"   "let f = File.open('$TMP/in.txt')
f.seek(0, 'bogus')"
check_same "file tell closed"   "let f = File.open('$TMP/in.txt')
f.close()
f.tell()"
check_same "file lines closed"  "let f = File.open('$TMP/in.txt')
let it = f.lines()
f.close()
for l in it { l }"
check_same "file chunks closed" "let f = File.open('$TMP/in.txt')
let it = f.chunks(2)
f.close()
for c in it { c }"

# A spread inside a sized array literal is a parse-time SyntaxError anchored
# at the spread element on both backends.
check_same "sized array spread" 'let a = [9, ...[8]](3)'
# The set literal's unhashable throw is positionless in the runtime; the JIT
# must publish the literal's position (the interp's eval boundary anchor).
check_same "set literal unhashable" 'let s = {[1], 2}'
# Same gap on the non-IDENTIFIER-key path of an object literal: the sidecar
# hash on insert throws positionless, and the JIT must publish the object
# literal's position to match.
check_same "object literal unhashable key" 'let o = {([1], 2): "x"}'

# A binding inside an or-pattern is a parse-time SyntaxError anchored at the
# binding name, in every pattern position (match arm, destructure, for
# binding, parameter) and at any nesting depth.
check_same "or-bind match arm"  'match 1 { a | _ => a, _ => 0 }'
check_same "or-bind ctor arm"   'enum R { Ok(T), Err(E) }
match R.Ok(1) { Ok(x) | Err(x) => x, _ => 0 }'
check_same "or-bind destructure" 'let [5 | a] = [7]'
check_same "or-bind nested"     'let {k: [_, b | 2]} = {k: [1, 2]}'
check_same "or-bind typed"      'match 1 { x: Long | _ => 0, _ => 1 }'
check_same "or-bind for"        'for [a | _] in [[1]] { a }'
check_same "or-bind param"      'fn f([a | _]) { a }'
# TYPE_ANNOTATION closes at the first non-Type alternative, so this parses as
# `((n: Long | Float) | 42)` — an or-pattern binding, not a wider Union.
check_same "or-bind type-then-literal" "match 1 { n: Long | Float | 42 => n, _ => 0 }"

# `static new` is a parse-time SyntaxError anchored at the name: the two
# backends used to sort it into the statics and then disagree about which
# binding of the class object's `new` survived.
check_same "static new" 'class Z {
  static new() { 1 }
}'
check_same "static new with ctor" 'class Z {
  new() { self.a = 1 }
  static new(x) { x }
}'

# A destructure's two throws both report the statement, not the leaf that
# failed: the shape mismatch and a `let`-less leaf's ImmutableError.
check_same "destructure mismatch"  'let [a, b] = [1]'
check_same "destructure immutable" 'let c = 0
[c] = [5]'

# A property write's receiver is Object only: Array/Tensor share to_object's
# wider read gate, but their inherited property sidecar is not a write
# surface. Every DOT write form rejects with the read path's wording.
check_same "prop write array recv"    'let a = [1, 2]
a.k = 9'
check_same "prop write tensor recv"   'let t = Tensor.from([1.0])
t.k = 5'
check_same "prop compound array recv" 'let a = [1]
a.k += 1'
check_same "prop ??= array recv"      'let a = [1]
a.k ??= 7'
check_same "prop write scalar recv"   '(5).a = 1'

# A Shared.new view rejects every write form as ImmutableError anchored at
# the statement (interp throws positionless); the compound pre-check must
# not shadow it with "missing property", and the `??=` receiver-kind
# rejects must not anchor at the property/subscript token.
check_same "compound shared view prop" 'let s = Shared.new({a: 1})
s.a += 1'
check_same "??= shared view prop"      'let s = Shared.new({a: 1})
s.a ??= 2'
check_same "??= shared view index"     "let s = Shared.new({a: 1})
s['a'] ??= 2"
check_same "??= packed field"          '@packable
class P {
  x: Float32 = 0.0
}
let buf = SharedBuffer.new(1, P)
buf[0].x ??= 2.0'
# The interp rejects a namespace unknown member at the DOT node in every
# write form, `??=` included (its plain read reports the chain head).
check_same "??= ns unknown member"     'Math.zzz ??= 1'

# A positionless error escaping a UFCS call backfills at the postfix
# chain's head (interp's eval() boundary), NOT the call site — the
# boundary channel set_call_boundary publishes. Explicit errors (arity,
# to_long's conversion read of the call-site channel) stay at the
# argument list.
check_same "ufcs native boundary"      'let h = hash
[1].h()'
check_same "ufcs stdlib boundary"      '[1].hash()'
check_same "ufcs stdlib arity"         '(3).println(4)'
check_same "ufcs to_long call site"    "'ab'.to_long()"
# The display walker's too-deep ValueError is positionless; both the
# direct println and its UFCS spelling publish the chain position.
check_same "println too-deep direct"   'mut v = [0]
mut i = 0
while i < 6000 { v = [v]; i += 1 }
println(v)'
check_same "println too-deep ufcs"     'mut v = [0]
mut i = 0
while i < 6000 { v = [v]; i += 1 }
v.println()'

# `==` and the ordering operators reach a user `eq` / `cmp` from inside the
# comparison helper, so the invocation has no codegen call site of its own.
# An explicit error from that method's binder must report the operator's
# position, not wherever the last real call happened to be.
check_same "== overload dispatch"      'class P {
  new(x) { self.x = x }
  eq() { true }
  eq(a, b) { true }
}
let a = P.new(1)
let b = P.new(2)
println(a == b)'
check_same "< overload dispatch"       'class P {
  new(x) { self.x = x }
  cmp() { 0 }
  cmp(a, b) { 0 }
}
let a = P.new(1)
let b = P.new(2)
println(a < b)'
check_same "== typed eq param"         'class P {
  new(x) { self.x = x }
  eq(o: Long) { true }
}
let a = P.new(1)
println(a == P.new(1))'
# Derived eq/cmp declare `other`; calling one bare is a missing-required
# ArityError at the call, and an unorderable field pair is the same
# `cannot compare` TypeError the `<` operator raises.
check_same "derived eq bare"           '@derive(Eq)
class P {
  new(x) { self.x = x }
}
P.new(1).eq()'
check_same "derived cmp unorderable"   '@derive(Comparable)
class P {
  new(x) { self.x = x }
}
println(P.new([1]) < P.new([2]))'
# An unknown @derive trait is reported by the static pass, so it fires before
# anything runs — the same instant on every backend, and independent of
# whether the declaration is ever reached.
check_same "derive unknown trait"      'println("before")
@derive(Eq, Nope)
class P {
  new(x) { self.x = x }
}'
check_same "derive unknown in fn"      'println("before")
fn mk() {
  @derive(Nope)
  class P {
    new(x) { self.x = x }
  }
  P
}
println("declared")'

# A declared return type is checked against the value the call produced, so
# the violation reports at the CALL, not at the declaration — including the
# implicit-return form, where the JIT has no `return` node to stamp.
check_same "return type tail"          'fn f() -> String {
  5
}
f()'
check_same "return type explicit"      'fn f() -> String {
  return 5
}
f()'
check_same "return type lambda"        'let g = fn () -> String { 7 }
g()'
# A trait default is a function like any other: same check, same anchor.
# (A class method cannot declare a return type — the grammar has no slot
# for one — so a trait default is where a method-shaped body gets checked.)
check_same "return type trait default" 'trait T {
  base() -> Long
  as_str() -> String {
    self.base()
  }
}
class C {
  new() { }
  base() {
    2
  }
}
C.new().as_str()'
# Two same-name methods in one trait: the static pass rejects the
# declaration (a trait has no overload set to merge them into).
check_same "trait duplicate method"    'println("before")
trait T {
  m() -> Long
  d() -> Long { 1 }
  d(a) -> Long { a }
}'
# A packed field write reports at the member — the field name is what the
# message is about — while the rest of a property write reports at the
# statement.
check_same "packed field write miss"   '@packable
class Pt {
  x: Long
  new(v) { self.x = v }
}
let buf = SharedBuffer.new(2, Pt)
buf[0].zz = 4'
check_same "packed view write miss"    '@packable
class Pt {
  x: Long
  new(v) { self.x = v }
}
let buf = SharedBuffer.new(2, Pt)
let v = buf[0]
v.zz = 4'

if [[ $fail -eq 0 ]]; then echo "jit_error_pos_test OK"; exit 0; fi
exit 1
