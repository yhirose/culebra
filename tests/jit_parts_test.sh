#!/usr/bin/env bash
# --jit compiles a large program as several objects side by side
# (JIT::split_into_parts). CULEBRA_JIT_PART_SIZE=1 makes a small program such
# a one, and the run has to be the one the whole module gives: stdout, stderr
# and exit status — cold, from the object cache by either key, and with a
# function past the block cut optimized again.
#
# Usage: jit_parts_test.sh <path-to-culebra>
set -u

CULEBRA="${1:?usage: jit_parts_test.sh <culebra>}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export CULEBRA_REQUIRE_EXPLICIT_ENGINE=1

# What crosses a part: calls and function values, strings and inline caches,
# globals written from one function and read in another, a throw unwinding
# through frames of several functions with their defers and drops, a
# generator, a class and an isolate.
cat > "$TMP/a.cul" <<'EOF'
let mut count = 0

fn leaf(n) {
  throw "bad {n}" if n > 3
  count += 1
  "leaf {n}"
}

fn middle(n) {
  defer {
    println("leaving middle {n}")
  }
  leaf(n)
}

class Point {
  new(x) {
    self.x = x
  }
  twice() { self.x * 2 }
}

class Noisy {
  drop() { println("dropped") }
}

fn naturals(n) {
  for i in range(n) {
    yield i
  }
}

fn top(n) {
  let held = Noisy()
  middle(n)
}

for i in range(6) {
  try {
    println(top(i))
  } catch e {
    println("caught: {e}")
  }
}
println(count)
println([1, 2, 3].map(|x| Point(x).twice()))
for v in naturals(3) {
  println(v)
}
let worker = Isolate.spawn(fn () { [4, 5].map(|x| Point(x).twice()) })
println(worker.join())
leaf(9)
EOF

# A function past the block cut, called often enough to be optimized again:
# its snapshot names the symbols of a program that is several objects.
{
  echo 'let mut total = 0'
  echo 'fn long(n) {'
  echo '  let mut s = n'
  for i in $(seq 1 100); do  # some 6000 blocks: past 4096, under 16384
    echo "  if s > $i { s -= 1 } else { s += 2 }"
  done
  echo '  total += 1'
  echo '  s'
  echo '}'
  echo 'fn drive() {'
  echo '  let mut acc = 0'
  echo '  for i in range(4000) { acc += long(i) }'
  echo '  acc'
  echo '}'
  echo 'println(drive())'
  echo 'println(total)'
} > "$TMP/long.cul"

fail=0
bad() { echo "FAIL: $1" >&2; fail=1; }

# run <name> <part size> <cache> <file> [env...]
run() {
  local name="$1" size="$2" cache="$3" file="$4"; shift 4
  env CULEBRA_JIT_PART_SIZE="$size" CULEBRA_JIT_CACHE="$cache" "$@" \
    "$CULEBRA" --jit "$file" > "$TMP/$name.out" 2> "$TMP/$name.err"
  echo "$?" > "$TMP/$name.rc"
}
same() {  # the two runs agree on all three
  local part
  for part in out rc; do
    cmp -s "$TMP/$1.$part" "$TMP/$2.$part" ||
      bad "$2 differs from $1 in $part: $(head -c 400 "$TMP/$2.$part") $(head -c 400 "$TMP/$2.err")"
  done
  grep -v '^\[jit-time\]' "$TMP/$2.err" | grep -v '^$' > "$TMP/$2.said"
  cmp -s "$TMP/$1.err" "$TMP/$2.said" ||
    bad "$2 differs from $1 in err: $(head -c 400 "$TMP/$2.said")"
}
# A timed run also prints LLVM's reports: stdout and status are what to compare.
agrees() {
  cmp -s "$TMP/$1.out" "$TMP/$2.out" && cmp -s "$TMP/$1.rc" "$TMP/$2.rc" ||
    bad "$2 differs from $1: $(head -c 400 "$TMP/$2.out")"
}

HUGE=1000000000
for p in a long; do
  run "$p.whole" $HUGE off "$TMP/$p.cul"
  run "$p.parts" 1 off "$TMP/$p.cul"
  same "$p.whole" "$p.parts"
  run "$p.wait" 1 off "$TMP/$p.cul" CULEBRA_JIT_REOPTIMIZE=wait
  same "$p.whole" "$p.wait"
done
run reopt 1 off "$TMP/long.cul" CULEBRA_JIT_REOPTIMIZE=wait \
  CULEBRA_JIT_TIME_PASSES=1
grep -q '^\[jit-time\] reopt ' "$TMP/reopt.err" ||
  bad "long was not optimized again"

# The parts are really parts: the timings name the split.
run split 1 off "$TMP/a.cul" CULEBRA_JIT_TIME_PASSES=1
grep -q '^\[jit-time\] split ' "$TMP/split.err" || bad "a.cul was not split"

# The cache: cold, then the source key (no pipeline), then the object keys
# alone (the .ref entries removed), and a part evicted.
for p in a long; do
  cache="$TMP/cache-$p"
  run "$p.cold" 1 "$cache" "$TMP/$p.cul"
  same "$p.whole" "$p.cold"
  run "$p.source" 1 "$cache" "$TMP/$p.cul" CULEBRA_JIT_TIME_PASSES=1 \
    CULEBRA_JIT_REOPTIMIZE=wait
  agrees "$p.whole" "$p.source"
  grep -q '^\[jit-time\] split ' "$TMP/$p.source.err" &&
    bad "$p: the source key did not answer"
  rm -f "$cache"/*.ref
  run "$p.object" 1 "$cache" "$TMP/$p.cul" CULEBRA_JIT_TIME_PASSES=1
  agrees "$p.whole" "$p.object"
  grep -q '^\[jit-time\] cached ' "$TMP/$p.object.err" ||
    bad "$p: the object keys did not answer"
  rm -f "$(ls "$cache"/*.o | head -1)"
  run "$p.evicted" 1 "$cache" "$TMP/$p.cul"
  same "$p.whole" "$p.evicted"
done

[ "$fail" = 0 ] && echo "jit parts: ok"
exit "$fail"
