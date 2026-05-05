#!/usr/bin/env bash
#
# Shadow-prohibition negative tests. Each case is a Culebra snippet that
# is expected to fail with a "cannot shadow" error on both the tree
# interpreter and the JIT backend.

set -u

CULEBRA=${CULEBRA:-./build/culebra}

passed=0
failed=0

check() {
    local name="$1"
    local code="$2"
    local expected="$3"
    local only="${4:-both}"   # "interp" skips JIT; default runs both
    local tmp="./build/.shadow_test_$$.cul"
    printf '%s\n' "$code" > "$tmp"
    for mode in "" "--jit"; do
        local label="interp"
        [ -n "$mode" ] && label="jit"
        if [ "$only" = "interp" ] && [ "$label" = "jit" ]; then
            continue
        fi
        local out
        out=$("$CULEBRA" $mode "$tmp" 2>&1 || true)
        if echo "$out" | grep -q "$expected"; then
            echo "  PASS [$label] $name"
            passed=$((passed+1))
        else
            echo "  FAIL [$label] $name"
            echo "    expected: $expected"
            echo "    got:      $out"
            failed=$((failed+1))
        fi
    done
    rm -f "$tmp"
}

echo "== shadow prohibition negative tests =="

check "mut shadow of captured var" \
    'make_bumper = fn () {
       mut count = 0
       bump = fn () { mut count = 5 }
       bump()
     }
     make_bumper()' \
    "cannot shadow"

check "let shadow of captured var" \
    'outer_fn = fn () {
       mut x = 0
       inner = fn () { let x = 5 }
       inner()
     }
     outer_fn()' \
    "cannot shadow"

check "let mut shadow of captured var" \
    'f = fn () {
       mut y = 1
       g = fn () { let mut y = 2 }
       g()
     }
     f()' \
    "cannot shadow"

check "parameter shadow of captured var" \
    'f = fn () {
       mut a = 10
       inner = fn (a) { a + 1 }
       inner(5)
     }
     f()' \
    "cannot shadow"

check "match pattern shadow of captured var" \
    'f = fn () {
       mut n = 10
       describe = fn (v) {
         match v {
           n: Long => n * 2
         }
       }
       describe(5)
     }
     f()' \
    "cannot shadow"

check "for-in binding shadow of captured var" \
    'f = fn () {
       mut x = 0
       inner = fn () { for x in [1, 2] { puts(x) } }
       inner()
     }
     f()' \
    "cannot shadow"

check "drop must be a Function" \
    'let _ = { drop: 42 }' \
    "'drop' must be a Function"

check "drop must take zero arguments" \
    'let _ = { drop: fn (x) { x } }' \
    "'drop' must be a Function"

echo "-- summary: $passed passed, $failed failed --"
exit $failed
