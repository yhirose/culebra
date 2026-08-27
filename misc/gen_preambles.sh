#!/bin/sh
# Generate include/stdlib_preambles.gen.h from the culebra sources in
# src/preambles/*.cul (the editable single source of truth). Each .cul is
# embedded verbatim as a `<NAME>_MODULE_SOURCE` raw-string constant, so the
# single self-contained `culebra` binary keeps carrying them. Most are stdlib
# modules, listed in lazy_ns_modules() / lazy_fn_groups(); test_ambient.cul is
# not — `culebra test` runs it directly. Run after editing a .cul; `--check`
# (CI gate) fails if the committed header is stale.
set -eu
DIR=$(cd "$(dirname "$0")/.." && pwd)
SRC="$DIR/src/preambles"
OUT="$DIR/include/stdlib_preambles.gen.h"
gen() {
  printf '// Generated from src/preambles/*.cul by misc/gen_preambles.sh — do not edit.\n'
  printf '// Edit the .cul sources, then run `just gen-preambles` (CI checks sync).\n'
  printf '#pragma once\n\n'
  for base in time term canvas args matchers regex string_fns log desktop path vector2 vector3 deque priority_queue effects test_ambient; do
    name=$(printf '%s' "$base" | tr 'a-z' 'A-Z')
    printf 'inline constexpr const char* %s_MODULE_SOURCE = R"=culpre=(' "$name"
    cat "$SRC/$base.cul"
    printf ')=culpre=";\n\n'
  done
}
if [ "${1:-}" = "--check" ]; then
  # Compare two real files: BSD diff spools a `-` operand through the darwin
  # temp dir, which a sandboxed run cannot write — and that failure looked
  # identical to "out of sync". Keep could-not-compare as its own outcome.
  tmp=$(mktemp "${TMPDIR:-/tmp}/gen_preambles.XXXXXX")
  trap 'rm -f "$tmp"' EXIT
  gen > "$tmp"
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
else
  gen > "$OUT"
  echo "wrote $OUT"
fi
