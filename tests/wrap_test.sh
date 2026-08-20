#!/usr/bin/env bash
# End-to-end test for `culebra wrap` (Phase 4 / P4-3): build
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
if ! "$EXT" --tree "$DEMO" > "$OUT/interp.txt" 2>&1; then
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
if "$CULEBRA" --tree "$DEMO" > /dev/null 2>&1; then
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

# Usage-gated link: a program that names NO wrapped namespace must not pull
# the wrap archive (or the wrapped library's link flags) into its AOT binary
# — the Http/OpenSSL gating, applied to wrap. The binding still works when
# used (asserted above); here we prove the unused case costs nothing. A
# no-wrap binary must build, run, and be strictly smaller than the wrap-using
# one (it omits the registrar object the latter force-loads).
printf 'print("plain\\n")\n' > "$OUT/plain.cul"
if ! "$EXT" build "$OUT/plain.cul" -o "$OUT/plain-bin"; then
  echo "wrap_test FAIL: ext-culebra build of a no-wrap program failed" >&2
  exit 1
fi
if ! "$OUT/plain-bin" > "$OUT/plain.txt" 2>&1 || [ "$(cat "$OUT/plain.txt")" != "plain" ]; then
  echo "wrap_test FAIL: no-wrap AOT binary did not run correctly:" >&2
  cat "$OUT/plain.txt" >&2
  exit 1
fi
sz_wrap=$(wc -c < "$OUT/demo-bin")
sz_plain=$(wc -c < "$OUT/plain-bin")
if [ "$sz_plain" -ge "$sz_wrap" ]; then
  echo "wrap_test FAIL: no-wrap binary ($sz_plain B) not smaller than wrap binary ($sz_wrap B) — wrap archive linked despite no use" >&2
  exit 1
fi

echo "wrap_test OK"
