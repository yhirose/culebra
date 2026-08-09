#!/usr/bin/env bash
# CULEBRA_RT_KEEP scope ratchet.
#
# The macro pair in include/runtime/rt_macros.h exists for exactly one
# reason: keep the `culebra_runtime_*` ABI helpers alive for ORC JIT's
# DynamicLibrarySearchGenerator, which resolves them by name over -rdynamic.
# An internal helper (`_jit_*` / `_culebra_*`) that codegen never calls by
# name gets nothing from the macro in the JIT path (an unneeded `used`) and
# rides the same optimizer-dependent "the compiler happens to inline it"
# argument in the feature-archive path as the ~230 siblings already left
# plain `inline` — see 8b6d447, which found and fixed 7 such helpers that
# had drifted onto the macro by copy-paste from a neighboring ABI function.
#
# This gate keeps that class of drift from coming back: every
# CULEBRA_RT_KEEP site must decorate a `culebra_runtime_*`-named function
# (checked textually, since macro-generated names like
# `culebra_runtime_math_##name` still spell the prefix literally) or be on
# the explicit exception list below, with a comment saying why.
set -euo pipefail
cd "$(dirname "$0")/.."

# culebra_aot_bootstrap (include/runtime/runtime_aot.h) is looked up by name
# from JIT-generated IR for the AOT entry point (include/jit.h, mod.get()
# symbol lookup) exactly like a culebra_runtime_* helper, just without the
# prefix.
EXCEPTIONS='culebra_aot_bootstrap'

fail=0
checked=0

while IFS= read -r file; do
  [[ "$file" == include/runtime/rt_macros.h ]] && continue
  while IFS=: read -r line_no _; do
    checked=$((checked + 1))
    # Window: the CULEBRA_RT_KEEP line through the first line with `{`
    # (function open brace), which bounds both a plain declaration and a
    # macro template's signature line without reaching into the body.
    window=$(awk -v start="$line_no" 'NR >= start { print; if (index($0, "{")) exit }' "$file")
    if grep -q 'culebra_runtime_' <<<"$window"; then
      continue
    fi
    if grep -qE "$EXCEPTIONS" <<<"$window"; then
      continue
    fi
    fail=1
    echo "rt-keep-scope FAIL: $file:$line_no carries CULEBRA_RT_KEEP on a" \
         "non-culebra_runtime_* function:" >&2
    echo "  $(head -1 <<<"$window")" >&2
  done < <(grep -n 'CULEBRA_RT_KEEP' "$file" || true)
done < <(grep -rl 'CULEBRA_RT_KEEP' include/ | sort)

if (( fail )); then
  cat >&2 <<'EOF'
  CULEBRA_RT_KEEP buys nothing for a function codegen never calls by name --
  it's ordinary inline C++, resolved at compile time like its ~230 siblings.
  Drop the macro pair to a bare `inline` (see 8b6d447), or add the name to
  EXCEPTIONS in tools/check_rt_keep_scope.sh with a comment saying why
  codegen looks it up by name (culebra_aot_bootstrap is the one precedent).
EOF
  exit 1
fi

echo "rt-keep-scope OK ($checked CULEBRA_RT_KEEP site(s), all culebra_runtime_*" \
     "or an explicit exception)"
