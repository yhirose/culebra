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

if [[ $fail -eq 0 ]]; then echo "lint_test OK"; exit 0; fi
exit 1
