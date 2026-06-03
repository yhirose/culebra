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

if [[ $fail -eq 0 ]]; then echo "lint_test OK"; exit 0; fi
exit 1
