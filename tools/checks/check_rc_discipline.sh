#!/usr/bin/env bash
# RC-discipline ratchet (GAP3/GAP4-lite).
#
# Source-level gate on the ownership discipline: the hand-placed RC forms are
# migration debt / documented carve-outs, so their COUNT may only shrink. A
# new bare retain/release (instead of the Owned / JitOwnedVal /
# JitUnwindRelease / ThrowGuard layer) fails this check — lower a ceiling when
# you convert a site, never raise one without review.
set -euo pipefail
cd "$(dirname "$0")/../.."

fail=0

# Every path this gate measures is hardcoded below, and several counts are
# taken with `|| true` so a genuine zero can pass. That combination makes a
# rename invisible: grep on a missing file prints nothing, `|| true` swallows
# the error, and the ratchet then evaluates `(( "" > 0 ))`, which is false.
# Guard both ends — the paths must exist, and a count must be a number.
for f in include/jit/jit.h include/stdlib/bindings.h include/conc/sendable.h; do
  [[ -f $f ]] || {
    echo "rc-discipline FAIL: no $f — renamed or moved? update this gate" >&2
    exit 1
  }
done

ratchet() { # name actual ceiling
  if ! [[ $2 =~ ^[0-9]+$ ]]; then
    echo "rc-discipline FAIL: $1 produced no count ('$2') — the pattern or" >&2
    echo "  the path it scans moved; this ratchet is measuring nothing." >&2
    fail=1
    return
  fi
  # A nonzero ceiling asserts a population exists. If it has emptied, the code
  # being guarded was renamed, moved or deleted, and the ratchet now passes
  # while measuring nothing. Hard-zero invariants carry ceiling 0 and are
  # unaffected.
  if (( $3 > 0 && $2 == 0 )); then
    echo "rc-discipline FAIL: $1 = 0 against ceiling $3 — the population this" >&2
    echo "  ratchet guards is empty, so it proves nothing. Repoint it at the" >&2
    echo "  code that replaced it, or retire it." >&2
    fail=1
    return
  fi
  if (( $2 > $3 )); then
    echo "rc-discipline FAIL: $1 = $2 (ceiling $3) — new hand-placed RC ops?" >&2
    echo "  Use the ownership layer instead." >&2
    fail=1
  fi
}

# Codegen-side bare RC emissions in jit.h (excluding the emitters' own
# definitions and comment lines). Every remaining site is a documented
# carve-out class or ownership-layer infrastructure (the unwind-temp pool
# release in release_unwind_temps, like the cleanup-pad loops); the set must
# not grow.
rel=$(grep "emit_value_release(" include/jit/jit.h \
      | grep -v "void emit_value_release" | grep -vc "^[[:space:]]*//")
ret=$(grep "emit_value_retain(" include/jit/jit.h \
      | grep -v "void emit_value_retain" | grep -vc "^[[:space:]]*//")
# 51 -> 49 (2026-07-25): the for-in protocol iterator moved onto a scope slot,
# so emit_iter_dispose no longer hand-places the alloca's release (nor the
# local unwind pad that mirrored it) — scope teardown owns both now.
# 49 -> 51 (2026-08-03, reviewed): compile_function_call_raw's errorBB pins
# `callee` (often BORROWED out of `selfVal`'s own slots) before releasing
# `selfVal`, fixing a use-after-free on the `__call__`/ctor overload probes.
# The pin is dropped once per leaf arm; the ctor and not-a-function arms
# need a new bare release each (the overload arm folds it into the existing
# recursive-call handoff instead).
# 51 -> 55 (2026-08-06, reviewed): compile_assign_complex's new `??=`
# nil-coalesce dispatch for Array/Object INDEX and Object DOT lvalues
# builds its own basic-block structure (rval is null for `??=`, so it can't
# reuse the existing compound/plain dispatch's Owned-threaded flow) —
# each arm needs its own retain-for-merge-result / release-of-the-discarded
# short-circuit-or-nil-hit value, mirroring the bare-RC style the adjacent
# compound/plain arms already use for the same receiver kinds.
ratchet "bare emit_value_release sites (jit.h)" "$rel" 55
# 29 includes the ctor-overload shared-meta multi-capture retain in
# compile_class_decl: one class meta fans out into N `new` overload closures,
# each capture needing its own +1 (a genuine fan-out, not a throw-safety
# carve-out).
# 29 -> 34 (2026-08-06, reviewed): same `??=` nil-coalesce dispatch as
# above — the Array/Object/DOT arms each retain their merge-result value
# (a +0 borrow promoted to +1, or the value handed to object_set_any /
# emit_object_set / array_set, whose consumed +1 must be re-minted for the
# expression result) and retain the Object-INDEX key across the
# object_get_for_coalesce / object_set_any pair, matching the existing
# compound branch's identical retain-before-transient-consume pattern.
ratchet "bare emit_value_retain sites (jit.h)" "$ret" 34
# The borrow -> +1 seam funnels its retain through one call, so its call sites
# are invisible to the grep above. Count them here on their own ceiling —
# otherwise the seam becomes a way to add hand-placed retains unnoticed.
# 4 -> 5 (2026-08-03, reviewed): `.presence()` hands the receiver itself back
# out as the method's result on the non-empty arm (Array/Object/String/Set,
# same identity Ruby's `Object#presence` returns) — a genuine new borrow ->
# owned crossing through the seam, not a bare retain bypassing it.
brw=$(grep "emit_borrow_to_owned(" include/jit/jit.h \
      | grep -v "llvm::Value\* emit_borrow_to_owned" | grep -vc "^[[:space:]]*//")
ratchet "borrow->owned conversions (jit.h)" "$brw" 5

# Native-method endpoints consume self via RAII (JitMethodSelf at entry), not
# a tail release a throw would skip. sendable_rt.h is fully converted; keep
# it at zero.
tail_self=$(grep -c "culebra_runtime_value_release(self\.tag, self\.data)" \
            include/conc/sendable.h || true)
ratchet "tail self-releases (sendable_rt.h)" "$tail_self" 0

# Helper-side (runtime C++) bare RC calls per file (GAP3-ENFORCE ratchet). A
# helper that owns a value across a may-throw region uses the RAII forms
# (JitOwnedVal / JitMethodSelf / JitMethodArgs / JitUnwindRelease); a
# new bare call is either a normal-path consume that belongs in one of those
# or fresh migration debt — justify it in review before raising a ceiling.
count_bare() { # file
  grep -E "_culebra_value_(release|retain)_impl\(|culebra_runtime_value_(release|retain)\(" "$1" \
    | grep -vcE "^[[:space:]]*//"
}
# 96 -> 98 (2026-07-12, reviewed): the two additions are the kwarg
# resolver's ctor-catch releases of the un-consumed kw/splat slab +1s on a
# mid-merge throw — the throw-edge releaser the callee-consumes
# contract requires (a leak FIX inside the owning RAII class, not new debt;
# JitUnwindRelease is fixed-arity and cannot hold a variable slab).
# 98 -> 100 (2026-07-28, reviewed): same function, one more throw edge. A
# keyword-supplied value's target param isn't known until the name lookup
# in _jit_ns_kwarg_resolve_core, so it skips the compile-time per-argument
# type check the positional path gets — this added that check (a symmetry
# FIX: Compress.deflate(s, level: "x") raised on interp but not JIT/AOT
# before it). The two releases are the same already-filled-slab +
# remaining-merged-kwargs cleanup the sibling throw edges in this loop use.
# 100 -> 98 (2026-08-01): the Http route/ws handlers and the sqlite
# transaction body hold their invoke result in JitOwnedVal, so the tail
# releases a throwing response-apply / COMMIT used to skip are gone.
# 98 -> 99 (2026-08-07, reviewed): new culebra_runtime_random_choice picks
# one element out of a raw JitArray and retains it before returning —
# the exact same shape culebra_runtime_random_weighted_choice already uses
# (already inside the 98). There's no Owned/JitOwnedVal layer to route
# through here: this is a CULEBRA_RT_INLINE extern-C runtime helper, not
# codegen, so the retain has to be bare — one more instance of an already-
# justified pattern, not a new debt shape.
# 99 -> 100 (2026-08-11, reviewed): new _ns_global_repeat (bare global
# `repeat(n, value)`) retains its borrowed `value` once per copy before
# pushing it into the freshly-built Array — the args a raw NsMethod adapter
# receives are borrowed (the generic ns-call dispatch drops every arg slot
# after the call returns), so each of the n copies needs its own retain
# before array_push absorbs it. Same "no Owned/JitOwnedVal layer reachable
# from a raw runtime adapter" shape as random_choice/weighted_choice above.
ratchet "bare RC calls (stdlib_rt.h)" "$(count_bare include/stdlib/bindings.h)" 100
# 17 -> 12 (2026-08-01): the isolate/parallel child entries hold the rebuilt
# closure, its args and the call result in JitOwnedVal, so their tail releases
# are gone — and with them the hang a throwing child caused by never dropping a
# captured channel endpoint.
# 12 -> 11 (2026-08-08): the Shared view's iterator keeps its state in closure
# captures, so its `iter` self-returning reader (and that reader's hand-placed
# retain) is gone.
ratchet "bare RC calls (sendable_rt.h)" "$(count_bare include/conc/sendable.h)" 11

# Runtime-side borrow -> +1 seam (JitOwnedVal::from_borrowed). Its retain lives
# inside the ownership layer, so it is invisible to count_bare above — the same
# blind spot emit_borrow_to_owned has on the codegen side. Count the call sites
# on their own ceiling so the seam cannot become a quiet way to add hand-placed
# retains. Current population: 2 (Sys.env returning its borrowed `fallback`;
# CodeGen.Program.run's natives table, which holds each bound closure for the
# run rather than trusting the caller's Object to keep it -- a native could
# reach that Object and remove the very entry the shim is about to call).
rbrw=$(grep -rE --include='*.h' "JitOwnedVal::from_borrowed\(" include/ \
       | grep -vcE "^[^:]*:[[:space:]]*//" || true)
ratchet "runtime borrow->owned seam sites" "$rbrw" 2

# Codegen-side hand-placed throw guards: the automatic unwind-temp window
# is the default cleaner for a codegen-owned +1, so the hand-placed
# ThrowGuard population should only shrink as sites migrate onto it.
tg=$(grep -cE "ThrowGuard [a-z_]+\(this" include/jit/jit.h)
ratchet "ThrowGuard sites (jit.h)" "$tg" 21

# Raw-across-BB ENFORCE. consume() is block-pinned (using the raw
# outside its pin block aborts codegen), so the only ways a bare +1 can
# still cross a basic block are the forms counted here — each must shrink
# as sites migrate onto Pinned / OwnedPhi / scope slots:
#  - consume_unchecked: the justified escape hatch (consume()'s own
#    delegation and consume_all/OwnedPhi internals excluded via jit_-> /
#    v.-prefix distinction is not greppable, so count all and set the
#    ceiling to the audited total);
#  - hand-built %Value phis: every tagged phi should be an OwnedPhi (its
#    internal spelling `jit_->builder_.CreatePHI` is naturally excluded);
#  - explicitly-typed consume assignments: `llvm::Value* x = ...consume();`
#    (or auto*) converts at the assignment and the raw then crosses
#    unchecked. A consume nested in a scalar conversion
#    (`value_to_long(...consume())`) is not counted — the stored value is
#    an i64/i1, not a +1.
# Current population: 3 ownership-layer internals (consume()'s own
# delegation, consume_all, OwnedPhi::add_incoming) + 11 justified sites
# (dispatch-arm handoffs, slot-owned crossings, prologue transfer, the
# call.phi late-merge arm raws in compile_function_call_raw, and the
# for-protocol iterator whose alloca slot owns the crossing — the last
# three added 2026-07-12 by the compile_*→Owned return-seam migration,
# each with a per-site rationale comment).
cu=$(grep "consume_unchecked()" include/jit/jit.h \
     | grep -v "llvm::Value\* consume_unchecked" | grep -vc "^[[:space:]]*//")
ratchet "consume_unchecked sites (jit.h)" "$cu" 14
vphi=$(grep -v "jit_->builder_" include/jit/jit.h | grep -c "CreatePHI(valueType_" \
       || true)
ratchet "hand-built %Value phis (jit.h)" "$vphi" 0

# compile_* raw-return seam (closed 2026-07-12): every compile_* helper
# and extension compile hook returns Owned — a raw llvm::Value* return type on
# a compile_* function reopens the untyped +1 seam between the node compilers
# and compile()'s dispatch. Borrowed-contract emitters were renamed emit_*
# (emit_property_get returns +0, emit_comparison_i1 returns i1), so this
# stays a plain grep.
rawc=$({ grep -cE "llvm::Value\* (JIT::|JitExtension::)?(try_)?compile_" \
         include/jit/jit.h include/stdlib/bindings.h || true; } \
       | awk -F: '{s+=$2} END {print s}')
ratchet "raw-returning compile_* helpers (jit.h + stdlib_rt.h)" "$rawc" 0
tassign=$(grep -cE '(llvm::Value|auto) ?\* ?[A-Za-z_]+ ?= ?[^;]*\.consume\(\);' \
          include/jit/jit.h || true)
ratchet "typed consume assignments (jit.h)" "$tassign" 0

if (( fail )); then exit 1; fi
echo "rc-discipline OK (release=$rel/55 retain=$ret/34 borrow=$brw/5" \
     "rt-borrow=$rbrw/1 tail-self=$tail_self/0" \
     "stdlib=$(count_bare include/stdlib/bindings.h)/100" \
     "sendable=$(count_bare include/conc/sendable.h)/11 throwguard=$tg/21" \
     "unchecked=$cu/14 vphi=$vphi/0 typed-consume=$tassign/0 rawcompile=$rawc/0)"
