#!/usr/bin/env bash
# Entry-block alloca gate (JIT + VM lowering codegen).
#
# LLVM only folds an `alloca` into the frame layout when it is a constant size
# AND sits in the function's entry block (AllocaInst::isStaticAlloca). One left
# in an ordinary block is lowered as a dynamic stack bump that is not undone
# until the function returns — so a loop body containing it grows the stack
# every pass and eventually SIGSEGVs, with no leak gate and no test able to see
# it short of running millions of rounds.
#
# Both codegens therefore build their scratch slots through an entry-block
# builder (`entryB` in jit.h, `eb` in vm.h). Two real crashes came from sites
# that missed it: the class meta's method slab (`class` declared in a loop) and
# the compound / `??=` index read-back slots (`a[i] += 1` in a loop).
#
# Checked over emitted IR rather than the source, so it holds for whichever
# emitter produced the alloca and covers both lanes at once.
set -euo pipefail
cd "$(dirname "$0")/.."

BIN="${1:-build-dev/culebra}"
if [[ ! -x "$BIN" ]]; then
  echo "alloca-discipline FAIL: culebra binary not found at $BIN" >&2
  exit 2
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Probe: the shapes whose emitters allocate scratch slots, each inside a loop
# body so a stray alloca lands outside the entry block. Kept to constructs both
# lanes compile, since the VM lowering is scanned from the same file.
cat > "$TMP/probe.cul" <<'CUL'
fn helper(x) {
  x + 1
}
fn shapes(n) {
  mut a = [0, nil, 1]
  mut o = {'k': 0, 'n': nil}
  mut acc = 0
  mut i = 0
  while i < n {
    class Local {
      one() { 1 }
      two() { 2 }
    }
    a[0] += 1
    a[1] ??= 5
    a[1] = nil
    o.k += 1
    o.n ??= 7
    o.n = nil
    acc += helper(i) + a[0] + Local.new().one()
    let r = 0..3
    acc += a[r].size()
    let t = (i, i)
    let m = match t {
      (x, y) => x + y
    }
    acc += m + Math.abs(-1)
    i += 1
  }
  acc
}
println(shapes(2))
CUL

scan() {  # $1 = lane flag, $2 = label
  local ir="$TMP/${2}.ll"
  if ! "$BIN" "$1" --emit-llvm "$TMP/probe.cul" > "$ir" 2>"$TMP/$2.err"; then
    echo "alloca-discipline FAIL: $2 could not emit IR" >&2
    sed 's/^/  /' "$TMP/$2.err" >&2
    exit 1
  fi
  awk -v lane="$2" '
    /^define / { infn = 1; entry = 1; first = 1; next }
    !infn { next }
    /^}/ { infn = 0; next }
    /^[ \t]*$/ { next }
    # The first label after `define` names the entry block itself; every later
    # one opens a block the frame does not own.
    /^[-A-Za-z0-9_.$]+:/ { if (first) first = 0; else entry = 0; next }
    { first = 0 }
    / = alloca / && !entry { printf "  %s: %s\n", lane, $0; bad++ }
    END { exit(bad ? 1 : 0) }
  ' "$ir" && return 0
  return 1
}

fail=0
scan --jit jit || fail=1
scan --vm-llvm vm || fail=1
if (( fail )); then
  echo "alloca-discipline FAIL: alloca outside the entry block (see above)." >&2
  echo "  Build scratch slots with the entry-block IRBuilder (entryB / eb)." >&2
  exit 1
fi
echo "alloca-discipline OK (jit + vm lowering: every alloca is entry-block)"
