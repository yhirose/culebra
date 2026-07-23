#!/bin/sh
# Generate include/stdlib_preambles.gen.h from the stdlib preamble sources in
# src/preambles/*.cul (the editable single source of truth). Each .cul is
# embedded verbatim as a `<NAME>_MODULE_SOURCE` raw-string constant, so the
# single self-contained `culebra` binary keeps carrying them. Run after editing
# a .cul; `--check` (CI gate) fails if the committed header is stale.
set -eu
DIR=$(cd "$(dirname "$0")/.." && pwd)
SRC="$DIR/src/preambles"
OUT="$DIR/include/stdlib_preambles.gen.h"
gen() {
  printf '// Generated from src/preambles/*.cul by misc/gen_preambles.sh — do not edit.\n'
  printf '// Edit the .cul sources, then run `just gen-preambles` (CI checks sync).\n'
  printf '#pragma once\n\n'
  for base in time term canvas args matchers regex string_replace log desktop path effects; do
    name=$(printf '%s' "$base" | tr 'a-z' 'A-Z')
    printf 'inline constexpr const char* %s_MODULE_SOURCE = R"=culpre=(' "$name"
    cat "$SRC/$base.cul"
    printf ')=culpre=";\n\n'
  done
}
if [ "${1:-}" = "--check" ]; then
  if gen | diff -u "$OUT" - >/dev/null 2>&1; then
    echo "stdlib_preambles.gen.h in sync"
  else
    echo "ERROR: stdlib_preambles.gen.h is out of sync with src/preambles/*.cul." >&2
    echo "Run \`just gen-preambles\`." >&2
    gen | diff -u "$OUT" - >&2 || true
    exit 1
  fi
else
  gen > "$OUT"
  echo "wrote $OUT"
fi
