#!/usr/bin/env bash
# CULEBRA_JIT_CACHE — a program's compiled object, reached by two keys. The
# source key (the program's source and options, and this binary) skips the IR
# pipeline and the backend; the object key (the optimized module's text and
# the backend's identity) skips the backend alone, and holds for another
# build. Whichever answers, the run has to be the one a cold start gives:
# stdout, stderr and exit status.
#
# This can't be a tests/*.cul sweep test: it runs the same file twice against
# one cache directory, under two binaries, and reads what each run skipped.
#
# Usage: jit_cache_test.sh <path-to-culebra>
set -u

CULEBRA="${1:?usage: jit_cache_test.sh <culebra>}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export CULEBRA_REQUIRE_EXPLICIT_ENGINE=1

cat > "$TMP/a.cul" <<'EOF'
let xs = [1, 2, 3].map(|x| x * 2)
println("a: {xs}")
EOF

cat > "$TMP/b.cul" <<'EOF'
let xs = [1, 2, 3].map(|x| x + 1)
println("b: {xs}")
EOF

cat > "$TMP/throws.cul" <<'EOF'
println("before")
let f = fn (n) {
  throw ValueError("bad {n}") if n > 1
  n
}
f(1)
f(2)
println("after")
EOF

fail=0
bad() { echo "FAIL: $1" >&2; fail=1; }

# run <name> <cache> <args...>: stdout, stderr and status into $TMP/<name>.*
run() {
  local name="$1" cache="$2"; shift 2
  CULEBRA_JIT_CACHE="$cache" "$CULEBRA" "$@" > "$TMP/$name.out" 2> "$TMP/$name.err"
  echo "$?" > "$TMP/$name.rc"
}
same() {  # same <name1> <name2>: the two runs agree on all three
  cmp -s "$TMP/$1.out" "$TMP/$2.out" && cmp -s "$TMP/$1.err" "$TMP/$2.err" \
    && cmp -s "$TMP/$1.rc" "$TMP/$2.rc"
}
entries() { find "$1" -name '*.o' | wc -l | tr -d ' '; }

# --- a cold run stores one object; a warm run is the same run ---------------
run cold "$TMP/c" --jit "$TMP/a.cul"
[ "$(cat "$TMP/cold.out")" = "a: [2, 4, 6]" ] || bad "cold run printed: $(cat "$TMP/cold.out")"
[ "$(entries "$TMP/c")" = 1 ] || bad "a cold run left $(entries "$TMP/c") objects, want 1"
run warm "$TMP/c" --jit "$TMP/a.cul"
same cold warm || bad "a warm run differs from the cold one"
[ "$(entries "$TMP/c")" = 1 ] || bad "a warm run stored a second object"

# --- a hit runs neither the IR pipeline nor the backend ---------------------
CULEBRA_JIT_TIME_PASSES=1 run cold_t "$TMP/t" --jit "$TMP/a.cul"
CULEBRA_JIT_TIME_PASSES=1 run warm_t "$TMP/t" --jit "$TMP/a.cul"
grep -q '^\[jit-time\] optimize' "$TMP/cold_t.err" || bad "a cold run reported no optimize phase"
grep -q '^\[jit-time\] cached' "$TMP/warm_t.err" || bad "a warm run did not report the hit"
grep -q '^\[jit-time\] optimize' "$TMP/warm_t.err" && bad "a warm run ran the IR pipeline"
grep -q 'Pass execution timing report' "$TMP/warm_t.err" && bad "a warm run ran LLVM passes"

# --- the object is what runs: b's key holding a's object prints what a does --
run fill_a "$TMP/s" --jit "$TMP/a.cul"
a_obj="$(find "$TMP/s" -name '*.o')"
run fill_b "$TMP/s" --jit "$TMP/b.cul"
[ "$(cat "$TMP/fill_b.out")" = "b: [2, 3, 4]" ] || bad "b printed: $(cat "$TMP/fill_b.out")"
[ "$(entries "$TMP/s")" = 2 ] || bad "two programs left $(entries "$TMP/s") objects, want 2"
for o in "$TMP/s"/*.o; do
  [ "$o" = "$a_obj" ] || cp "$a_obj" "$o"
done
run swapped "$TMP/s" --jit "$TMP/b.cul"
[ "$(cat "$TMP/swapped.out")" = "a: [2, 4, 6]" ] || bad "a hit did not run the cached object: $(cat "$TMP/swapped.out")"

# --- another build: the same module to the same backend ---------------------
# The driver under a second path is another binary to the source key, and the
# same LLVM, CPU and settings to the object key: it optimizes, then links what
# the first one compiled.
OTHER="$TMP/culebra-other"
cp "$CULEBRA" "$OTHER"
CULEBRA_JIT_TIME_PASSES=1 run first_t "$TMP/o" --jit "$TMP/a.cul"
other() {  # other <name> <cache> <args...>: `run` under the second binary
  local name="$1" cache="$2"; shift 2
  CULEBRA_JIT_CACHE="$cache" "$OTHER" "$@" > "$TMP/$name.out" 2> "$TMP/$name.err"
  echo "$?" > "$TMP/$name.rc"
}
CULEBRA_JIT_TIME_PASSES=1 other other_t "$TMP/o" --jit "$TMP/a.cul"
grep -q '^\[jit-time\] optimize' "$TMP/other_t.err" || bad "another binary skipped the IR pipeline on the first binary's word"
grep -q '^\[jit-time\] cached' "$TMP/other_t.err" || bad "another binary compiled a module the cache held"
[ "$(entries "$TMP/o")" = 1 ] || bad "two binaries stored $(entries "$TMP/o") objects for one module, want 1"
cmp -s "$TMP/first_t.out" "$TMP/other_t.out" || bad "another binary's run printed something else"
CULEBRA_JIT_TIME_PASSES=1 other other_again "$TMP/o" --jit "$TMP/a.cul"
grep -q '^\[jit-time\] optimize' "$TMP/other_again.err" && bad "a binary ran the IR pipeline on its own second run"
other other_throw "$TMP/x2" --jit "$TMP/throws.cul"
run first_throw "$TMP/x2" --jit "$TMP/throws.cul"
same other_throw first_throw || bad "an uncaught throw reports differently off another build's object"

# --- an uncaught throw reports the same on a hit ----------------------------
run throw_cold "$TMP/x" --jit "$TMP/throws.cul"
run throw_warm "$TMP/x" --jit "$TMP/throws.cul"
[ "$(cat "$TMP/throw_cold.rc")" != 0 ] || bad "the throwing program exited 0"
grep -q 'ValueError' "$TMP/throw_cold.err" || bad "the cold throw named no ValueError"
same throw_cold throw_warm || bad "an uncaught throw reports differently on a hit"
run throw_vm "" --vm "$TMP/throws.cul"
same throw_vm throw_warm || bad "a hit's uncaught throw differs from the executor's"

# --- the options are part of the key ----------------------------------------
run o0 "$TMP/c" --jit -O0 "$TMP/a.cul"
run fast "$TMP/c" --jit-faststart "$TMP/a.cul"
[ "$(entries "$TMP/c")" = 3 ] || bad "-O0 and --jit-faststart share an entry: $(entries "$TMP/c") objects, want 3"
cmp -s "$TMP/o0.out" "$TMP/cold.out" && cmp -s "$TMP/fast.out" "$TMP/cold.out" \
  || bad "-O0 or --jit-faststart printed something else"

# --- --emit-llvm prints the pipeline's output, cache or no cache ------------
run emit "$TMP/c" --jit --emit-llvm "$TMP/a.cul"
grep -q '^define ' "$TMP/emit.out" || bad "--emit-llvm printed no IR against a warm cache"

[ "$fail" = 0 ] && echo "jit_cache_test OK"
exit "$fail"
