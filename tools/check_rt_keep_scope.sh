#!/usr/bin/env bash
# CULEBRA_RT_KEEP / CULEBRA_RT_INLINE scope ratchet.
#
# The macro pair in include/runtime/rt_macros.h exists for exactly one
# reason: keep the `culebra_runtime_*` ABI helpers alive for ORC JIT's
# DynamicLibrarySearchGenerator, which resolves them by name over -rdynamic,
# and give the AOT archive's TU a strong definition of each. An internal
# helper (`_jit_*` / `_culebra_*`) that codegen never calls by name gets
# nothing from either macro: `used` is an unneeded pin in the JIT path, and a
# strong archive definition is what PE's ld cannot fold with the COMDAT copy a
# feature TU emits when it declines to inline a call — mingw's GCC 16 did that
# to `_jit_handle_bind_method`, and the Webview link needed
# --allow-multiple-definition to survive it. Plain `inline` everywhere, like
# the ~230 siblings that always were, is COMDAT against COMDAT, which every
# linker folds. See 39559596 for the 7 helpers that drifted onto KEEP by
# copy-paste, and the commit that moved the remaining 125 off INLINE.
#
# This gate keeps that class of drift from coming back: every
# CULEBRA_RT_KEEP or CULEBRA_RT_INLINE site must decorate a
# `culebra_runtime_*`-named function (checked textually, since
# macro-generated names like `culebra_runtime_math_##name` still spell the
# prefix literally) or be on the explicit exception list below, with a
# comment saying why.
set -euo pipefail
cd "$(dirname "$0")/.."

# culebra_aot_bootstrap (include/runtime/runtime_aot.h) is looked up by name
# from JIT-generated IR for the AOT entry point (include/jit.h, mod.get()
# symbol lookup) exactly like a culebra_runtime_* helper, just without the
# prefix.
#
# _jit_shared_val_prop / _jit_shared_val_index (include/sendable_jit.h) are
# declared plainly in jit_runtime.h and defined only where sendable_jit.h is
# compiled, so a feature TU that reaches the declaration through rt.h without
# the definition resolves the call at link time — against an out-of-line body
# the archive TU has to have emitted, which is what CULEBRA_RT_INLINE's strong
# definition guarantees there and a bare `inline` would leave to the
# optimizer.
EXCEPTIONS='culebra_aot_bootstrap|_jit_shared_val_prop|_jit_shared_val_index'

fail=0
checked=0

while IFS= read -r file; do
  [[ "$file" == include/runtime/rt_macros.h ]] && continue
  while IFS=: read -r line_no text; do
    # A comment that mentions the macro is not a site.
    [[ "$text" =~ ^[[:space:]]*// ]] && continue
    checked=$((checked + 1))
    # Window: the macro's line through the first line with `{` (function
    # open brace), which bounds both a plain declaration and a macro
    # template's signature line without reaching into the body.
    window=$(awk -v start="$line_no" 'NR >= start { print; if (index($0, "{")) exit }' "$file")
    if grep -q 'culebra_runtime_' <<<"$window"; then
      continue
    fi
    if grep -qE "$EXCEPTIONS" <<<"$window"; then
      continue
    fi
    fail=1
    echo "rt-keep-scope FAIL: $file:$line_no carries CULEBRA_RT_KEEP/INLINE on a" \
         "non-culebra_runtime_* function:" >&2
    echo "  $(head -1 <<<"$window")" >&2
  done < <(grep -nE 'CULEBRA_RT_(KEEP|INLINE)' "$file" || true)
done < <(grep -rlE 'CULEBRA_RT_(KEEP|INLINE)' include/ | sort)

if (( fail )); then
  cat >&2 <<'EOF'
  CULEBRA_RT_KEEP / CULEBRA_RT_INLINE buy nothing for a function codegen never
  calls by name -- it's ordinary inline C++, resolved at compile time like its
  ~230 siblings -- and the strong archive definition INLINE gives it is the
  one thing PE's ld cannot fold with a feature TU's un-inlined copy. Drop the
  macro pair to a bare `inline` (see 39559596), or add the name to EXCEPTIONS
  in tools/check_rt_keep_scope.sh with a comment saying why codegen looks it
  up by name (culebra_aot_bootstrap) or why another TU links against its
  out-of-line body (_jit_shared_val_prop).
EOF
  exit 1
fi

echo "rt-keep-scope OK ($checked CULEBRA_RT_KEEP/INLINE site(s), all culebra_runtime_*" \
     "or an explicit exception)"
