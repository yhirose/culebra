#!/usr/bin/env bash
# Regression test for the static lint pass (include/lint.h): the
# let-reassignment check must abort BEFORE eval (the sound subset) and
# must NOT flag the sound-negative cases. Driven end-to-end through the
# culebra binary so it exercises the real ModuleLoader wiring that all
# backends share. Usage: lint_test.sh <path-to-culebra>
set -u
CULEBRA="${1:?usage: lint_test.sh <culebra-binary>}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

# A rejected case prepends puts("RAN"); a correct before-eval abort means
# "RAN" never prints and the diagnostic is ImmutableError.
expect_reject() {
  printf 'puts("RAN")\n%s\n' "$2" > "$TMP/t.cul"
  out=$("$CULEBRA" "$TMP/t.cul" 2>&1)
  if [[ "$out" == *RAN* || "$out" != *ImmutableError* ]]; then
    echo "FAIL reject [$1]: $out"; fail=1
  fi
}

expect_accept() {
  printf '%s\n' "$2" > "$TMP/t.cul"
  if ! out=$("$CULEBRA" "$TMP/t.cul" 2>&1); then
    echo "FAIL accept [$1]: $out"; fail=1
  fi
}

# break/continue outside a loop: a sound before-eval abort means "RAN"
# never prints and the diagnostic is "SyntaxError: ... outside loop"
# (matching what the JIT already raises at compile time).
expect_loop_reject() {
  printf 'puts("RAN")\n%s\n' "$2" > "$TMP/t.cul"
  out=$("$CULEBRA" "$TMP/t.cul" 2>&1)
  if [[ "$out" == *RAN* || "$out" != *SyntaxError* || "$out" != *"outside loop"* ]]; then
    echo "FAIL loop-reject [$1]: $out"; fail=1
  fi
}

# Rejected: reassigning a `let` binding (hoisted ImmutableError).
expect_reject "top-level let reassign"  'let a = 1
a = 2'
expect_reject "fn-scope let reassign"   'f = fn () { let b = 1; b = 2 }'

# Accepted (sound-negative — must NOT be flagged):
expect_accept "let mut reassign"        'let mut a = 1
a = 2'
expect_accept "mut reassign"            'mut a = 1
a = 2'
expect_accept "no reassign"             'let a = 1'
# `{ }` is a scope boundary: the inner `let a` is block-local; the outer
# `a = 2` is a fresh auto-local, not a reassignment.
expect_accept "block-local then outer"  '{ let a = 1 }
a = 2'
# Separate function scopes: same name, no cross-scope reassignment.
expect_accept "separate fn scopes"      'f = fn () { let a = 1 }
g = fn () { let a = 2 }'

# Rejected: break / continue with no enclosing loop in the same function.
# A function / lambda / defer body is a loop boundary (break cannot cross it),
# so these abort before eval — the JIT segfaults or the interp aborts with an
# uncaught Break/ContinueSignal without this hoist.
expect_loop_reject "break top-level"          'break'
expect_loop_reject "continue top-level"       'continue'
expect_loop_reject "break in fn"              'fn f() { break }'
expect_loop_reject "continue in fn"           'fn f() { continue }'
expect_loop_reject "break in fn inside loop"  'for i in [1] { fn f() { break } }'
expect_loop_reject "break in defer in loop"   'for i in [1] { defer { break } }'
expect_loop_reject "continue in defer in loop" 'for i in [1] { defer { continue } }'

# Accepted (sound-negative — break / continue legitimately inside a loop):
expect_accept "break in while"          'mut n = 0
while true { n = n + 1; if n > 1 { break } }'
expect_accept "continue in while"       'mut n = 0
while n < 3 { n = n + 1; if n == 2 { continue } }'
expect_accept "break in for"            'for i in [1, 2] { break }'
expect_accept "break in if in for"      'for i in [1, 2] {
  if i == 2 { break }
}'
expect_accept "break in block in loop"  'for i in [1, 2] {
  { if i == 2 { break } }
}'
expect_accept "break in try in loop"    'for i in [1, 2] {
  try { if i == 2 { break } } catch e { }
}'
# A loop nested inside a fn re-establishes loop context; its break is fine
# even though the fn itself is inside an outer loop.
expect_accept "nested loop in fn"       'for i in [1] {
  fn f() { for j in [1, 2] { break } }
}'

# Duplicate parameter names: the earlier binding is unreachable (calls bind
# last-wins), so it aborts before eval with "SyntaxError: duplicate parameter".
expect_dup_reject() {
  printf 'puts("RAN")\n%s\n' "$2" > "$TMP/t.cul"
  out=$("$CULEBRA" "$TMP/t.cul" 2>&1)
  if [[ "$out" == *RAN* || "$out" != *"duplicate parameter"* ]]; then
    echo "FAIL dup-reject [$1]: $out"; fail=1
  fi
}
expect_dup_reject "fn dup"              'fn f(x, x) { x }'
expect_dup_reject "fn dup non-adjacent" 'fn f(a, b, a) { a }'
expect_dup_reject "lambda dup"          'let g = |x, x| x'
expect_dup_reject "typed dup"           'fn f(x: Int, x: Int) { x }'
expect_dup_reject "param vs rest dup"   'fn f(x, *x) { x }'
expect_dup_reject "ctor dup"            'class C { new(a, a) { } }'
expect_dup_reject "method dup"          'class C { greet(name, name) { } }'
expect_dup_reject "trait sig dup"       'trait T { sig(a, a) }'

# Accepted (sound-negative): `_` sink may repeat; distinct names / patterns ok.
expect_accept "sink params repeat"      'fn f(_, _) { 1 }'
expect_accept "distinct params"         'fn f(x, y) { x + y }'
expect_accept "defaulted distinct"      'fn f(a, b = 5) { a }'
expect_accept "args-rest distinct"      'fn f(x, *args) { x }'
expect_accept "destructuring param"     'fn f({a, b}) { a }'
expect_accept "param shadowed by let"   'fn f(x) { let mut x = 1 }'

# Reserved parameter names: `self` / `this` are language-core identifiers the
# callee binds unconditionally, so a same-named parameter shadows that binding
# (and on the JIT the overwritten implicit slot leaked a ref every call). Abort
# before eval with "SyntaxError: '<name>' is a reserved name".
expect_reserved_reject() {
  printf 'puts("RAN")\n%s\n' "$2" > "$TMP/t.cul"
  out=$("$CULEBRA" "$TMP/t.cul" 2>&1)
  if [[ "$out" == *RAN* || "$out" != *"is a reserved name"* ]]; then
    echo "FAIL reserved-reject [$1]: $out"; fail=1
  fi
}
expect_reserved_reject "fn self"        'fn f(self, x) { x }'
expect_reserved_reject "fn this"        'fn f(this) { 1 }'
expect_reserved_reject "self non-first" 'fn f(x, self) { x }'
expect_reserved_reject "lambda self"    'let g = |self| self'
expect_reserved_reject "method self"    'class C { m(self, x) { x } }'
expect_reserved_reject "ctor this"      'class C { new(this) { } }'
expect_reserved_reject "trait sig self" 'trait T { go(self) }'
expect_reserved_reject "kw-only this"   'fn f(x, *, this) { x }'
expect_reserved_reject "typed self"     'fn f(self: Int) { 1 }'

# Accepted (sound-negative): names that merely CONTAIN self/this are ordinary.
expect_accept "selfie param"            'fn f(selfie, x) { selfie }'
expect_accept "myself param"            'fn f(myself) { myself }'
expect_accept "this_ param"             'fn f(this_) { this_ }'

# `return` outside any function is a SyntaxError (docs §return; `Sys.exit`
# is the early-exit mechanism, and the module interface is `export`, so a
# top-level return value goes nowhere).
expect_syntax_reject() {
  printf 'puts("RAN")\n%s\n' "$2" > "$TMP/t.cul"
  out=$("$CULEBRA" "$TMP/t.cul" 2>&1)
  if [[ "$out" == *RAN* || "$out" != *"return outside function"* ]]; then
    echo "FAIL return-reject [$1]: $out"; fail=1
  fi
}
expect_syntax_reject "top-level return value" 'return 5'
expect_syntax_reject "top-level bare return"  'return'
expect_syntax_reject "return in top if"       'if true { return 1 }'
expect_syntax_reject "return in top for"      'for i in [1] { return }'

# Accepted: `return` inside a function / lambda / method, or a defer closure
# (a defer body is its own closure, so its `return` exits the defer).
expect_accept "return in fn"            'fn f() { return 5 }'
expect_accept "return in nested loop"   'fn f() { for i in [1] { return i } }'
expect_accept "return in method"        'class C { m() { return 9 } }'
expect_accept "return in defer"         'fn f() { defer { return } }'
expect_accept "return in top defer"     'for i in [1] { defer { return } }'

# Malformed parameter lists: ordering rules that are certain to fail. Hoisted
# into lint.h so every backend rejects them pre-eval with the same message
# (the JIT previously accepted `**` not-last, a duplicate `*`, and a trailing
# bare `*` silently — those were interp/JIT divergences). `$3` is a substring
# of the expected SyntaxError message.
expect_param_reject() {
  printf 'puts("RAN")\n%s\n' "$3" > "$TMP/t.cul"
  out=$("$CULEBRA" "$TMP/t.cul" 2>&1)
  if [[ "$out" == *RAN* || "$out" != *SyntaxError* || "$out" != *"$2"* ]]; then
    echo "FAIL param-reject [$1]: $out"; fail=1
  fi
}
expect_param_reject "non-default after default" "non-default parameter 'b' follows a default parameter" 'fn f(a = 1, b) { b }'
expect_param_reject "args not last"     "'*args' must be the last parameter"        'fn f(*args, b) { b }'
expect_param_reject "args after sep"    "'*args' cannot follow a '*' separator"     'fn f(a, *, *args) { a }'
expect_param_reject "kwargs not last"   "'**' catch-all must be the last parameter" 'fn f(**kw, b) { b }'
expect_param_reject "dup separator"     "duplicate '*' keyword-only separator"      'fn f(a, *, b, *, c) { a }'
expect_param_reject "dangling separator" "named arguments must follow '*' separator" 'fn f(a, *) { a }'
# The same rules apply in every param-bearing form (lambda / method / trait).
expect_param_reject "lambda non-default" "non-default parameter 'b' follows a default parameter" 'let g = |a = 1, b| a'
expect_param_reject "method kwargs not last" "'**' catch-all must be the last parameter" 'class C { m(**kw, b) { } }'
expect_param_reject "trait sig dangling sep" "named arguments must follow '*' separator" 'trait T { sig(a, *) }'
# Dead code still rejected (the win over the interp's eval-time check, which
# would never run a never-evaluated branch).
expect_param_reject "dead-code malformed" "'**' catch-all must be the last parameter" 'if false { fn f(**kw, b) { } }'

# Accepted (sound-negative): every well-formed parameter shape must still run.
expect_accept "plain params"            'fn f(a, b) { }'
expect_accept "trailing defaults"       'fn f(a, b = 1, c = 2) { }'
expect_accept "args rest last"          'fn f(a, *args) { }'
expect_accept "kwargs rest last"        'fn f(a, **kw) { }'
expect_accept "kw-only after sep"       'fn f(a, *, b) { }'
expect_accept "kw-only default + plain" 'fn f(a, *, b = 1, c) { }'
expect_accept "default before sep"      'fn f(a = 1, *, b) { }'
expect_accept "sep then kwargs"         'fn f(a, *, b, **kw) { }'

# Malformed assignments: shapes the interp's eval_assignment rejects before any
# write. Hoisted into lint.h so every backend rejects them pre-eval (the JIT
# previously ran a keyword LHS like `if = 1` silently). Reuses the SyntaxError
# substring helper.
expect_param_reject "compound declares (let)"  "compound assignment cannot declare a new variable" 'let x += 1'
expect_param_reject "compound declares (mut)"  "compound assignment cannot declare a new variable" 'mut x += 1'
expect_param_reject "compound declares (??=)"  "compound assignment cannot declare a new variable" 'let x ??= 1'
expect_param_reject "??= on index target"      "is only supported on a simple variable target" 'mut o = [1]
o[0] ??= 9'
expect_param_reject "??= on property target"   "is only supported on a simple variable target" 'mut o = { a: 1 }
o.a ??= 9'
expect_param_reject "keyword LHS (if)"         "left-hand side is invalid variable name" 'if = 1'
expect_param_reject "keyword LHS (true)"       "left-hand side is invalid variable name" 'true = 1'
expect_param_reject "keyword LHS (match)"      "left-hand side is invalid variable name" 'match = 1'

# Accepted (sound-negative): well-formed assignments must still run.
expect_accept "plain reassign"          'mut x = 0
x = 5'
expect_accept "compound on mut"         'mut x = 1
x += 2'
expect_accept "??= on simple var"       'mut x = nil
x ??= 7'
expect_accept "index assign"            'mut o = [1]
o[0] = 9'

# Shadow: a nested function may not shadow a binding captured from an enclosing
# function. Single-sourced in lint.h (lint::check_shadow); the JIT now routes
# through it instead of its own collect_fn_locals pass, so assert BOTH backends
# reject before eval. For-loop variables are block-scoped to the body and are
# captured by closures there — shadowing one was previously missed by the interp.
expect_shadow_reject() {
  printf 'puts("RAN")\n%s\n' "$2" > "$TMP/t.cul"
  for be in "" "--jit"; do
    out=$("$CULEBRA" $be "$TMP/t.cul" 2>&1)
    if [[ "$out" == *RAN* || "$out" != *ShadowError* ]]; then
      echo "FAIL shadow-reject [$1${be:+ jit}]: $out"; fail=1
    fi
  done
}
expect_shadow_accept() {
  printf '%s\n' "$2" > "$TMP/t.cul"
  for be in "" "--jit"; do
    if ! out=$("$CULEBRA" $be "$TMP/t.cul" 2>&1); then
      echo "FAIL shadow-accept [$1${be:+ jit}]: $out"; fail=1
    fi
  done
}
expect_shadow_reject "inner param shadows let"   'fn outer() { let x = 1; fn inner(x) { x } }'
expect_shadow_reject "for-var simple inside loop" 'fn f() { for i in [1] { fn g(i) { i } } }'
expect_shadow_reject "for-var destructure inside" 'fn f() { for (i, x) in [(1, 2)] { fn g(x) { x } } }'
expect_shadow_reject "block-let nested fn"        'fn f() { { let b = 1; fn g(b) { b } } }'
# Accepted (sound-negative): out of scope, or a legitimate capture.
expect_shadow_accept "for-var after loop"         'fn f() { for i in [1] { }; fn g() { let i = 2 } }'
expect_shadow_accept "toplevel for-var"           'for i in [1] { fn g(i) { i } }'
expect_shadow_accept "nested captures for-var"    'fn f() { for i in [1] { let y = i; fn g() { y } } }'
expect_shadow_accept "toplevel binding shadowable" 'let x = 1
fn f() { let x = 2; x }'

# --- Undefined variable (sound subset): a name bound in no enclosing scope
# and not a builtin is certain to raise NameError, so it aborts before eval
# (like ShadowError, and regardless of whether the code is reachable). A
# flow-dependent NameError (use-before-def) is NOT flagged — it stays a
# catchable runtime error. Load-stage check, so it fires on every backend.
expect_undef_reject() {
  printf 'puts("RAN")\n%s\n' "$2" > "$TMP/t.cul"
  for be in "" "--jit"; do
    out=$("$CULEBRA" $be "$TMP/t.cul" 2>&1)
    if [[ "$out" == *RAN* || "$out" != *NameError* ||
          "$out" != *"undefined variable"* ]]; then
      echo "FAIL undef-reject [$1${be:+ jit}]: $out"; fail=1
    fi
  done
}
expect_undef_accept() {
  printf '%s\n' "$2" > "$TMP/t.cul"
  for be in "" "--jit"; do
    if ! out=$("$CULEBRA" $be "$TMP/t.cul" 2>&1); then
      echo "FAIL undef-accept [$1${be:+ jit}]: $out"; fail=1
    fi
  done
}
expect_undef_reject "top-level read"          'puts(zzz)'
expect_undef_reject "uncalled fn body read"   'fn f() { puts(zzz) }'
expect_undef_reject "undefined object value"  'let o = {k: zzz}'
expect_undef_reject "undefined kwarg value"   'fn f(a) { a }
f(a: zzz)'
expect_undef_reject "undefined index base"    'zzz[0] = 1'
# Accepted (sound-negative — must run on both backends):
expect_undef_accept "forward-ref fn body"     'fn a() { b() }
fn b() { 1 }
a()'
expect_undef_accept "use-before-def runtime"  'let r = try { x; let x = 1; nil } catch e { nil }'
expect_undef_accept "destructure binding"     'let (p, q) = (1, 2)
puts(p + q)'
expect_undef_accept "for-var read"            'for i in [1, 2] { puts(i) }'
expect_undef_accept "match arm binding"       'let v = match 1 { n => n }
puts(v)'
expect_undef_accept "catch var"               'try { throw "x" } catch e { puts(e) }'
expect_undef_accept "this in method"          'class C { f() { this } }'
expect_undef_accept "self recursion"          'fn f(n) { if n > 0 { self(n - 1) } else { 0 } }
puts(f(3))'
expect_undef_accept "dunder __ARGS__"         'fn f(*xs) { __ARGS__.size() }
puts(f(1, 2))'
expect_undef_accept "ufcs method name"        'let xs = [1]
puts(xs.size())'
expect_undef_accept "object shorthand"        'let k = 1
let o = {k}'
expect_undef_accept "enum variant names"      'enum Color { Red, Green, Blue }
puts(Color.Red)'

# Duplicate class members abort before eval on every backend (the interp
# used to keep the first definition silently; the JIT threw a catchable
# ImmutableError mid-definition). Static and instance members are separate
# namespaces, so a static/instance same-name pair stays accepted.
expect_dup_member_reject() {
  printf 'puts("RAN")\n%s\n' "$2" > "$TMP/t.cul"
  for be in "" "--jit"; do
    out=$("$CULEBRA" $be "$TMP/t.cul" 2>&1)
    if [[ "$out" == *RAN* || "$out" != *"duplicate member"* ]]; then
      echo "FAIL dup-member reject [$1${be:+ jit}]: $out"; fail=1
    fi
  done
}
expect_dup_member_accept() {
  printf '%s\n' "$2" > "$TMP/t.cul"
  for be in "" "--jit"; do
    if ! out=$("$CULEBRA" $be "$TMP/t.cul" 2>&1); then
      echo "FAIL dup-member accept [$1${be:+ jit}]: $out"; fail=1
    fi
  done
}
# Constructors, operator/dunder methods, and statics don't overload — a
# duplicate is a "duplicate member" error. A field clashing with a method
# name is too.
expect_dup_member_reject "__call__"     'class A { __call__(x) { x } __call__(x) { x + 1 } }'
expect_dup_member_reject "dup new"      'class A { new(){} new(x){} }'
expect_dup_member_reject "static"       'class A { static v = 1 static v = 2 }'
expect_dup_member_reject "field/method" 'class A { foo(x){1} foo: Long = 0 }'

# Two instance methods with an IDENTICAL positional-type signature are
# unreachable / ambiguous — rejected with a distinct message.
identical_sig_reject() {
  printf 'puts("RAN")\n%s\n' "$2" > "$TMP/t.cul"
  for be in "" "--jit"; do
    out=$("$CULEBRA" $be "$TMP/t.cul" 2>&1)
    if [[ "$out" == *RAN* || "$out" != *"identical signature"* ]]; then
      echo "FAIL identical-sig reject [$1${be:+ jit}]: $out"; fail=1
    fi
  done
}
identical_sig_reject "same arity, no types" 'class A { m() { 1 } m() { 2 } }'
identical_sig_reject "same types"           'class A { m(x: Long) { 1 } m(y: Long) { 2 } }'

expect_dup_member_accept "static vs instance" 'class A { static m = 1 m() { 2 } }
puts(A.m + A.new().m())'
expect_dup_member_accept "two classes" 'class A { m() { 1 } }
class B { m() { 2 } }
puts(A.new().m() + B.new().m())'
# Instance-method multidispatch: same name, distinct positional-type sigs.
expect_dup_member_accept "method overload" 'class A { new(){} m(x: Long) { "l" } m(x: String) { "s" } }
puts(A.new().m(1) + A.new().m("a"))'
expect_dup_member_accept "arity overload" 'class A { new(){} m() { 1 } m(x) { x } }
puts(A.new().m() + A.new().m(41))'

# --- `culebra lint` CLI: advisory warnings (unused locals) in report mode ---
# `culebra lint <file>` prints diagnostics without running and sets exit code
# 0 (clean) / 1 (warnings only) / 2 (errors). A warning case must name the
# unused variable and exit 1; a sound-negative must be clean (exit 0).
expect_lint_warns() {  # $1=label $2=source $3=substring that must appear
  printf '%s\n' "$2" > "$TMP/l.cul"
  out=$("$CULEBRA" lint "$TMP/l.cul" 2>&1); rc=$?
  if [[ $rc -ne 1 || "$out" != *"$3"* ]]; then
    echo "FAIL lint-warn [$1]: rc=$rc out=$out"; fail=1
  fi
}
expect_lint_clean() {  # $1=label $2=source
  printf '%s\n' "$2" > "$TMP/l.cul"
  out=$("$CULEBRA" lint "$TMP/l.cul" 2>&1); rc=$?
  if [[ $rc -ne 0 || -n "$out" ]]; then
    echo "FAIL lint-clean [$1]: rc=$rc out=$out"; fail=1
  fi
}
expect_lint_error() {  # $1=label $2=source
  printf '%s\n' "$2" > "$TMP/l.cul"
  out=$("$CULEBRA" lint "$TMP/l.cul" 2>&1); rc=$?
  if [[ $rc -ne 2 ]]; then
    echo "FAIL lint-error [$1]: rc=$rc out=$out"; fail=1
  fi
}

expect_lint_warns "unused let" 'fn f() { let a = 1; 2 }'        "unused variable 'a'"
expect_lint_warns "unused mut" 'fn f() { mut a = 1; a = 2; 3 }' "unused variable 'a'"
# Sound-negatives — must NOT warn:
expect_lint_clean "read let"        'fn f() { let a = 1; a + 1 }'
expect_lint_clean "closure capture" 'fn f() { let a = 1; || a + 1 }'
expect_lint_clean "compound is use" 'fn f() { mut a = 0; a += 1; a }'
expect_lint_clean "sink"            'fn f() { let _ = 1; 2 }'
expect_lint_clean "unused param"    'fn f(x) { 1 }'   # params not flagged (MVP)
expect_lint_clean "unused toplevel" 'let g = 1'       # top-level not flagged (MVP)
# Errors propagate through the same CLI with exit 2:
expect_lint_error "undefined var"      'fn f() { nope }'
expect_lint_error "break outside loop" 'fn f() { break }'

# --- effects / generators through the CLI ---
# The CLI must run its error checks over the *lowered* AST (what the backends
# run). On the raw parse, `effect fn` and `handle … with` bind names no
# analyzer knows, so operations, clause parameters and `resume` all read as
# undefined and every effects file fails to lint.
expect_lint_clean "effects tail-resumptive" 'effect fn ask()
puts(handle { perform ask() } with ask(resume) { resume(1) })'
expect_lint_clean "effects effect fn body" 'effect fn ask()
effect fn double() { let n = perform ask(); n * 2 }
puts(handle { double() } with ask(resume) { resume(21) })'
expect_lint_clean "effects multi-shot" 'effect fn choose(a, b)
puts(handle { perform choose(1, 2) } with choose(a, b, resume) { [resume(a), resume(b)] })'
expect_lint_clean "effects return clause" 'effect fn get()
mut cell = 0
puts(handle { perform get() } with get(resume) { resume(cell) } with return(v) { v + 1 })'
expect_lint_clean "generator yield" 'fn counter(n) { mut i = 0; while i < n { yield i; i = i + 1 } }
mut t = 0
for v in counter(3) { t = t + v }
puts(t)'
# Advisory warnings run over the source as written, so bindings the lowering
# synthesizes are never reported: an abort clause deliberately never reads its
# `resume`, and a clause may ignore an operation argument.
expect_lint_clean "abort clause ignores resume" 'effect fn fail()
puts(handle { perform fail(); "x" } with fail(resume) { "aborted" })'
expect_lint_clean "clause ignores an op arg" 'effect fn pair(x, y)
puts(handle { perform pair(1, 2) } with pair(x, y, resume) { resume(x) })'
# The lowering rejects malformed effects by throwing; the CLI must turn that
# into an error diagnostic (exit 2) rather than aborting on uncaught exception.
expect_lint_error "two return clauses" 'effect fn a()
puts(handle { perform a() } with a(r) { r(1) } with return(v) { v } with return(w) { w })'
expect_lint_error "duplicate clause" 'effect fn a()
puts(handle { perform a() } with a(r) { r(1) } with a(r) { r(2) })'
# A real unused local elsewhere in the same file is still reported:
expect_lint_warns "unused local beside effects" 'effect fn ask()
fn helper() { let dead = 1; 2 }
puts(handle { perform ask() } with ask(resume) { resume(helper()) })' "unused variable 'dead'"
# An `effect fn` body and each handler clause body are their own scopes, so an
# unused local written in one is reported (at its authored position) just like
# in any function.
expect_lint_warns "unused in effect fn body" 'effect fn ask()
effect fn work() { let dead = 1; perform ask() }
puts(handle { work() } with ask(resume) { resume(2) })' "unused variable 'dead'"
expect_lint_warns "unused in handler clause body" 'effect fn ask()
puts(handle { perform ask() } with ask(resume) { let dead = 1; resume(2) })' "unused variable 'dead'"
expect_lint_warns "unused in return clause body" 'effect fn ask()
puts(handle { perform ask() } with ask(resume) { resume(2) } with return(v) { let dead = 1; v })' "unused variable 'dead'"
# Sound-negatives — locals that ARE used in these scopes must stay clean:
expect_lint_clean "used in effect fn body" 'effect fn ask()
effect fn work() { let n = perform ask(); n * 2 }
puts(handle { work() } with ask(resume) { resume(2) })'
expect_lint_clean "used in handler clause body" 'effect fn ask()
puts(handle { perform ask() } with ask(resume) { let doubled = 2 * 3; resume(doubled) })'
# A clause captures an enclosing local — that read must count, so no warning:
expect_lint_clean "clause captures enclosing local" 'effect fn ask()
fn f() { let base = 100; handle { perform ask() } with ask(resume) { resume(base) } }
puts(f())'

if [[ $fail -eq 0 ]]; then echo "lint_test OK"; exit 0; fi
exit 1
