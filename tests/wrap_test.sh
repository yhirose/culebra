#!/usr/bin/env bash
# End-to-end test for `culebra wrap` (Phase 4 / P4-3): build
# an extended binary from the examples/wrap declaration, then assert the
# wrapped class behaves identically under --vm, --jit, and an AOT
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
# Two binding TUs in one extended binary: the Vec2 example, and the probe that
# pins how the wrap layer treats each kind of exception a wrapped body raises
# (tests/wrap/interrupt_binding.cpp). One build covers both — this phase is the
# slow, toolchain-sensitive one, so a second ext-culebra would not be free.
if ! "$CULEBRA" wrap "$ROOT/examples/wrap/vec2_binding.cpp" \
                     "$ROOT/tests/wrap/interrupt_binding.cpp" -o "$EXT"; then
  echo "wrap_test FAIL: culebra wrap did not produce a binary" >&2
  exit 1
fi

DEMO="$ROOT/examples/wrap/demo.cul"
if ! "$EXT" --vm "$DEMO" > "$OUT/vm.txt" 2>&1; then
  echo "wrap_test FAIL: extended binary (--vm) crashed:" >&2
  cat "$OUT/vm.txt" >&2
  exit 1
fi
if ! "$EXT" --jit "$DEMO" > "$OUT/jit.txt" 2>&1; then
  echo "wrap_test FAIL: extended binary (--jit) crashed:" >&2
  cat "$OUT/jit.txt" >&2
  exit 1
fi
if ! diff "$OUT/vm.txt" "$OUT/jit.txt"; then
  echo "wrap_test FAIL: vm vs jit output diverged" >&2
  exit 1
fi

# The stock binary must NOT know the wrapped namespace (proves the
# binding came from the declaration, not from the core).
if "$CULEBRA" --vm "$DEMO" > /dev/null 2>&1; then
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

# What the wrap layer does with each kind of exception a wrapped body raises.
# rethrow_as_culebra turns a std::exception into a catchable RuntimeError; an
# interrupt is not one (culebra::Interrupted derives from nothing), so it must
# unwind straight through instead of being reported as a program error. Both
# arms are asserted on every backend, and the AOT one is the only lane where
# the conversion lives in the linked archive rather than this binary.
CAUGHT="$ROOT/tests/wrap/interrupt_caught.cul"
UNCAUGHT="$ROOT/tests/wrap/interrupt_uncaught.cul"
CAUGHT_WANT='Interrupted|RuntimeError: native failure|alive|'

if ! "$EXT" build "$CAUGHT" -o "$OUT/caught-bin"; then
  echo "wrap_test FAIL: ext-culebra build of the interrupt probe failed" >&2
  exit 1
fi
if ! "$EXT" build "$UNCAUGHT" -o "$OUT/uncaught-bin"; then
  echo "wrap_test FAIL: ext-culebra build of the uncaught probe failed" >&2
  exit 1
fi

# Output through a file, never a pipe: the exit code matters in both checks,
# and a pipeline reports the tail's.
check_caught() {
  local desc="$1"; shift
  local outf="$OUT/caught.$desc"
  "$@" > "$outf" 2>&1
  local code=$?
  local got; got="$(tr '\n' '|' < "$outf")"
  if [ "$code" != 0 ] || [ "$got" != "$CAUGHT_WANT" ]; then
    echo "wrap_test FAIL: interrupt through wrap [$desc]: exit=$code out='$got'" >&2
    echo "                                    want: exit=0 out='$CAUGHT_WANT'" >&2
    exit 1
  fi
}
check_caught "vm"  "$EXT" --vm  "$CAUGHT"
check_caught "jit" "$EXT" --jit "$CAUGHT"
check_caught "aot" "$OUT/caught-bin"

# Uncaught: the top-level defer still runs and the process ends at 130. A wrap
# layer that swallowed the interrupt would exit 0 after printing AFTER, and one
# that converted it would exit 1 with an error line.
check_uncaught() {
  local desc="$1"; shift
  local outf="$OUT/uncaught.$desc"
  "$@" > "$outf" 2>&1
  local code=$?
  local out; out="$(tr '\n' '|' < "$outf")"
  if [ "$code" != 130 ] || [ "$out" != 'DEFER|interrupted|' ]; then
    echo "wrap_test FAIL: uncaught interrupt through wrap [$desc]:" >&2
    echo "                exit=$code out='$out' (want exit=130 'DEFER|interrupted|')" >&2
    exit 1
  fi
}
check_uncaught "vm"  "$EXT" --vm  "$UNCAUGHT"
check_uncaught "jit" "$EXT" --jit "$UNCAUGHT"
check_uncaught "aot" "$OUT/uncaught-bin"

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
