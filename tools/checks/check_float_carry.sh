#!/usr/bin/env bash
# Float-carry gate (the bytecode VM's LLVM lowering = `--jit`).
#
# A loop-carried Float travels as the i64 payload of its Value, so the phi
# that carries it is an i64 whose incoming edges are `bitcast double` and
# whose uses bitcast it straight back — a GPR<->FPR move each way, every
# iteration, on the loop's critical path. LLVM's own fold declines on these
# loops (the emitter hands the same bitcast to a second phi, breaking its
# one-use rule), and JIT::PromoteFloatPhis (jit.h, docs/internals/vm.md §7)
# retypes the whole phi web to double instead. Losing it costs nothing that
# any test can see: no error, no warning, the scalar loop just runs a third
# slower — which is exactly how the LLVM-side gap went unnoticed.
#
# So the shape is pinned here in the emitted IR. A phi is one the pass owes
# a double when every edge into it already carries one — a bitcast of a
# double, a constant read bit for bit as one, or another such phi, which is
# the pass's own precondition — and no phi like that may still be read back
# through a `bitcast i64 ... to double`. A phi whose payload arrives from
# anywhere else (a field load, a call's return) is not the pass's business
# and is left alone, so an emitter change elsewhere cannot turn this red by
# fair means.
#
# Each row is compiled on its own, and the row's busiest function must still
# hold a double phi per Float it carries. That is the second half: a value
# that falls out of registers altogether leaves no phi to read, so the check
# above would go quiet rather than red.
set -euo pipefail
cd "$(dirname "$0")/../.."

BIN="${1:-build-dev/culebra}"
if [[ ! -x "$BIN" ]]; then
  echo "float-carry FAIL: culebra binary not found at $BIN" >&2
  exit 2
fi

TMP="$(mktemp -d "${TMPDIR:-/tmp}/culebra-float-carry.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# The probes are the scalar and Vector2 rows of tools/bench/vector_loop.cul,
# copied rather than run: a counted `for` carrying Floats across the back
# edge, constants captured from the enclosing scope, a `let` inside an `if`
# arm, a unary minus, and the `@value` fields of a Vector2 held in frame
# slots. Each is a hole the scalar-floor work closed; each puts a carried
# Float through a phi. check_early_ifcvt.sh keeps its own copy of the scalar
# row rather than sharing this one, so that a row added here for this gate
# cannot break the shape that one requires.
cat > "$TMP/consts.cul" <<'CUL'
let DT = 0.016
let GX = 0.5
let GY = -9.8
let MAXSPEED = 40.0
let BOUNCE = 0.8
CUL

cat "$TMP/consts.cul" > "$TMP/scalars.cul"
cat >> "$TMP/scalars.cul" <<'CUL'

let scalars = fn (n) {
  let mut px = 0.0
  let mut py = 100.0
  let mut vx = 3.0
  let mut vy = 0.0
  for _ in 0..n {
    vx += GX * DT
    vy += GY * DT
    let speed = Math.sqrt(vx * vx + vy * vy)
    if speed > MAXSPEED {
      let k = MAXSPEED / speed
      vx *= k
      vy *= k
    }
    px += vx * DT
    py += vy * DT
    if py < 0.0 {
      py = 0.0
      vy = -vy * BOUNCE
    }
  }
  px + py + vx + vy
}

println(scalars(1000))
CUL

cat "$TMP/consts.cul" > "$TMP/vector2.cul"
cat >> "$TMP/vector2.cul" <<'CUL'

let vec_operators = fn (n) {
  let g = Vector2.new(GX, GY)
  let mut p = Vector2.new(0.0, 100.0)
  let mut v = Vector2.new(3.0, 0.0)
  for _ in 0..n {
    v += g * DT
    let speed = v.length()
    if speed > MAXSPEED {
      v *= MAXSPEED / speed
    }
    p += v * DT
    if p.y < 0.0 {
      p = Vector2.new(p.x, 0.0)
      v = Vector2.new(v.x, -v.y * BOUNCE)
    }
  }
  p.x + p.y + v.x + v.y
}

println(vec_operators(1000))
CUL

# A third row for the plainest shape there is: a `while` in a function
# carrying one Float. Its two cold uses (the return store and an unwind
# release) once outvoted the loop's own bitcast in the pass's own cost model,
# and this loop ran four times slower than the same loop written with `for`.
cat > "$TMP/while_one.cul" <<'CUL'
fn drift(n) {
  mut acc = 0.0
  mut i = 0
  while i < n {
    acc += 1.5
    i += 1
  }
  acc
}

println(drift(1000))
CUL

# The first two rows carry four Floats (px/py/vx/vy; the x and y of `p` and
# `v`); while_one carries the one its name says.
MIN_ROW_PHIS=4

scan() {  # $1 = probe basename, $2 = double phis its busiest function needs
  local min_row_phis="${2:-$MIN_ROW_PHIS}"
  local ir="$TMP/$1.ll"
  if ! "$BIN" --jit --emit-llvm "$TMP/$1.cul" > "$ir" 2>"$TMP/$1.err"; then
    echo "  $1: could not emit IR" >&2
    sed 's/^/    /' "$TMP/$1.err" >&2
    return 1
  fi
  awk -v probe="$1" -v min_row_phis="$min_row_phis" '
    # The phis the pass owes a double: every edge into one already carries
    # a double, counting the phis that meet the same test. Start with all of
    # them and drop those an edge disqualifies until the set holds still.
    function settle(   p, i, t, ok, changed) {
      for (p in ninc) owed[p] = 1
      do {
        changed = 0
        for (p in owed) {
          ok = 1
          for (i = 1; i <= ninc[p]; i++) {
            t = inc[p, i]
            if (t ~ /^%/ && !(t in bits) && !(t in owed)) { ok = 0; break }
          }
          if (!ok) { drop[p] = 1; changed = 1 }
        }
        for (p in drop) delete owed[p]
        delete drop
      } while (changed)
    }
    function flush(   i) {
      settle()
      for (i = 1; i <= nreads; i++)
        if (src[i] in owed) { printf "  %s %s: %s\n", probe, fn, line[i]; bad++ }
      if (dphi > best) { best = dphi; bestfn = fn }
      delete owed; delete inc; delete ninc; delete bits; delete src; delete line
      nreads = 0; dphi = 0
    }
    /^define / { match($0, /@[A-Za-z0-9_$.]+/); fn = substr($0, RSTART, RLENGTH); next }
    /^}/ { flush(); fn = ""; next }
    fn == "" { next }
    / = phi double / { dphi++; next }
    / = phi i64 / {
      rest = $0
      k = 0
      while (match(rest, /\[ [^,]+,/)) {
        inc[$1, ++k] = substr(rest, RSTART + 2, RLENGTH - 3)
        rest = substr(rest, RSTART + RLENGTH)
      }
      ninc[$1] = k
      next
    }
    / = bitcast double %[^ ]+ to i64/ { bits[$1] = 1; next }
    /bitcast i64 %[^ ]+ to double/ {
      match($0, /bitcast i64 %[^ ]+/)
      nreads++
      src[nreads] = substr($0, RSTART + 12, RLENGTH - 12)
      line[nreads] = $0
    }
    END {
      if (bad) exit 1
      if (best < min_row_phis) {
        printf "  %s: its busiest function carries %d double phis (need %d)\n",
               probe, best, min_row_phis
        exit 2
      }
      printf "  %s: %d double phis in %s\n", probe, best, bestfn
    }
  ' "$ir" && return 0

  local rc=$?
  if (( rc == 1 )); then
    echo "  $1: a loop-carried Float is still an i64 phi (see above)" >&2
  else
    echo "  $1: the row no longer carries its Floats in double phis" >&2
  fi
  return 1
}

fail=0
scan scalars || fail=1
scan vector2 || fail=1
scan while_one 1 || fail=1
if (( fail )); then
  echo "float-carry FAIL (see above)." >&2
  echo "  A phi every edge feeds a double should be a double phi, and each row" >&2
  echo "  should still hold one per Float it carries. Check that" >&2
  echo "  JIT::PromoteFloatPhis is registered (optimize_module) and still" >&2
  echo "  recognises the emitter's shape." >&2
  exit 1
fi
echo "float-carry OK (every phi owed a double is one)"
