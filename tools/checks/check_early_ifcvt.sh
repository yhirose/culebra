#!/usr/bin/env bash
# Early if-conversion gate (the bytecode VM's LLVM lowering = `--jit`).
#
# JIT::tune_backend turns off AArch64's early if-conversion: it speculates
# a small `if` arm into fcsel whatever the branch's odds, and an arm that
# assigns a loop-carried Float then puts fcmp+fcsel on the loop's critical
# path every iteration (jit.h, docs/internals/vm.md §7). The knob lives in
# the backend, so nothing in the emitted IR shows whether it still holds —
# check_float_carry.sh cannot see it — and the way it goes quiet is a
# silent one: the option is looked up by name, and an LLVM that renamed it
# would leave the lookup empty and the loop a sixth slower.
#
# So this reads the machine code instead. With CULEBRA_JIT_CACHE set, the
# JIT writes the object it compiled, and `objdump -d` gives it back with
# the function symbols intact. Over the scalars row of
# tools/bench/vector_loop.cul, the loop (the one function holding the
# fsqrt) may contain no fcsel at all: with the pass running there are
# three.
#
# Only AArch64 codegen has the pass, so the gate skips — out loud — on any
# other host. It runs where the codegen exists: locally and on the macOS
# lane of CI.
set -euo pipefail
cd "$(dirname "$0")/../.."

case "$(uname -m)" in
  arm64|aarch64) ;;
  *)
    echo "early-ifcvt SKIP (host is $(uname -m); the knob is AArch64 codegen)"
    exit 0
    ;;
esac

BIN="${1:-build-dev/culebra}"
if [[ ! -x "$BIN" ]]; then
  echo "early-ifcvt FAIL: culebra binary not found at $BIN" >&2
  exit 2
fi

OBJDUMP=""
for cand in llvm-objdump objdump; do
  if command -v "$cand" >/dev/null 2>&1; then OBJDUMP="$cand"; break; fi
done
if [[ -z "$OBJDUMP" ]]; then
  echo "early-ifcvt FAIL: neither llvm-objdump nor objdump is on PATH" >&2
  exit 2
fi

TMP="$(mktemp -d "${TMPDIR:-/tmp}/culebra-early-ifcvt.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# The scalars row of tools/bench/vector_loop.cul. The bounce arm at the end
# is the one the pass converts: two assignments, one of them to a Float the
# loop carries. check_float_carry.sh copies the same row, and the two copies
# stay separate on purpose — this gate needs exactly one function holding an
# fsqrt, so a line added there for that gate must not move this one.
cat > "$TMP/probe.cul" <<'CUL'
let DT = 0.016
let GX = 0.5
let GY = -9.8
let MAXSPEED = 40.0
let BOUNCE = 0.8

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

# Both cache variables are pinned: an inherited size cap can be small enough
# that the object is evicted in the same process that wrote it.
if ! CULEBRA_JIT_CACHE="$TMP/cache" CULEBRA_JIT_CACHE_MAX_MB=4096 \
     "$BIN" --jit "$TMP/probe.cul" > /dev/null 2>"$TMP/run.err"; then
  echo "early-ifcvt FAIL: the probe did not run" >&2
  sed 's/^/  /' "$TMP/run.err" >&2
  exit 1
fi
if ! ls "$TMP"/cache/*.o >/dev/null 2>&1; then
  echo "early-ifcvt FAIL: the JIT wrote no object into CULEBRA_JIT_CACHE" >&2
  exit 1
fi

# objdump under a sandbox complains about its own cache file, so its stderr
# is kept aside rather than shown — and printed only if the read fails, when
# the reason is the first thing worth seeing.
"$OBJDUMP" -d "$TMP"/cache/*.o > "$TMP/asm.txt" 2>"$TMP/objdump.err" || true

# Per function: the fcsel count, and whether this is the loop (has the
# fsqrt). One function carries the loop; it must carry no fcsel.
awk '
  /^[0-9a-f]+ <[^>]*>:$/ { fn = $2; gsub(/[<>:]/, "", fn); next }
  fn == "" { next }
  /fcsel/ { sel[fn]++ }
  /fsqrt/ { loop[fn] = 1 }
  END {
    n = 0
    for (f in loop) { n++; where = f; bad = sel[f] + 0 }
    if (n != 1) {
      printf "  %d functions hold an fsqrt (expected the one loop)\n", n
      exit 2
    }
    if (bad) {
      printf "  %s: %d fcsel in the loop\n", where, bad
      exit 1
    }
    printf "early-ifcvt OK (no fcsel in the loop %s)\n", where
  }
' "$TMP/asm.txt" && exit 0
rc=$?

if (( rc == 1 )); then
  echo "early-ifcvt FAIL: the bounce arm was if-converted (see above)." >&2
  echo "  JIT::tune_backend should have turned aarch64-enable-early-ifcvt off;" >&2
  echo "  check that it still runs at target init and that the option still" >&2
  echo "  exists under that name in this LLVM." >&2
else
  echo "early-ifcvt FAIL: the probe's loop was not found in the object." >&2
  echo "  Either the disassembly is empty or the probe no longer compiles to" >&2
  echo "  one function holding the fsqrt." >&2
  if [[ -s "$TMP/objdump.err" ]]; then
    echo "  $OBJDUMP said:" >&2
    sed 's/^/    /' "$TMP/objdump.err" >&2
  fi
fi
exit 1
