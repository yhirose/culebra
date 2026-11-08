#!/bin/sh
# Generate include/stdlib/preambles.gen.h from the culebra sources in
# src/preambles/*.cul (the editable single source of truth). Each .cul is
# embedded verbatim as a `<NAME>_MODULE_SOURCE` raw-string constant, so the
# single self-contained `culebra` binary keeps carrying them. Most are stdlib
# modules, listed in lazy_ns_modules() / lazy_fn_groups(); test_ambient.cul is
# not — `culebra test` runs it directly. `just dev` runs it before every
# build, so an edited .cul is what gets baked; `--check` (CI gate) fails if
# the committed header is stale.
set -eu
DIR=$(cd "$(dirname "$0")/.." && pwd)
SRC="$DIR/src/preambles"
OUT="$DIR/include/stdlib/preambles.gen.h"
gen() {
  printf '// Generated from src/preambles/*.cul by misc/gen_preambles.sh — do not edit.\n'
  printf '// Edit the .cul sources, then run `just gen-preambles` (CI checks sync).\n'
  printf '#pragma once\n\n'
  for base in time term canvas audio kauai midi args matchers regex peg fst string_fns log desktop path vector2 vector3 deque priority_queue state_machine dir effects test_ambient llm; do
    name=$(printf '%s' "$base" | tr 'a-z' 'A-Z')
    printf 'inline constexpr const char* %s_MODULE_SOURCE = R"=culpre=(' "$name"
    cat "$SRC/$base.cul"
    printf ')=culpre=";\n\n'
  done
}
# Both modes generate into a real file first. --check compares it: BSD diff
# spools a `-` operand through the darwin temp dir, which a sandboxed run
# cannot write, and that failure looked identical to "out of sync".
tmp=$(mktemp "${TMPDIR:-/tmp}/gen_preambles.XXXXXX")
trap 'rm -f "$tmp"' EXIT
gen > "$tmp"
if [ "${1:-}" = "--check" ]; then
  report=$(diff -u "$OUT" "$tmp" 2>&1) && status=0 || status=$?
  if [ "$status" -eq 0 ]; then
    echo "stdlib_preambles.gen.h in sync"
  elif [ "$status" -eq 1 ]; then
    echo "ERROR: stdlib_preambles.gen.h is out of sync with src/preambles/*.cul." >&2
    echo "Run \`just gen-preambles\`." >&2
    printf '%s\n' "$report" >&2
    exit 1
  else
    echo "ERROR: could not compare $OUT (diff exited $status):" >&2
    printf '%s\n' "$report" >&2
    exit "$status"
  fi
elif ! cmp -s "$tmp" "$OUT"; then
  # Written only when its text changes: `just dev` runs this before every
  # build (the bake depends on the header, not the .cul), and a new mtime
  # alone would re-bake every module.
  mv "$tmp" "$OUT"
  echo "wrote $OUT"
fi
