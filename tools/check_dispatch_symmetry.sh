#!/usr/bin/env bash
# Dispatch-tag symmetry gate (refactoring Area ②a).
#
# interp and the bytecode compiler are the two consumers of the AST: every
# node tag the interpreter's _eval_dispatch handles must have a counterpart
# case in the compiler's dispatch, and vice versa. The switches themselves
# are the eval_X <-> compile_X correspondence table; this gate keeps their
# tag sets equal, so adding a grammar node to one walker and forgetting the
# other fails the test gate instead of becoming a niche one-backend bug.
#
# The compiler splits its dispatch by position (statement, expression), so
# its side of the comparison is the union of the two switches. Until Phase 3
# the second consumer was the JIT's compile(); the JIT lowers bytecode now
# and reads no AST at all.
#
# Intentional asymmetries are the explicit allowlist below — extend it only
# with a comment saying why the tag cannot (or need not) match.
set -euo pipefail
cd "$(dirname "$0")/.."

# Case labels of the dispatch switch, one tag per line, sorted. The awk range
# is anchored on the dispatch function's signature and ends at the method's
# closing brace (two-space indent).
interp_tags() {
  awk '/Value _eval_dispatch\(const peg::Ast/,/^  }$/' include/interpreter.h \
    | { grep -vE '^[[:space:]]*//' | grep -oE 'case "[A-Z_]+"_' || true; } \
    | sed 's/case "//;s/"_//' | sort -u
}
vm_tags() {
  { awk '/void compile_statement_inner\(const peg::Ast& ast\) \{/,/^  }$/' \
        include/vm.h
    awk '/ExprResult compile_expr\(const peg::Ast& ast\) \{/,/^  }$/' \
        include/vm.h
  } | { grep -vE '^[[:space:]]*//' | grep -oE 'case "[A-Z_]+"_' || true; } \
    | sed 's/case "//;s/"_//' | sort -u
}

interp=$(interp_tags)
vm=$(vm_tags)

# Extraction sanity floor: if a dispatch function is renamed or moved, the awk
# anchor silently matches nothing and the diff below would "pass" on two empty
# sets. Both dispatches handle 50+ tags today; anything below that means the
# extraction broke, not that the language shrank.
for set_name in interp vm; do
  n=$(printf '%s\n' "${!set_name}" | grep -c . || true)  # ${!x}: indirect ref
  if (( n < 50 )); then
    echo "dispatch-symmetry FAIL: extracted only $n tags from the $set_name" \
         "dispatch — the awk anchor in $0 no longer matches its function" >&2
    exit 1
  fi
done

# Compiler-only tags that are intentional: the interpreter folds raw string
# tokens through its is_token fallthrough instead of a dedicated case.
allowed_vm_only=$'STRING'

interp_only=$(comm -23 <(printf '%s\n' "$interp") <(printf '%s\n' "$vm"))
vm_only=$(comm -13 <(printf '%s\n' "$interp") <(printf '%s\n' "$vm") \
          | grep -vxF "$allowed_vm_only" || true)

if [[ -n "$interp_only" || -n "$vm_only" ]]; then
  [[ -z "$interp_only" ]] || echo "dispatch-symmetry FAIL: tags handled by" \
    "interp _eval_dispatch but missing from the bytecode compiler:" \
    $interp_only >&2
  [[ -z "$vm_only" ]] || echo "dispatch-symmetry FAIL: tags handled by the" \
    "bytecode compiler but missing from interp _eval_dispatch:" $vm_only >&2
  echo "  Add the node to both walkers (interp/VM symmetry is a hard" >&2
  echo "  requirement), or allowlist it here with a rationale." >&2
  exit 1
fi

echo "dispatch-symmetry OK ($(printf '%s\n' "$interp" | grep -c .) shared tags," \
     "compiler-only allowlist:" $allowed_vm_only")"
