#!/usr/bin/env bash
# CULEBRA_RT_KEEP / CULEBRA_RT_INLINE scope ratchet: both decorate only
# `culebra_runtime_*` functions (checked textually — a macro-generated name
# like `culebra_runtime_math_##name` still spells the prefix) or an EXCEPTIONS
# entry. rt_macros.h explains why an internal helper on either macro is a PE
# link hazard; 39559596 and 83ceea96 removed the ones that had drifted on.
set -euo pipefail
cd "$(dirname "$0")/../.."

# culebra_aot_bootstrap (include/runtime/runtime_aot.h) is looked up by name
# from JIT-generated IR for the AOT entry point (include/jit.h, mod.get()
# symbol lookup) exactly like a culebra_runtime_* helper, just without the
# prefix. (The Shared.new readers used to be here too — declared in
# jit_runtime.h, defined only in sendable_jit.h — until they became hooks the
# view constructor installs, so nothing links against them by name any more.)
EXCEPTIONS='culebra_aot_bootstrap'

# One pass per file: a non-comment macro line opens a window that runs
# through the function's `{`, a prototype's closing `;`, or the next macro
# line — bounding a declaration and a macro template's signature line alike
# without reaching into the body.
fail=0
checked=0
while IFS= read -r file; do
  [[ "$file" == include/runtime/rt_macros.h ]] && continue
  out=$(awk -v exc="$EXCEPTIONS" '
    function judge() {
      checked++
      if (index(text, "culebra_runtime_") == 0 && text !~ exc)
        printf "FAIL %d %s\n", start, head
      win = 0
    }
    /CULEBRA_RT_(KEEP|INLINE)/ && $0 !~ /^[[:space:]]*\/\// {
      if (win) judge()
      win = 1; start = NR; head = $0; text = ""
    }
    win {
      text = text $0 "\n"
      if (index($0, "{") || $0 ~ /;[[:space:]]*$/) judge()
    }
    END { if (win) judge(); printf "CHECKED %d\n", checked }' "$file")
  while IFS= read -r line; do
    case "$line" in
      CHECKED\ *) checked=$((checked + ${line#CHECKED }));;
      FAIL\ *)
        fail=1
        line_no=${line#FAIL }; line_no=${line_no%% *}
        echo "rt-keep-scope FAIL: $file:$line_no carries CULEBRA_RT_KEEP/INLINE on a" \
             "non-culebra_runtime_* function:" >&2
        echo "  ${line#FAIL * }" >&2;;
    esac
  done <<<"$out"
done < <(grep -rlE 'CULEBRA_RT_(KEEP|INLINE)' include/ | sort)

if (( fail )); then
  cat >&2 <<'EOF'
  Drop the macro pair to a bare `inline` (see 39559596 / 83ceea96 and
  rt_macros.h), or add the name to EXCEPTIONS in tools/checks/check_rt_keep_scope.sh
  with a comment saying why codegen looks it up by name
  (culebra_aot_bootstrap).
EOF
  exit 1
fi

echo "rt-keep-scope OK ($checked CULEBRA_RT_KEEP/INLINE site(s), all culebra_runtime_*" \
     "or an explicit exception)"
