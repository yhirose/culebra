#!/usr/bin/env bash
# cmake/gen_pe_exports.cmake, run against a fixed nm output.
#
# That script builds the PE export table the Windows JIT resolves its helpers
# through, and it runs on no other platform — so a change to it links fine
# everywhere local dev builds and produces a short .def on Windows alone, where
# the symptom is every JIT'd program failing to find a runtime helper. (It has
# already happened once: two MATCHES in one if() clobber CMAKE_MATCH_*, and the
# generator emitted nothing.) Feeding it a COFF-shaped listing off a stub `nm`
# exercises the whole script anywhere, in well under a second.
#
# Deliberately not real objects: ELF gives an inline `used` helper class W and
# COFF gives it T, so the classification this checks can only be posed by hand.
set -euo pipefail
cd "$(dirname "$0")/.."

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cat > "$work/nm" <<'EOF'
#!/bin/bash
cat <<'SYMS'

obj1.obj:
0000000000000000 T culebra_runtime_object_new
0000000000000000 T culebra_runtime_cell_new
0000000000000000 B culebra_g_wake
0000000000000000 T _ZN7culebra3fooEv
0000000000000000 T main

obj2.obj:
0000000000000000 T culebra_runtime_object_new
0000000000000000 D culebra_g_sigint
SYMS
EOF
chmod +x "$work/nm"

cmake -DNM="$work/nm" -DLIST=cmake/exported_symbols.txt -DOUT="$work/out.def" \
      -DOBJECTS=ignored.obj -P cmake/gen_pe_exports.cmake > /dev/null

# Whitelisted names only (no C++ mangling, no `main`), deduplicated, data
# marked DATA — without which the .def exports a thunk's address, not the
# variable's.
cat > "$work/want.def" <<'EOF'
EXPORTS
  culebra_runtime_cell_new
  culebra_runtime_object_new
  culebra_g_sigint DATA
  culebra_g_wake DATA
EOF

if ! diff -u "$work/want.def" "$work/out.def"; then
  echo "check_pe_exports_gen: cmake/gen_pe_exports.cmake emitted the above" >&2
  exit 1
fi
echo "pe-exports-gen OK"
