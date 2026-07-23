#!/usr/bin/env bash
# Build the culebra playground (interp-only, WebAssembly) into site/playground/.
# The wasm artifacts are committed so GitHub Pages ("deploy from branch") serves
# them directly. Run from the repo root (via `just site-build`).
#
# Two builds, and worker.js picks one at load time:
#
#   culebra-basic   plain wasm, runs anywhere.
#   culebra-full    adds JSPI, which unlocks two things that both need to
#                   suspend a synchronous wasm call over an async JS API:
#                   the WebGPU Tensor backend (tensorlib's gpu::flush()) and
#                   the TUI tab's Term.read_key (waiting for a keypress).
#
# They exist as separate binaries because a -sJSPI build cannot even be
# instantiated without JSPI — the glue calls `new WebAssembly.Suspending()`
# while wiring imports — so a single "full" build would take the whole page
# down on Safari (and on every iOS browser, all of which are WebKit).
# Chrome/Edge 137+ and Firefox 153+ have JSPI; Safari has it in 27 beta, so
# the basic build is expected to become unnecessary once that ships. Each
# visitor downloads exactly one of the two.
#
# Both builds share the interpreter, so they differ by roughly the size of
# the emdawnwebgpu port (tens of KB), not by a meaningful fraction. The basic
# build still runs a TUI script — Term.read_key stubs to "no key yet" (see
# term.h), so it degrades to non-interactive the way piped stdin does
# natively, instead of failing to link.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

EMSDK="${EMSDK_DIR:-$HOME/Projects/emsdk}"
if [ ! -f "$EMSDK/emsdk_env.sh" ]; then
  echo "error: emsdk not found at $EMSDK (set EMSDK_DIR)" >&2
  exit 1
fi
# shellcheck disable=SC1091
source "$EMSDK/emsdk_env.sh" >/dev/null 2>&1

OUT="site/playground"
mkdir -p "$OUT"

COMMON=(
  -std=c++23 -O2 -fwasm-exceptions
  -sUSE_ZLIB=1 -sSTACK_SIZE=16MB -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createCulebra
  -sENVIRONMENT=web,worker
  -sEXPORTED_FUNCTIONS=_run_culebra,_malloc,_free
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString
  -Iinclude
  -Ivendor/cpp-peglib -Ivendor/cpp-unicodelib -Ivendor/cpp-regexlib
  -Ivendor/cpp-tensorlib/include -Ivendor/cpp-tensorlib/kernels
)

echo "[playground] compiling culebra-basic.wasm…"
emcc "${COMMON[@]}" playground/wasm_main.cc -o "$OUT/culebra-basic.js"

# JSPI (not Asyncify) because it composes with -fwasm-exceptions, which the
# interpreter needs. JSPI_EXPORTS makes run_culebra return a Promise, so
# worker.js awaits it — harmless for the basic build, where it stays
# synchronous. CULEBRA_WASM_JSPI switches on the real Term.read_key (term.h)
# and IO.*_is_terminal (os_compat.h) instead of their "not interactive" stubs.
echo "[playground] compiling culebra-full.wasm (WebGPU + TUI, JSPI)…"
emcc "${COMMON[@]}" \
  --use-port=emdawnwebgpu -DTENSORLIB_WEBGPU -DCULEBRA_WASM_JSPI \
  -sJSPI=1 -sJSPI_EXPORTS=run_culebra \
  playground/wasm_main.cc -o "$OUT/culebra-full.js"

# Copy the static frontend alongside the wasm (brand.css lives in site/assets/).
cp playground/index.html playground/app.js playground/worker.js \
   playground/editor.js playground/culebra-lang.js playground/styles.css "$OUT/"

echo "[playground] done → $OUT/ (basic $(du -h "$OUT/culebra-basic.wasm" | cut -f1), full $(du -h "$OUT/culebra-full.wasm" | cut -f1))"
