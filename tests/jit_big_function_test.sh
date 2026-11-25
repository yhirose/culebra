#!/usr/bin/env bash
# The JIT compiles a function past kMaxOptimizedBlocks (include/jit/jit.h)
# unoptimised: `optnone` + `noinline`, so neither the IR pipeline nor the
# backend spends superlinear time on it. A program that only computes the same
# answer either way cannot see the cut go, so this reads the IR — as do the
# checks below on what a long function's blocks are made of.
# Usage: <path-to-culebra>
set -u
CULEBRA="${1:?usage: jit_big_function_test.sh <culebra-binary>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0
LIMIT=4096

emit() { CULEBRA_JIT_CACHE=off "$CULEBRA" --jit "$@" --emit-llvm "$TMP/p.cul" 2>/dev/null; }

# Each fixture is compiled once per level, with CULEBRA_JIT_BLOCK_STATS on:
# <stem>.ll is the IR (-O0 prints it as lowered), <stem>.stats the tables
# and <stem>.counts each function's lowered blocks, by name.
compile() { # <file> <-O level> <stem>
  CULEBRA_JIT_CACHE=off CULEBRA_JIT_BLOCK_STATS=1 "$CULEBRA" --jit "$2" \
    --emit-llvm "$1" > "$TMP/$3.ll" 2> "$TMP/$3.stats" ||
    { echo "FAIL: --jit $2 --emit-llvm ${1##*/} exited $?"; fail=1; }
  awk '/^\[jit-blocks\] [^ ]+ .*: [0-9]+ blocks/ { for (i = 1; i <= NF; i++)
    if ($i == "blocks,") print $2, $(i - 1) }' "$TMP/$3.stats" |
    sort > "$TMP/$3.counts"
}
blocks_of() { # <stem> <name>
  awk -v n="$2" '$1 == n { print $2 }' "$TMP/$1.counts"
}

# The functions of an -O0 IR file with more than LIMIT blocks.
big_names() { # <ll>
  awk -v min="$LIMIT" '
    /^define/ { n = $0; sub(/\(.*/, "", n); sub(/.*@/, "", n); b = 0; f = 1; next }
    f && /^}/ { if (b > min) print n; f = 0; next }
    f && /^[A-Za-z0-9_.$"-]+:/ { b++ }' "$1"
}

# The attributes the optimised module gives function `name`.
attrs_of() {
  awk -v name="$1" '
    $0 ~ "^define .*@" name "\\(" { split($0, w, "#"); g = w[2]; sub(/[^0-9].*/, "", g) }
    /^attributes #/ { a = $2; sub(/#/, "", a); attrs[a] = $0 }
    END { print attrs[g] }' "$TMP/opt.ll"
}

# Function `name` in IR file `ll`: its marked pad release tests and its
# `rc.release` blocks.
fn_of() { # <name> <ll>: the function's own lines
  sed -n "/^define .*@$1(/,/^}/p" "$2"
}
pad_tests_of() { # <name> <ll>
  fn_of "$1" "$2" > "$TMP/fn.ll"
  echo "$(grep -c '!culebra[.]pad_release_test' "$TMP/fn.ll")" \
    "$(grep -c '^rc[.]release[0-9]*:' "$TMP/fn.ll")"
}

# tests/test_jit_big_function.cul holds one function past the cut.
compile "$HERE/test_jit_big_function.cul" -O0 lowered
compile "$HERE/test_jit_big_function.cul" -O2 opt
names=$(big_names "$TMP/lowered.ll")
if [[ -z "$names" ]]; then
  echo "FAIL: no function past $LIMIT blocks in test_jit_big_function.cul"
  fail=1
fi
for n in $names; do
  a=$(attrs_of "$n")
  if [[ "$a" != *optnone* || "$a" != *noinline* ]]; then
    echo "FAIL: $n (past $LIMIT blocks) is not optnone + noinline: $a"
    fail=1
  fi
done

# Past the cut a function is reached through its code pointer alone
# (JIT::reach_through_code_pointer), so its code can be replaced while the
# program runs: the pointer's initializer is the one reference left to it.
for n in $names; do
  refs=$(grep -E "@$n([^.A-Za-z0-9_]|\$)" "$TMP/opt.ll" | grep -v '^define ')
  only_init="^@$n[.]code = [a-z_ ]*global ptr @$n\$"
  if [[ ! "$refs" =~ $only_init ]]; then
    echo "FAIL: $n (past $LIMIT blocks) is reached other than through" \
      "@$n.code: $refs"
    fail=1
  fi
done

# Its call counter (JIT::count_calls) leaves every alloca in the entry block:
# one after it is a dynamic allocation, made again on every call.
for n in $names; do
  late=$(fn_of "$n" "$TMP/opt.ll" |
    awk '/^[A-Za-z0-9_.$"-]+:/ { b++ } b > 1 && / alloca / { c++ } END { print c + 0 }')
  if (( late > 0 )); then
    echo "FAIL: $n (past $LIMIT blocks) has $late allocas past its entry block"
    fail=1
  fi
done

# A function past the cut the program calls often is optimized again while it
# runs (JIT::Reoptimizer). `wait` compiles it on the call that reaches the
# count, so from the second call on test_jit_big_function.cul runs the
# optimized code, throws and drops included, and its asserts still hold.
reopt() { # <mode> <cache> <program>: the run's stderr
  CULEBRA_JIT_CACHE="$2" CULEBRA_JIT_TIME_PASSES=1 CULEBRA_JIT_REOPTIMIZE="$1" \
    CULEBRA_JIT_REOPTIMIZE_CALLS=1 "$CULEBRA" --jit "$3" 2>&1 > /dev/null
}
expect_reopt() { # <what> <exit> <line>: of the run in $out and $rc
  if (( rc != $2 )) || ! grep -q "$3" <<< "$out" ||
    grep -q 'could not optimize' <<< "$out"; then
    echo "FAIL: $1 (exit $rc, expected $2):"
    grep -v '^ ' <<< "$out" | grep -E 'reopt|culebra:|Error' | head -5
    fail=1
  fi
}
big="$HERE/test_jit_big_function.cul"
# A source-key hit of the object cache links an earlier run's object, and
# optimizes again from the snapshots that run kept beside it.
mkdir "$TMP/cache"
for run in miss hit; do
  out=$(reopt wait "$TMP/cache" "$big")
  rc=$?
  for n in $names; do
    expect_reopt "$n optimized again under wait, cache $run" 0 \
      "^\[jit-time\] reopt .* $n\$"
  done
done
if reopt off "$TMP/cache" "$big" | grep -q '^\[jit-time\] reopt '; then
  echo "FAIL: CULEBRA_JIT_REOPTIMIZE=off optimized a function again"
  fail=1
fi
# The default compiles on a thread of its own while the program goes on. A
# run that ends first, by returning or by Sys.exit (which does not unwind),
# stops it: what is under way finishes or is cancelled, and says which.
out=$(reopt "" "$TMP/cache" "$big")
rc=$?
expect_reopt "the default, the program returning" 0 '^\[jit-time\] reopt '
cp "$big" "$TMP/exits.cul"
echo 'Sys.exit(3)' >> "$TMP/exits.cul"
out=$(reopt "" off "$TMP/exits.cul")
rc=$?
expect_reopt "the default, the program calling Sys.exit" 3 '^\[jit-time\] reopt '

# Past the cut a cleanup pad's release is the helper call alone
# (settle_pad_release_tests): each inline tag test the lowering marked as a
# pad's (-O0 prints the marks) takes its `rc.release` block with it, and no
# mark reaches the pipeline.
for n in $names; do
  read -r marks before < <(pad_tests_of "$n" "$TMP/lowered.ll")
  read -r _ after < <(pad_tests_of "$n" "$TMP/opt.ll")
  if (( marks == 0 || before - after < marks )); then
    echo "FAIL: $n (past $LIMIT blocks) kept its pads' release tests:" \
      "$marks marked, rc.release $before -> $after"
    fail=1
  fi
done
if grep -q 'culebra[.]pad_release_test' "$TMP/opt.ll"; then
  echo "FAIL: a pad release test's mark reached the pipeline"
  fail=1
fi

# Past the cut the Float copies of arith runs (Lowering::arith_runs) go
# before the count, as nothing would fold them. CULEBRA_JIT_BLOCK_STATS
# counts the lowering before that, so the function -O2 lowers must be bigger
# than the one -O0 does (it had copies to drop) and no guard may be left.
for n in $names; do
  o0=$(blocks_of lowered "$n")
  o2=$(blocks_of opt "$n")
  if (( o2 <= o0 )); then
    echo "FAIL: $n was lowered with no arith run to drop ($o0 -> $o2 blocks)"
    fail=1
  fi
  if fn_of "$n" "$TMP/opt.ll" | grep -q 'arith[.]is_float'; then
    echo "FAIL: $n (past $LIMIT blocks) kept a Float copy's guard"
    fail=1
  fi
done

# A function that only its Float copies take past the cut is counted
# without them (JIT::arith_versions), so it keeps them and their guards.
compile "$HERE/jit_big_function/near_cut.cul" -O0 near0
compile "$HERE/jit_big_function/near_cut.cul" -O2 near2
kept=$(join "$TMP/near0.counts" "$TMP/near2.counts" |
  awk -v min="$LIMIT" '$2 <= min && $3 > min { print $1 }')
if [[ -z "$kept" ]]; then
  echo "FAIL: no function in jit_big_function/near_cut.cul that only its" \
    "Float copies take past $LIMIT blocks"
  fail=1
fi
for n in $kept; do
  if ! fn_of "$n" "$TMP/near2.ll" | grep -q 'arith[.]is_float'; then
    echo "FAIL: $n (past $LIMIT blocks only with its Float copies) lost them"
    fail=1
  fi
done

# A small program keeps the whole pipeline.
printf 'fn f(x) { x > 1 ? x * 2 : x + 1 }\nprintln(f(3))\n' > "$TMP/p.cul"
if emit -O2 | grep -q optnone; then
  echo "FAIL: a small program was compiled optnone"
  fail=1
fi

# CULEBRA_JIT_BLOCK_STATS=<n> says which part of the lowering made a long
# function's blocks (docs/internals/vm.md §7). It counts the IR as lowered,
# which -O0 prints as it is, and gives every block an owner.
awk '
  /^define/ { n = $0; sub(/\(.*/, "", n); sub(/.*@/, "", n); b = 0; f = 1; next }
  f && /^}/ { print n, b; f = 0; next }
  f && /^[A-Za-z0-9_.$"-]+:/ { b++ }' "$TMP/lowered.ll" | sort > "$TMP/ir_counts"
if [[ ! -s "$TMP/lowered.counts" ]] ||
  ! diff "$TMP/ir_counts" "$TMP/lowered.counts"; then
  echo "FAIL: CULEBRA_JIT_BLOCK_STATS counts differ from the -O0 IR's"
  fail=1
fi
if grep -qE '^\[jit-blocks\] +[0-9]+ +[0-9.]+% +(-|[0-9]+) +\? ' "$TMP/lowered.stats"; then
  echo "FAIL: CULEBRA_JIT_BLOCK_STATS left a block without an owner"
  fail=1
fi

# A property read as a value (PropVal) is an own-field read and one slow
# path, without the proto arm and the binding diamond a call's read has.
printf '%s\n' 'fn f(a, b) {' '  let x = a.p * b + a.q * b' '  [x, a].size() + x' \
  '}' 'println(f({p: 1.5, q: 2.5}, 2.0))' > "$TMP/p.cul"
pv_row=$(CULEBRA_JIT_CACHE=off CULEBRA_JIT_BLOCK_STATS=1 "$CULEBRA" --jit -O0 \
  --emit-llvm "$TMP/p.cul" 2>&1 > /dev/null |
  awk '/^\[jit-blocks\] __vm_fn_1 / { f = 1; next } /^\[jit-blocks\] __/ { f = 0 }
       f && / PropVal / { print }')
if [[ "$pv_row" != *pval.* || "$pv_row" == *bind.* ]]; then
  echo "FAIL: a property read as a value has the call read's shape: $pv_row"
  fail=1
fi

# A function under the cut keeps the inline test in its pads, as marked:
# without it the register allocator spilled more at the head of the
# function's loops (docs/internals/vm.md §7).
emit -O0 > "$TMP/small.ll"
read -r marks _ < <(pad_tests_of __vm_fn_1 "$TMP/small.ll")
if (( marks == 0 )); then
  echo "FAIL: a pad of a function under the cut releases without the inline test"
  fail=1
fi

# A cell opens a cleanup segment in every open scope, and the segments of one
# scope share one release ladder (Lowering's emit_shared): the unwind code of
# a top level, and of a block in it, grows with its cells, not with their
# square. Doubling the cells about doubles main's releases; one ladder per
# segment made it nearly four times (a large program's main once carried
# 400k of them).
cells() { # <n>: main's release calls, for n captured bindings at each level
  local n=$1 i
  {
    for ((i = 1; i <= n; i++)); do printf 'let a%d = [%d]\nfn f%d() { a%d }\n' $i $i $i $i; done
    echo 'if a1.size() > 0 {'
    for ((i = 1; i <= n; i++)); do printf '  let b%d = [%d]\n  let g%d = fn () { b%d }\n' $i $i $i $i; done
    printf '  println(g1()[0] + f%d()[0])\n}\n' "$n"
  } > "$TMP/p.cul"
  emit -O0 > "$TMP/cells.ll"
  fn_of __culebra_main "$TMP/cells.ll" |
    grep -cE 'call void @culebra_runtime_(cell|value)_release'
}
r40=$(cells 40)
r80=$(cells 80)
if (( r40 == 0 || r80 == 0 || r80 * 4 > r40 * 10 )); then
  echo "FAIL: main's unwind releases grow faster than its cells:" \
    "$r40 for 40 a level, $r80 for 80"
  fail=1
fi

[[ $fail -eq 0 ]] && echo "jit_big_function_test: OK"
exit $fail
