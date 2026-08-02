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
  # Compare two real files rather than piping the generated text in as `-`:
  # BSD diff spools a stdin operand through the per-user darwin temp dir, which
  # a sandboxed run cannot write. That failure used to be swallowed by the
  # `2>&1` and reported as "out of sync", which sends you to regenerate a header
  # that was already correct. Keep the three outcomes apart: same, different,
  # and could-not-compare.
  tmp=$(mktemp "${TMPDIR:-/tmp}/gen_preambles.XXXXXX")
  tmpdiff=$(mktemp "${TMPDIR:-/tmp}/gen_preambles.XXXXXX")
  trap 'rm -f "$tmp" "$tmpdiff"' EXIT
  gen > "$tmp"
  diff -u "$OUT" "$tmp" > "$tmpdiff" 2>&1 && status=0 || status=$?
  if [ "$status" -eq 0 ]; then
    echo "stdlib_preambles.gen.h in sync"
  elif [ "$status" -eq 1 ]; then
    echo "ERROR: stdlib_preambles.gen.h is out of sync with src/preambles/*.cul." >&2
    echo "Run \`just gen-preambles\`." >&2
    cat "$tmpdiff" >&2
    exit 1
  else
    echo "ERROR: could not compare $OUT (diff exited $status):" >&2
    cat "$tmpdiff" >&2
    exit "$status"
  fi
else
  gen > "$OUT"
  echo "wrote $OUT"
fi
