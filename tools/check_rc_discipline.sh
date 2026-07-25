#!/usr/bin/env bash
# RC-discipline ratchet (GAP3/GAP4-lite, docs/jit_ownership.md §4.7).
#
# Source-level gate on the ownership discipline: the hand-placed RC forms are
# migration debt / documented carve-outs, so their COUNT may only shrink. A
# new bare retain/release (instead of the Owned / JitOwnedVal /
# JitUnwindRelease / ThrowGuard layer) fails this check — lower a ceiling when
# you convert a site, never raise one without a review of docs/jit_ownership.md.
set -euo pipefail
cd "$(dirname "$0")/.."

fail=0
ratchet() { # name actual ceiling
  if (( $2 > $3 )); then
    echo "rc-discipline FAIL: $1 = $2 (ceiling $3) — new hand-placed RC ops?" >&2
    echo "  Use the ownership layer instead (docs/jit_ownership.md §4.2/§4.7)." >&2
    fail=1
  fi
}

# Codegen-side bare RC emissions in jit.h (excluding the emitters' own
# definitions and comment lines). Every remaining site is a documented
# carve-out class (jit_ownership.md §6) or ownership-layer infrastructure
# (the §4.8 unwind-temp pool release in release_unwind_temps, like the
# cleanup-pad loops); the set must not grow.
rel=$(grep "emit_value_release(" include/jit.h \
      | grep -v "void emit_value_release" | grep -vc "^[[:space:]]*//")
ret=$(grep "emit_value_retain(" include/jit.h \
      | grep -v "void emit_value_retain" | grep -vc "^[[:space:]]*//")
ratchet "bare emit_value_release sites (jit.h)" "$rel" 51
# 29 includes the ctor-overload shared-meta multi-capture retain in
# compile_class_decl: one class meta fans out into N `new` overload closures,
# each capture needing its own +1 (a genuine fan-out, not a throw-safety
# carve-out). Reviewed against jit_ownership.md §4.2/§4.7.
ratchet "bare emit_value_retain sites (jit.h)" "$ret" 29
# The borrow -> +1 seam funnels its retain through one call, so its call sites
# are invisible to the grep above. Count them here on their own ceiling —
# otherwise the seam becomes a way to add hand-placed retains unnoticed.
brw=$(grep "emit_borrow_to_owned(" include/jit.h \
      | grep -v "llvm::Value\* emit_borrow_to_owned" | grep -vc "^[[:space:]]*//")
ratchet "borrow->owned conversions (jit.h)" "$brw" 4

# Native-method endpoints consume self via RAII (JitMethodSelf at entry), not
# a tail release a throw would skip. sendable_jit.h is fully converted; keep
# it at zero.
tail_self=$(grep -c "culebra_runtime_value_release(self\.tag, self\.data)" \
            include/sendable_jit.h || true)
ratchet "tail self-releases (sendable_jit.h)" "$tail_self" 0

# Helper-side (runtime C++) bare RC calls per file (GAP3-ENFORCE ratchet). A
# helper that owns a value across a may-throw region uses the RAII forms
# (JitOwnedVal / JitMethodSelf / JitMethodArgs / JitUnwindRelease, §4.7); a
# new bare call is either a normal-path consume that belongs in one of those
# or fresh migration debt — justify it in review before raising a ceiling.
count_bare() { # file
  grep -E "_culebra_value_(release|retain)_impl\(|culebra_runtime_value_(release|retain)\(" "$1" \
    | grep -vcE "^[[:space:]]*//"
}
# 96 -> 98 (2026-07-12, reviewed): the two additions are the kwarg
# resolver's ctor-catch releases of the un-consumed kw/splat slab +1s on a
# mid-merge throw — the throw-edge releaser the §4.7 callee-consumes
# contract requires (a leak FIX inside the owning RAII class, not new debt;
# JitUnwindRelease is fixed-arity and cannot hold a variable slab).
ratchet "bare RC calls (stdlib_jit.h)" "$(count_bare include/stdlib_jit.h)" 98
ratchet "bare RC calls (sendable_jit.h)" "$(count_bare include/sendable_jit.h)" 17

# Codegen-side hand-placed throw guards: the automatic unwind-temp window
# (§4.8) is the default cleaner for a codegen-owned +1, so the hand-placed
# ThrowGuard population should only shrink as sites migrate onto it.
tg=$(grep -cE "ThrowGuard [a-z_]+\(this" include/jit.h)
ratchet "ThrowGuard sites (jit.h)" "$tg" 21

# Raw-across-BB ENFORCE (§4.9). consume() is block-pinned (using the raw
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
cu=$(grep "consume_unchecked()" include/jit.h \
     | grep -v "llvm::Value\* consume_unchecked" | grep -vc "^[[:space:]]*//")
ratchet "consume_unchecked sites (jit.h)" "$cu" 14
vphi=$(grep -v "jit_->builder_" include/jit.h | grep -c "CreatePHI(valueType_" \
       || true)
ratchet "hand-built %Value phis (jit.h)" "$vphi" 0

# compile_* raw-return seam (§4.9, closed 2026-07-12): every compile_* helper
# and extension compile hook returns Owned — a raw llvm::Value* return type on
# a compile_* function reopens the untyped +1 seam between the node compilers
# and compile()'s dispatch. Borrowed-contract emitters were renamed emit_*
# (emit_property_get returns +0, emit_comparison_i1 returns i1), so this
# stays a plain grep.
rawc=$({ grep -cE "llvm::Value\* (JIT::|JitExtension::)?(try_)?compile_" \
         include/jit.h include/stdlib_jit.h || true; } \
       | awk -F: '{s+=$2} END {print s}')
ratchet "raw-returning compile_* helpers (jit.h + stdlib_jit.h)" "$rawc" 0
tassign=$(grep -cE '(llvm::Value|auto) ?\* ?[A-Za-z_]+ ?= ?[^;]*\.consume\(\);' \
          include/jit.h || true)
ratchet "typed consume assignments (jit.h)" "$tassign" 0

if (( fail )); then exit 1; fi
echo "rc-discipline OK (release=$rel/51 retain=$ret/29 borrow=$brw/4" \
     "tail-self=$tail_self/0" \
     "stdlib=$(count_bare include/stdlib_jit.h)/98" \
     "sendable=$(count_bare include/sendable_jit.h)/17 throwguard=$tg/21" \
     "unchecked=$cu/14 vphi=$vphi/0 typed-consume=$tassign/0 rawcompile=$rawc/0)"
