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

# A rejected case prepends inspect("RAN"); a correct before-eval abort means
# "RAN" never prints and the diagnostic is ImmutableError.
expect_reject() {
  printf 'inspect("RAN")\n%s\n' "$2" > "$TMP/t.cul"
  out=$("$CULEBRA" --tree "$TMP/t.cul" 2>&1)
  if [[ "$out" == *RAN* || "$out" != *ImmutableError* ]]; then
    echo "FAIL reject [$1]: $out"; fail=1
  fi
}

expect_accept() {
  printf '%s\n' "$2" > "$TMP/t.cul"
  if ! out=$("$CULEBRA" --tree "$TMP/t.cul" 2>&1); then
    echo "FAIL accept [$1]: $out"; fail=1
  fi
}

# break/continue outside a loop: a sound before-eval abort means "RAN"
# never prints and the diagnostic is "SyntaxError: ... outside loop"
# (matching what the JIT already raises at compile time).
expect_loop_reject() {
  printf 'inspect("RAN")\n%s\n' "$2" > "$TMP/t.cul"
  out=$("$CULEBRA" --tree "$TMP/t.cul" 2>&1)
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
  printf 'inspect("RAN")\n%s\n' "$2" > "$TMP/t.cul"
  out=$("$CULEBRA" --tree "$TMP/t.cul" 2>&1)
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

# Reserved parameter names: `self` / `fn` are language-core identifiers the
# callee binds unconditionally, so a same-named parameter shadows that binding
# (and on the JIT the overwritten implicit slot leaked a ref every call). Abort
# before eval with "SyntaxError: '<name>' is a reserved name".
expect_reserved_reject() {
  printf 'inspect("RAN")\n%s\n' "$2" > "$TMP/t.cul"
  out=$("$CULEBRA" --tree "$TMP/t.cul" 2>&1)
  if [[ "$out" == *RAN* || "$out" != *"is a reserved name"* ]]; then
    echo "FAIL reserved-reject [$1]: $out"; fail=1
  fi
}
expect_reserved_reject "fn self"        'fn f(self, x) { x }'
expect_reserved_reject "fn fn"          'fn f(fn) { 1 }'
expect_reserved_reject "self non-first" 'fn f(x, self) { x }'
expect_reserved_reject "lambda self"    'let g = |self| self'
expect_reserved_reject "method self"    'class C { m(self, x) { x } }'
expect_reserved_reject "ctor fn"        'class C { new(fn) { } }'
expect_reserved_reject "trait sig self" 'trait T { go(self) }'
expect_reserved_reject "kw-only fn"     'fn f(x, *, fn) { x }'
expect_reserved_reject "typed self"     'fn f(self: Int) { 1 }'

# Accepted (sound-negative): names that merely CONTAIN self/fn are ordinary.
expect_accept "selfie param"            'fn f(selfie, x) { selfie }'
expect_accept "myself param"            'fn f(myself) { myself }'
expect_accept "fn_ param"               'fn f(fn_) { fn_ }'
expect_accept "this param"              'fn f(this) { this }'

# `return` outside any function is a SyntaxError (docs §return; `Sys.exit`
# is the early-exit mechanism, and the module interface is `export`, so a
# top-level return value goes nowhere).
expect_syntax_reject() {
  printf 'inspect("RAN")\n%s\n' "$2" > "$TMP/t.cul"
  out=$("$CULEBRA" --tree "$TMP/t.cul" 2>&1)
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
  printf 'inspect("RAN")\n%s\n' "$3" > "$TMP/t.cul"
  out=$("$CULEBRA" --tree "$TMP/t.cul" 2>&1)
  if [[ "$out" == *RAN* || "$out" != *SyntaxError* || "$out" != *"$2"* ]]; then
    echo "FAIL param-reject [$1]: $out"; fail=1
  fi
}
expect_param_reject "non-default after default" "non-default parameter 'b' follows a default parameter" 'fn f(a = 1, b) { b }'
# A destructuring parameter takes no default, so it is a required one: after
# a defaulted parameter no arity can fill it (the interp raised a runtime
# ArityError naming the synthetic slot; the JIT read the unfilled slab entry
# and crashed).
expect_param_reject "pattern after default" "non-default parameter '__destructure_1' follows a default parameter" 'let f = fn (a = 1, [b, c]) { b }'
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
# `??=` on a complex lvalue (index / property) is accepted: it extends past
# the former simple-variable-only MVP to Object dot/subscript and Array
# elements.
expect_accept "??= on index target"            'mut o = [1]
o[0] ??= 9'
expect_accept "??= on property target"         'mut o = { a: 1 }
o.a ??= 9'
expect_param_reject "keyword LHS (if)"         "left-hand side is invalid variable name" 'if = 1'
expect_param_reject "keyword LHS (true)"       "left-hand side is invalid variable name" 'true = 1'
expect_param_reject "keyword LHS (match)"      "left-hand side is invalid variable name" 'match = 1'
# Declaration introducers are reserved too: the PEG backtracks them to
# IDENTIFIER, so only this check keeps `class`/`import` from naming a variable.
expect_param_reject "keyword LHS (class)"      "left-hand side is invalid variable name" 'let class = 1'
expect_param_reject "keyword LHS (trait)"      "left-hand side is invalid variable name" 'let trait = 1'
expect_param_reject "keyword LHS (enum)"       "left-hand side is invalid variable name" 'let enum = 1'
expect_param_reject "keyword LHS (import)"     "left-hand side is invalid variable name" 'let import = 1'
expect_param_reject "keyword LHS (export)"     "left-hand side is invalid variable name" 'let export = 1'
expect_param_reject "keyword LHS (yield)"      "left-hand side is invalid variable name" 'let yield = 1'
# A lone non-identifier target used to be declared as a nameless slot, so the
# statement silently did nothing.
expect_param_reject "literal LHS"              "left-hand side is invalid variable name" '1 = 2'
expect_param_reject "compound tuple LHS"       "left-hand side is invalid variable name" 'mut a = 1
mut b = 2
(a, b) += (1, 2)'
# A call has no storage to write; both backends used to abort with a
# non-CulebraError (and with different text) instead of rejecting it.
expect_param_reject "call target"              "cannot assign to a function call result" 'let f = fn () { [1] }
f() = 3'
# `(f(), x)` is a CTOR_PATTERN tuple (`Ok(v)` style), so a call target only
# reaches PLACE_ASSIGN when the chain starts with something no pattern matches.
expect_param_reject "call target in place assign" "cannot assign to a function call result" 'mut fs = [fn () { 1 }]
mut x = 0
(fs[0].call(), x) = (1, 2)'

# Accepted (sound-negative): well-formed assignments must still run.
expect_accept "plain reassign"          'mut x = 0
x = 5'
# Place assignment: a chain target reads its receiver / index, a plain-name
# target writes (and may declare). Getting either wrong makes the sound
# undefined-variable pass reject a correct program at load.
expect_accept "place assign swap"       'mut p = [1, 2]
mut i = 0
mut j = 1
(p[i], p[j]) = (p[j], p[i])'
expect_accept "place assign declares"   'mut q = [1]
(fresh, q[0]) = (7, 6)
inspect(fresh)'
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
  printf 'inspect("RAN")\n%s\n' "$2" > "$TMP/t.cul"
  for be in "--tree" "--jit"; do
    out=$("$CULEBRA" $be "$TMP/t.cul" 2>&1)
    if [[ "$out" == *RAN* || "$out" != *ShadowError* ]]; then
      echo "FAIL shadow-reject [$1 ${be#--}]: $out"; fail=1
    fi
  done
}
expect_shadow_accept() {
  printf '%s\n' "$2" > "$TMP/t.cul"
  for be in "--tree" "--jit"; do
    if ! out=$("$CULEBRA" $be "$TMP/t.cul" 2>&1); then
      echo "FAIL shadow-accept [$1 ${be#--}]: $out"; fail=1
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
  printf 'inspect("RAN")\n%s\n' "$2" > "$TMP/t.cul"
  for be in "--tree" "--jit"; do
    out=$("$CULEBRA" $be "$TMP/t.cul" 2>&1)
    if [[ "$out" == *RAN* || "$out" != *NameError* ||
          "$out" != *"undefined variable"* ]]; then
      echo "FAIL undef-reject [$1 ${be#--}]: $out"; fail=1
    fi
  done
}
expect_undef_accept() {
  printf '%s\n' "$2" > "$TMP/t.cul"
  for be in "--tree" "--jit"; do
    if ! out=$("$CULEBRA" $be "$TMP/t.cul" 2>&1); then
      echo "FAIL undef-accept [$1 ${be#--}]: $out"; fail=1
    fi
  done
}
expect_undef_reject "top-level read"          'inspect(zzz)'
expect_undef_reject "uncalled fn body read"   'fn f() { inspect(zzz) }'
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
inspect(p + q)'
expect_undef_accept "for-var read"            'for i in [1, 2] { inspect(i) }'
expect_undef_accept "match arm binding"       'let v = match 1 { n => n }
inspect(v)'
expect_undef_accept "catch var"               'try { throw "x" } catch e { inspect(e) }'
expect_undef_accept "self in method"          'class C { f() { self } }'
expect_undef_accept "fn recursion"            'fn f(n) { if n > 0 { fn(n - 1) } else { 0 } }
inspect(f(3))'
expect_undef_accept "dunder __ARGS__"         'fn f(*xs) { __ARGS__.size() }
inspect(f(1, 2))'
expect_undef_accept "ufcs method name"        'let xs = [1]
inspect(xs.size())'
expect_undef_accept "object shorthand"        'let k = 1
let o = {k}'
expect_undef_accept "enum variant names"      'enum Color { Red, Green, Blue }
inspect(Color.Red)'

# Duplicate class members abort before eval on every backend (the interp
# used to keep the first definition silently; the JIT threw a catchable
# ImmutableError mid-definition). Static and instance members are separate
# namespaces, so a static/instance same-name pair stays accepted.
expect_dup_member_reject() {
  printf 'inspect("RAN")\n%s\n' "$2" > "$TMP/t.cul"
  for be in "--tree" "--jit"; do
    out=$("$CULEBRA" $be "$TMP/t.cul" 2>&1)
    if [[ "$out" == *RAN* || "$out" != *"duplicate member"* ]]; then
      echo "FAIL dup-member reject [$1 ${be#--}]: $out"; fail=1
    fi
  done
}
expect_dup_member_accept() {
  printf '%s\n' "$2" > "$TMP/t.cul"
  for be in "--tree" "--jit"; do
    if ! out=$("$CULEBRA" $be "$TMP/t.cul" 2>&1); then
      echo "FAIL dup-member accept [$1 ${be#--}]: $out"; fail=1
    fi
  done
}
# A duplicate static member or a field clashing with a method name is a
# "duplicate member" error. (Constructors AND operator/dunder methods DO
# overload now — see the accept / identical-sig cases below.)
expect_dup_member_reject "static"       'class A { static v = 1 static v = 2 }'
expect_dup_member_reject "field/method" 'class A { foo(x){1} foo: Long = 0 }'

# Two instance methods with an IDENTICAL positional-type signature are
# unreachable / ambiguous — rejected with a distinct message.
identical_sig_reject() {
  printf 'inspect("RAN")\n%s\n' "$2" > "$TMP/t.cul"
  for be in "--tree" "--jit"; do
    out=$("$CULEBRA" $be "$TMP/t.cul" 2>&1)
    if [[ "$out" == *RAN* || "$out" != *"identical signature"* ]]; then
      echo "FAIL identical-sig reject [$1 ${be#--}]: $out"; fail=1
    fi
  done
}
identical_sig_reject "same arity, no types" 'class A { m() { 1 } m() { 2 } }'
identical_sig_reject "same types"           'class A { m(x: Long) { 1 } m(y: Long) { 2 } }'
# Constructors overload on the same rule: an identical `new` signature is
# an unreachable-overload error, distinct signatures merge (accepted below).
identical_sig_reject "dup new same sig"     'class A { new(x: Long){self.x=x} new(y: Long){self.x=y} }'
# Operator/dunder methods overload on the same rule too — an identical
# signature is the same unreachable-overload error.
identical_sig_reject "dup dunder same sig"  'class A { __call__(x) { x } __call__(y) { y + 1 } }'

expect_dup_member_accept "static vs instance" 'class A { static m = 1 m() { 2 } }
inspect(A.m + A.new().m())'
expect_dup_member_accept "two classes" 'class A { m() { 1 } }
class B { m() { 2 } }
inspect(A.new().m() + B.new().m())'
# Instance-method multidispatch: same name, distinct positional-type sigs.
expect_dup_member_accept "method overload" 'class A { new(){} m(x: Long) { "l" } m(x: String) { "s" } }
inspect(A.new().m(1) + A.new().m("a"))'
expect_dup_member_accept "arity overload" 'class A { new(){} m() { 1 } m(x) { x } }
inspect(A.new().m() + A.new().m(41))'
# Constructor multidispatch: same name (`new`), distinct positional-type
# / arity sigs merge into one ctor dispatcher.
expect_dup_member_accept "ctor overload" 'class A { new(x: Long){self.t="l"} new(x: String){self.t="s"} new(){self.t="e"} }
inspect(A(1).t + A("a").t + A().t)'
# Dunder / operator multidispatch: same dunder name, distinct operand-type
# sigs merge into one dispatcher picked by the operand type.
expect_dup_member_accept "dunder overload" 'class A { new(n: Long){self.n=n} __add__(o: A){A(self.n+o.n)} __add__(k: Long){A(self.n+k)} }
inspect((A(2)+A(3)).n + (A(2)+10).n)'

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
# Parameters are deliberately never flagged: an unused parameter is
# overwhelmingly intentional (fixed dispatch/callback/method arity), so the
# check can't meet the zero-false-positive bar.
expect_lint_clean "unused param"    'fn f(x) { 1 }'

# --- unused top-level bindings (imports + top-level let/mut) ---
expect_lint_warns "unused toplevel let" 'let g = 1'   "unused top-level binding 'g'"
expect_lint_warns "unused toplevel mut" 'mut g = 1'   "unused top-level binding 'g'"
# Read anywhere (incl. inside a fn / via UFCS) or re-exported ⇒ used.
expect_lint_clean "toplevel read"    'let g = 1
inspect(g)'
expect_lint_clean "toplevel read in fn" 'let g = 1
fn f() { g + 1 }'
expect_lint_clean "toplevel ufcs use" 'let inc = fn (x) { x + 1 }
inspect((5).inc())'
expect_lint_clean "toplevel exported"  'let g = 1
export { g }'
# A top-level fn/class is the export surface — never a candidate.
expect_lint_clean "toplevel fn not flagged" 'fn helper() { 1 }'
# Underscore opt-out on a top-level binding.
expect_lint_clean "toplevel underscore"  'let _g = 1'
# `import` is the other unused-toplevel candidate. Lint runs below the
# module-loading layer (pure AST, no filesystem access), so the imported
# path need not resolve to a real file.
expect_lint_warns "unused import" "import Math from 'std/math'
inspect(1)" "unused import 'Math'"
expect_lint_clean "used import"  "import Math from 'std/math'
inspect(Math.sqrt(4))"

# --- unreachable code (straight-line terminator + a following statement) ---
expect_lint_warns "unreachable after return" 'fn f() { return 1
inspect(2) }'                                    "unreachable code"
expect_lint_warns "unreachable after throw"  'throw "x"
inspect(1)'                                       "unreachable code"
expect_lint_warns "unreachable after break"  'for i in [1] { break
inspect(i) }'                                     "unreachable code"
expect_lint_warns "unreachable after continue" 'for i in [1] { continue
inspect(i) }'                                     "unreachable code"
# A terminator that IS the last statement is fine; a return inside an if does
# not make the enclosing block dead.
expect_lint_clean "return is last"    'fn f() { let a = 1
return a }'
expect_lint_clean "return in if branch" 'fn f(x) { if x { return 1 }
2 }'

# --- idioms: forms that run and say the same thing the long way ------------
# `x = x <op> y` -> `x <op>= y`
expect_lint_warns "self add"  'mut i = 0
i = i + 1
inspect(i)'                                       "can be 'i += …'"
expect_lint_warns "self sub"  'mut i = 9
i = i - 1
inspect(i)'                                       "can be 'i -= …'"
expect_lint_warns "self mul"  'mut i = 2
i = i * 3
inspect(i)'                                       "can be 'i *= …'"
# Sound-negatives. A longer chain is NOT the compound form: `i = i - a + b`
# means `(i - a) + b`, while `i -= a + b` would subtract the sum.
expect_lint_clean "chain is not compound" 'mut i = 9
let a = 1
let b = 2
i = i - a + b
inspect(i)'
expect_lint_clean "already compound"  'mut i = 0
i += 1
inspect(i)'
expect_lint_clean "different name"    'mut i = 0
let j = 5
i = j + 1
inspect(i)'
expect_lint_clean "self on the right" 'mut i = 0
i = 1 + i
inspect(i)'

# `.size()` against 0 -> `.empty()`
expect_lint_warns "size eq zero"  'let xs = [1]
inspect(xs.size() == 0)'                          "use .empty()"
expect_lint_warns "size gt zero"  'let xs = [1]
inspect(xs.size() > 0)'                           "use !….empty()"
expect_lint_warns "size ne zero"  'let xs = [1]
inspect(xs.size() != 0)'                          "use !….empty()"
expect_lint_warns "zero eq size"  'let xs = [1]
inspect(0 == xs.size())'                          "use .empty()"
# Sound-negatives: another bound, a comparison the rewrite would not preserve,
# and a same-named method that takes an argument.
expect_lint_clean "size eq one"   'let xs = [1]
inspect(xs.size() == 1)'
expect_lint_clean "size ge zero"  'let xs = [1]
inspect(xs.size() >= 0)'
expect_lint_clean "count eq zero" 'let s = "ab"
inspect(s.count("a") == 0)'

# `range(0, n)` -> `range(n)`
expect_lint_warns "range zero start" 'inspect(range(0, 5).collect())' \
                                                  "range(0, n) is range(n)"
expect_lint_warns "range zero step"  'inspect(range(0, 9, step: 3).collect())' \
                                                  "range(0, n) is range(n)"
# Sound-negatives: a start that is not 0, and the single-argument form.
expect_lint_clean "range one start"  'inspect(range(1, 5).collect())'
expect_lint_clean "range single arg" 'inspect(range(5).collect())'

# --- non-exhaustive enum match (falls through to nil, not an error) --------
expect_lint_warns "missing nullary variant" \
'enum Shape { Circle(Float), Rect(Float, Float), Origin }
fn area(s) { match s { Circle(r) => r, Rect(w, h) => w * h } }
inspect(area(Shape.Circle(2.0)))' "doesn't handle: Origin"
expect_lint_warns "missing payload variant" \
'enum Shape { Circle(Float), Rect(Float, Float), Origin }
fn area(s) { match s { Circle(r) => r, x: Origin => 0.0 } }
inspect(area(Shape.Circle(2.0)))' "doesn't handle: Rect"
# A guarded arm may reject at runtime, so it does not count as covering the
# variant it names, even though the variant appears in the source.
expect_lint_warns "guarded arm does not count" \
'enum Shape { Circle(Float), Rect(Float, Float), Origin }
fn area(s) { match s { Circle(r) if r > 0.0 => r, Rect(w, h) => w * h, x: Origin => 0.0 } }
inspect(area(Shape.Circle(2.0)))' "doesn't handle: Circle"
# Sound-negatives: every variant named (ctor patterns + a type pattern for
# the nullary one), a bare `_` catch-all, and a type pattern naming the enum
# itself (matches every variant, same as a bare catch-all).
expect_lint_clean "all variants named" \
'enum Shape { Circle(Float), Rect(Float, Float), Origin }
fn area(s) { match s { Circle(r) => r, Rect(w, h) => w * h, x: Origin => 0.0 } }
inspect(area(Shape.Circle(2.0)))'
expect_lint_clean "wildcard catch-all" \
'enum Shape { Circle(Float), Rect(Float, Float), Origin }
fn area(s) { match s { Circle(r) => r, _ => 0.0 } }
inspect(area(Shape.Circle(2.0)))'
expect_lint_clean "enum-name type pattern is a catch-all" \
'enum Shape { Circle(Float), Rect(Float, Float), Origin }
fn area(s) { match s { Circle(r) => r, x: Shape => 0.0 } }
inspect(area(Shape.Circle(2.0)))'
# A variant name two enums in this file both declare is ambiguous when
# referenced unqualified — skip rather than risk a false positive (the
# runtime itself would match either enum'\''s instance by name alone).
expect_lint_clean "ambiguous unqualified variant name" \
'enum A { Foo(Long), Bar }
enum B { Foo(String) }
fn f(x) { match x { Foo(n) => n } }
inspect(f(A.Foo(1)))'
# The `Enum.Variant` qualifier disambiguates even when the bare variant name
# collides across enums in this file.
expect_lint_clean "qualified ctor disambiguates" \
'enum A { Foo(Long), Bar }
enum B { Foo(String) }
fn f(x) { match x { A.Foo(n) => n, x: Bar => 0 } }
inspect(f(A.Foo(1)))'
# No enum declared in the file at all: nothing to check against.
expect_lint_clean "no enum in file" \
'fn f(x) { match x { 1 => "one", _ => "other" } }
inspect(f(1))'

# Errors propagate through the same CLI with exit 2:
expect_lint_error "undefined var"      'fn f() { nope }'
expect_lint_error "break outside loop" 'fn f() { break }'

# --- effects / generators through the CLI ---
# The CLI must run its error checks over the *lowered* AST (what the backends
# run). On the raw parse, `effect fn` and `handle … with` bind names no
# analyzer knows, so operations, clause parameters and `resume` all read as
# undefined and every effects file fails to lint.
expect_lint_clean "effects tail-resumptive" 'effect fn ask()
inspect(handle { perform ask() } with ask(resume) { resume(1) })'
expect_lint_clean "effects effect fn body" 'effect fn ask()
effect fn double() { let n = perform ask(); n * 2 }
inspect(handle { double() } with ask(resume) { resume(21) })'
expect_lint_clean "effects multi-shot" 'effect fn choose(a, b)
inspect(handle { perform choose(1, 2) } with choose(a, b, resume) { [resume(a), resume(b)] })'
expect_lint_clean "effects return clause" 'effect fn get()
mut cell = 0
inspect(handle { perform get() } with get(resume) { resume(cell) } with return(v) { v + 1 })'
expect_lint_clean "generator yield" 'fn counter(n) { mut i = 0; while i < n { yield i; i += 1 } }
mut t = 0
for v in counter(3) { t += v }
inspect(t)'
# Advisory warnings run over the source as written, so bindings the lowering
# synthesizes are never reported: an abort clause deliberately never reads its
# `resume`, and a clause may ignore an operation argument.
expect_lint_clean "abort clause ignores resume" 'effect fn fail()
inspect(handle { perform fail(); "x" } with fail(resume) { "aborted" })'
expect_lint_clean "clause ignores an op arg" 'effect fn pair(x, y)
inspect(handle { perform pair(1, 2) } with pair(x, y, resume) { resume(x) })'
# The lowering rejects malformed effects by throwing; the CLI must turn that
# into an error diagnostic (exit 2) rather than aborting on uncaught exception.
expect_lint_error "two return clauses" 'effect fn a()
inspect(handle { perform a() } with a(r) { r(1) } with return(v) { v } with return(w) { w })'
expect_lint_error "duplicate clause" 'effect fn a()
inspect(handle { perform a() } with a(r) { r(1) } with a(r) { r(2) })'
# A real unused local elsewhere in the same file is still reported:
expect_lint_warns "unused local beside effects" 'effect fn ask()
fn helper() { let dead = 1; 2 }
inspect(handle { perform ask() } with ask(resume) { resume(helper()) })' "unused variable 'dead'"
# An `effect fn` body and each handler clause body are their own scopes, so an
# unused local written in one is reported (at its authored position) just like
# in any function.
expect_lint_warns "unused in effect fn body" 'effect fn ask()
effect fn work() { let dead = 1; perform ask() }
inspect(handle { work() } with ask(resume) { resume(2) })' "unused variable 'dead'"
expect_lint_warns "unused in handler clause body" 'effect fn ask()
inspect(handle { perform ask() } with ask(resume) { let dead = 1; resume(2) })' "unused variable 'dead'"
expect_lint_warns "unused in return clause body" 'effect fn ask()
inspect(handle { perform ask() } with ask(resume) { resume(2) } with return(v) { let dead = 1; v })' "unused variable 'dead'"
# Sound-negatives — locals that ARE used in these scopes must stay clean:
expect_lint_clean "used in effect fn body" 'effect fn ask()
effect fn work() { let n = perform ask(); n * 2 }
inspect(handle { work() } with ask(resume) { resume(2) })'
expect_lint_clean "used in handler clause body" 'effect fn ask()
inspect(handle { perform ask() } with ask(resume) { let doubled = 2 * 3; resume(doubled) })'
# A clause captures an enclosing local — that read must count, so no warning:
expect_lint_clean "clause captures enclosing local" 'effect fn ask()
fn f() { let base = 100; handle { perform ask() } with ask(resume) { resume(base) } }
inspect(f())'

# --- `culebra lint` surfaces ShadowError like the run path ---
# Shadowing an enclosing function's local is a pre-eval ShadowError when the
# file runs; the CLI must report it too, at the same position, not stay clean.
expect_lint_error "shadow in nested fn" 'fn outer() {
  let x = 1
  fn inner() { let x = 2; x }
  inner() + x
}
inspect(outer())'
expect_lint_error "shadow in handler clause" 'effect fn ask()
fn outer() {
  let x = 1
  handle { perform ask() } with ask(resume) { let x = 2; resume(x) }
  x
}
inspect(outer())'
# Sound-negative: a same name in sibling (non-nested) scopes is not shadowing.
expect_lint_clean "same name in sibling scopes" 'fn a() { let x = 1; x }
fn b() { let x = 2; x }
inspect(a() + b())'

# --- directory argument: recurse into `.cul` files, like `culebra fmt` ---
DIRTMP=$(mktemp -d)
mkdir -p "$DIRTMP/sub"
printf 'inspect(1)\n' > "$DIRTMP/clean.cul"
printf "import Math from 'std/math'\ninspect(1)\n" > "$DIRTMP/sub/dirty.cul"
out=$("$CULEBRA" lint "$DIRTMP" 2>&1); rc=$?
if [[ $rc -ne 1 || "$out" != *"dirty.cul"*"unused import 'Math'"* || "$out" == *"clean.cul"* ]]; then
  echo "FAIL lint-dir-recurse: rc=$rc out=$out"; fail=1
fi
rm -rf "$DIRTMP"

# --- `--fix`: mechanically remove unused-import lines ---
# A used import is left alone; an unused one is deleted and the file re-lints
# clean afterward. `--fix` reports the file it rewrote and exits 0.
FIXTMP=$(mktemp -d)
printf "import Math from 'std/math'\nimport IO from 'std/io'\n\nIO.inspect(Math.sqrt(4))\n" > "$FIXTMP/keep.cul"
out=$("$CULEBRA" lint --fix "$FIXTMP/keep.cul" 2>&1); rc=$?
if [[ $rc -ne 0 || -n "$out" ]]; then
  echo "FAIL lint-fix-noop-when-clean: rc=$rc out=$out"; fail=1
fi
if ! grep -q "^import Math" "$FIXTMP/keep.cul"; then
  echo "FAIL lint-fix-noop-when-clean: used import was removed"; fail=1
fi

printf "import Math from 'std/math'\nimport IO from 'std/io'\nimport Time from 'std/time'\n\nIO.inspect(1)\n" > "$FIXTMP/dead.cul"
out=$("$CULEBRA" lint --fix "$FIXTMP/dead.cul" 2>&1); rc=$?
if [[ $rc -ne 0 || "$out" != *"fixed 2 unused imports"* ]]; then
  echo "FAIL lint-fix-removes-unused: rc=$rc out=$out"; fail=1
fi
if grep -qE "^import (Math|Time)" "$FIXTMP/dead.cul"; then
  echo "FAIL lint-fix-removes-unused: a dead import survived: $(cat "$FIXTMP/dead.cul")"; fail=1
fi
if ! grep -q "^import IO" "$FIXTMP/dead.cul"; then
  echo "FAIL lint-fix-removes-unused: the live import was removed"; fail=1
fi
out=$("$CULEBRA" lint "$FIXTMP/dead.cul" 2>&1); rc=$?
if [[ $rc -ne 0 || -n "$out" ]]; then
  echo "FAIL lint-fix-removes-unused: file not clean after fix: rc=$rc out=$out"; fail=1
fi

# `--fix` deletes whole lines, so it must leave alone any dead import that
# shares its line with something else (the grammar allows `;`-joined
# statements). Deleting such a line drops the neighbour, and no re-parse check
# can catch it: the rest of the file still parses and lints just as cleanly.
# Three shapes, all of which must survive untouched:
#   a statement neighbour, a live import, a live import that shadows a builtin.
for shape in "import Math from 'std/math'; inspect(42)" \
             "import Alpha from './alpha'; import Beta from './beta'
inspect(Beta.x)" \
             "import Math from 'std/math'; import IO from 'std/io'
IO.inspect(1)"; do
  printf '%s\n' "$shape" > "$FIXTMP/shared.cul"
  before=$(cat "$FIXTMP/shared.cul")
  out=$("$CULEBRA" lint --fix "$FIXTMP/shared.cul" 2>&1); rc=$?
  if [[ "$(cat "$FIXTMP/shared.cul")" != "$before" ]]; then
    echo "FAIL lint-fix-shared-line: rewrote [$shape] -> [$(cat "$FIXTMP/shared.cul")]"; fail=1
  fi
  if [[ $rc -ne 1 || "$out" != *"skipped 1 unused import sharing a line"* ]]; then
    echo "FAIL lint-fix-shared-line: rc=$rc out=$out"; fail=1
  fi
done

# A write that fails must not be reported as a fix. (Skipped as root, which
# can write through a read-only mode.)
if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
  printf "import Math from 'std/math'\ninspect(1)\n" > "$FIXTMP/ro.cul"
  chmod 444 "$FIXTMP/ro.cul"
  out=$("$CULEBRA" lint --fix "$FIXTMP/ro.cul" 2>&1); rc=$?
  if [[ $rc -ne 2 || "$out" != *"can't write"* || "$out" == *"fixed 1 unused import"* ]]; then
    echo "FAIL lint-fix-unwritable: rc=$rc out=$out"; fail=1
  fi
  chmod 644 "$FIXTMP/ro.cul"
fi
rm -rf "$FIXTMP"

# A directory holding no .cul file is a mistake, not a clean run — reporting
# exit 0 here would make lint a silent no-op in a CI gate.
EMPTYTMP=$(mktemp -d)
out=$("$CULEBRA" lint "$EMPTYTMP" 2>&1); rc=$?
if [[ $rc -ne 2 ]]; then
  echo "FAIL lint-empty-dir: expected exit 2, got rc=$rc out=$out"; fail=1
fi
rm -rf "$EMPTYTMP"

# --- unknown flags are an error, not a file name ---
# `lint --fixx a.cul` used to report "can't open '--fixx'" and then lint a.cul
# anyway, so a typo looked like a successful run that just didn't fix anything.
printf "import Math from 'std/math'\ninspect(1)\n" > "$TMP/opt.cul"
for bad in --fixx -f --help=1; do
  out=$("$CULEBRA" lint "$bad" "$TMP/opt.cul" 2>&1); rc=$?
  if [[ $rc -ne 2 || "$out" != *"unknown option '$bad'"* || "$out" == *"unused import"* ]]; then
    echo "FAIL lint-unknown-option [$bad]: rc=$rc out=$out"; fail=1
  fi
done

if [[ $fail -eq 0 ]]; then echo "lint_test OK"; exit 0; fi
exit 1
