#!/usr/bin/env bash
# Exception-handler balance gate (JIT + VM lowering codegen).
#
# A landingpad that opens the in-flight exception with `__cxa_begin_catch`
# owes exactly one `__cxa_end_catch`, or the exception object's handler count
# never returns to zero and libsupc++ never runs `_Unwind_DeleteException` —
# ~184 B stranded per caught throw, growing linearly in any program that
# catches in a loop.
#
# The design that makes that impossible splits pads in two (jit.h):
#
#   * a *cleanup* pad (JitCleanupPad) runs its region's releases and hands the
#     throw on. It never opens the exception, so there is nothing to balance.
#   * a *handler* pad (emit_handler_prologue) ends the throw and closes what
#     it opened — directly on the handled arm, or, when it re-raises instead,
#     on the rethrow's own unwind edge (emit_handler_rethrow's relay, the
#     shape GCC emits for `catch (...) { throw; }`).
#
# Checked here over emitted IR, so it holds for whatever path produced the pad
# and covers the VM lowering (which shares the same emitters):
#
#   1. every `invoke @__cxa_rethrow` unwinds to a block that calls
#      `__cxa_end_catch`
#   2. no `call @__cxa_rethrow` at all — a rethrow with no unwind edge has
#      nowhere to close its handler
#   3. `__cxa_begin_catch` appears only in a handler prologue — a block that
#      also classifies the carrier (`culebra_runtime_try_translate`) or ends
#      the catch on the spot (the iterator dispose swallow). A cleanup pad
#      that starts opening the exception fails here.
#
# Plus one source ceiling: landingpads are built in exactly one place, so a
# new pad cannot bypass the cleanup/handler split to begin with.
set -euo pipefail
cd "$(dirname "$0")/.."

BIN="${1:-build-dev/culebra}"
if [[ ! -x "$BIN" ]]; then
  echo "eh-balance FAIL: culebra binary not found at $BIN" >&2
  exit 2
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Full probe: exercises the pad kinds that re-raise — try/catch (region pad),
# scope cleanup with a resource and a defer, the frame cleanup ladder, a
# container literal under construction, and a for-in whose iterator disposes.
cat > "$TMP/full.cul" <<'CUL'
class R {
  new(n) { self.n = n }
  drop() { println("drop {self.n}") }
}
fn thrower(x) { if x == 2 { throw "boom" }; return x }
fn gen() { yield 1; yield 2 }
fn nested() {
  let r = R("nested")
  defer { println("defer") }
  return [thrower(1), thrower(2)]
}
fn run() {
  let keep = R("outer")
  try {
    for v in gen() { thrower(v) }
  } catch e {
    println("caught {e}")
  }
  try { nested() } catch e { println("caught {e}") }
  throw "out"
}
try { run() } catch e { println("done {e}") }
CUL

# Minimal probe for the VM lane: the bytecode slice rejects most of the above,
# and one try/catch is enough — the lowering has a single region pad shape.
cat > "$TMP/min.cul" <<'CUL'
mut n = 0
for i in 0..3 {
  try { throw "x" } catch e { n = n + 1 }
}
println(n)
CUL

fail=0

# Reports, per emitted IR file: the bare-call count, the number of rethrow
# invokes, and any invoke whose unwind block never ends the catch.
audit() { # <label> <ir file>
  awk -v label="$1" '
    # The first `@name(` on a define line is the function; the personality
    # reference later on the same line is not.
    /^define/ {
      if (match($0, /@[A-Za-z0-9_.$]+\(/))
        fn = substr($0, RSTART + 1, RLENGTH - 2)
      blk = "entry"; next
    }
    /^[A-Za-z0-9_.$-]+:/ { blk = $0; sub(/:.*/, "", blk); next }
    /@__cxa_end_catch/ { ends[fn SUBSEP blk] = 1; handler[fn SUBSEP blk] = 1 }
    /@culebra_runtime_try_translate/ { handler[fn SUBSEP blk] = 1 }
    /@__cxa_begin_catch/ { o++; ofn[o] = fn; oblk[o] = blk }
    /call void @__cxa_rethrow/ {
      bare++; printf "eh-balance FAIL[%s]: bare `call @__cxa_rethrow` in %s / %s\n", label, fn, blk > "/dev/stderr"
    }
    # The unwind label sits on the invoke line or its continuation.
    /invoke void @__cxa_rethrow/ { pend_fn = fn; pend_blk = blk; want = 1 }
    want && /unwind label %/ {
      lbl = $0; sub(/.*unwind label %/, "", lbl); sub(/[ ,].*$/, "", lbl)
      n++; rfn[n] = pend_fn; rblk[n] = pend_blk; rlbl[n] = lbl; want = 0
    }
    END {
      bad = 0
      for (i = 1; i <= n; i++) {
        if (!((rfn[i] SUBSEP rlbl[i]) in ends)) {
          bad++
          printf "eh-balance FAIL[%s]: rethrow in %s / %s unwinds to %%%s, which never calls __cxa_end_catch\n", \
                 label, rfn[i], rblk[i], rlbl[i] > "/dev/stderr"
        }
      }
      for (i = 1; i <= o; i++) {
        if (!((ofn[i] SUBSEP oblk[i]) in handler)) {
          bad++
          printf "eh-balance FAIL[%s]: __cxa_begin_catch in %s / %s, which is not a handler prologue —\n  a cleanup pad must hand the throw on without opening it\n", \
                 label, ofn[i], oblk[i] > "/dev/stderr"
        }
      }
      printf "%d %d %d %d\n", n, bare + 0, bad, o + 0
    }
  ' "$2"
}

check() { # <label> <lane flag> <probe>
  local label="$1" lane="$2" probe="$3" ir="$TMP/$1.ll"
  if ! "$BIN" "$lane" -O0 --emit-llvm "$probe" > "$ir" 2>"$TMP/$1.err"; then
    echo "eh-balance FAIL: $BIN $lane --emit-llvm failed" >&2
    sed 's/^/  /' "$TMP/$1.err" >&2
    fail=1
    return
  fi
  read -r sites bare bad opens < <(audit "$label" "$ir")
  if (( bare > 0 || bad > 0 )); then
    echo "  Cleanup pads hand the throw on without opening it; a handler pad" >&2
    echo "  closes what it opened (jit.h CleanupPad / emit_handler_rethrow)." >&2
    fail=1
    return
  fi
  if (( opens == 0 )); then
    echo "eh-balance FAIL: probe opened no exception at all ($label) — the probe" >&2
    echo "  no longer reaches a handler pad, so this gate proves nothing" >&2
    fail=1
    return
  fi
  echo "eh-balance OK ($label): $opens handler prologue(s), $sites re-raise(s), all closed"
}

check jit --jit "$TMP/full.cul"
check vm --vm-llvm "$TMP/min.cul"

# Source ceiling: one place builds landingpads, so a new pad has to go through
# the cleanup/handler split rather than hand-rolling its own prologue.
pads=$(grep -ho "CreateLandingPad" include/*.h | wc -l)
if (( pads > 1 )); then
  echo "eh-balance FAIL: CreateLandingPad appears $pads times (ceiling 1)" >&2
  echo "  Landingpads are built only by jit.h emit_landingpad, which is what" >&2
  echo "  decides whether the pad opens the exception. Route the new pad" >&2
  echo "  through JitCleanupPad (cleanup) or emit_handler_prologue (handler)." >&2
  fail=1
else
  echo "eh-balance OK (source): landingpads are built in one place"
fi

exit $fail
