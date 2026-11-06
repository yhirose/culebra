#!/usr/bin/env bash
# Baked stdlib AST gate (stdlib/preamble_ast.h, culebra_preamble_cc --ast).
# The executor builds its `<stdlib>` module from the ASTs the build baked
# instead of parsing the source; losing them is silent (it parses again, tens
# of milliseconds slower per start), and a stale one would compile what the
# source no longer says. CULEBRA_CHECK_BAKED_AST=1 holds every baked entry
# to its parse and fails on one missing where the build bakes; the program
# only has to name a module for the check to run.
#
# Usage: tools/checks/check_baked_ast.sh <culebra binary>
set -euo pipefail

bin="${1:?usage: check_baked_ast.sh <culebra binary>}"
work=$(mktemp -d "${TMPDIR:-/tmp}/culebra-baked-ast.XXXXXX")
trap 'rm -rf "$work"' EXIT

cat > "$work/t.cul" <<'EOF'
IO.print(Time.seconds(90) == Time.minutes(1) + Time.seconds(30))
EOF

baked=$(CULEBRA_CHECK_BAKED_AST=1 "$bin" --vm "$work/t.cul") || {
  echo "check_baked_ast FAIL: the baked ASTs do not match the parse (above)" >&2
  exit 1
}
parsed=$(CULEBRA_PREAMBLE_SOURCE=1 "$bin" --vm "$work/t.cul")
if [[ "$baked" != "$parsed" ]]; then
  echo "check_baked_ast FAIL: baked [$baked], parsed [$parsed]" >&2
  exit 1
fi
echo "baked-ast OK (every baked stdlib AST matches its parse)"
