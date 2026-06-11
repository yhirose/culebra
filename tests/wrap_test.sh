#!/usr/bin/env bash
# End-to-end test for `culebra wrap` (design §14.3 Phase 4 / P4-3): build
# an extended binary from the examples/wrap declaration, then assert the
# wrapped class behaves identically under interp, --jit, and an AOT
# binary produced BY the extended binary. Requires a source checkout
# (the path baked into the driver) and a working cmake toolchain — this
# is the toolchain-sensitive phase, exercised on both CI OSes.
#
# Usage: wrap_test.sh <path-to-culebra>
set -u
CULEBRA="${1:?usage: wrap_test.sh <culebra-binary>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

# Reuse ccache when available so the tree rebuild is incremental (CI
# sets these job-wide already; harmless if ccache is absent).
if command -v ccache > /dev/null 2>&1; then
  export CMAKE_C_COMPILER_LAUNCHER="${CMAKE_C_COMPILER_LAUNCHER:-ccache}"
  export CMAKE_CXX_COMPILER_LAUNCHER="${CMAKE_CXX_COMPILER_LAUNCHER:-ccache}"
fi

EXT="$OUT/ext-culebra"
if ! "$CULEBRA" wrap "$ROOT/examples/wrap/vec2_binding.cpp" -o "$EXT"; then
  echo "wrap_test FAIL: culebra wrap did not produce a binary" >&2
  exit 1
fi

DEMO="$ROOT/examples/wrap/demo.cul"
if ! "$EXT" "$DEMO" > "$OUT/interp.txt" 2>&1; then
  echo "wrap_test FAIL: extended binary (interp) crashed:" >&2
  cat "$OUT/interp.txt" >&2
  exit 1
fi
if ! "$EXT" --jit "$DEMO" > "$OUT/jit.txt" 2>&1; then
  echo "wrap_test FAIL: extended binary (--jit) crashed:" >&2
  cat "$OUT/jit.txt" >&2
  exit 1
fi
if ! diff "$OUT/interp.txt" "$OUT/jit.txt"; then
  echo "wrap_test FAIL: interp vs jit output diverged" >&2
  exit 1
fi

# The stock binary must NOT know the wrapped namespace (proves the
# binding came from the declaration, not from the core).
if "$CULEBRA" "$DEMO" > /dev/null 2>&1; then
  echo "wrap_test FAIL: stock binary unexpectedly resolves Geo.Vec2" >&2
  exit 1
fi

# AOT: the extended binary's embedded wrap archive must carry the
# registration into standalone binaries.
if ! "$EXT" build "$DEMO" -o "$OUT/demo-bin"; then
  echo "wrap_test FAIL: ext-culebra build failed" >&2
  exit 1
fi
if ! "$OUT/demo-bin" > "$OUT/aot.txt" 2>&1; then
  echo "wrap_test FAIL: AOT binary crashed:" >&2
  cat "$OUT/aot.txt" >&2
  exit 1
fi
if ! diff "$OUT/jit.txt" "$OUT/aot.txt"; then
  echo "wrap_test FAIL: jit vs AOT output diverged" >&2
  exit 1
fi

echo "wrap_test OK"
