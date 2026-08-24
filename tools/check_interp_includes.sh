#!/usr/bin/env bash
# Ratchet: which files may #include the tree-walker's headers directly.
#
# Phase 4 B7-b cut interpreter.h / stdlib_interp.h out of the compiled
# lanes' include closure (vm.h compiles standalone without them). This
# guard keeps the cut cut: a new direct include of an interp header from a
# file outside the allowlist fails loudly here instead of silently
# re-growing the graph until B7-f's deletion discovers it. The allowlist is
# the tree-walker's own half plus the dual-stack hosts, and shrinks as
# B7-e/f retire them; it never grows.
set -euo pipefail
cd "$(dirname "$0")/.."

allow='
include/culebra.h
include/repl.h
include/sendable.h
include/stdlib_interp.h
include/debug_engine.h
include/test_engine.h
include/wrap.h
include/interp_sig_check.h
include/isolate.h
include/debugger.h
include/dap.h
include/foreign_binding.h
src/main.cc
src/runtime/culebra_rt.cc
src/runtime/culebra_rt_webview.cc
src/runtime/culebra_rt_scene.cc
tools/gen_canon_sigs.cc
'

# Carriers: headers that themselves pull the interp transitively. Including
# one of them re-grows the closure just as surely as including interpreter.h.
carriers='interpreter|stdlib_interp|isolate|sendable|sharedval|wrap|repl|debug_engine|test_engine|culebra'

fail=0
while IFS= read -r file; do
  case "$file" in
    tests/embedding/*) continue ;;  # embedding smokes host both stacks (B7-d)
  esac
  if ! grep -qx "$file" <<< "$allow"; then
    echo "interp-include FAIL: $file includes an interp-carrying header" \
         "directly — the compiled lanes' closure must stay interp-free" \
         "(vm.md §13; add to the allowlist only for a tree-walker-side file)" >&2
    fail=1
  fi
done < <(grep -rlE "^#include [\"<]($carriers)\.h[\">]" \
              include src tests tools --include='*.h' --include='*.cc' \
              --include='*.cpp')

[ "$fail" = 0 ] && echo "interp-include OK (direct includers are the allowlisted tree-walker half)"
exit "$fail"
