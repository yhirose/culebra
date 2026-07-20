#!/usr/bin/env bash
# Build the culebra playground (interp-only, WebAssembly) into site/playground/.
# The wasm artifacts are committed so GitHub Pages ("deploy from branch") serves
# them directly. Run from the repo root (via `just site-build`).
#
# Two builds, and worker.js picks one at load time:
#
#   culebra-cpu   plain wasm, runs anywhere.
#   culebra-gpu   adds the WebGPU Tensor backend, which needs JSPI to keep
#                 tensorlib's synchronous gpu::flush() over an async API.
#
# They exist as separate binaries because a -sJSPI build cannot even be
# instantiated without JSPI — the glue calls `new WebAssembly.Suspending()`
# while wiring imports — so a single GPU build would take the whole page down
# on Safari (and on every iOS browser, all of which are WebKit). Chrome/Edge
# 137+ and Firefox 153+ have JSPI; Safari has it in 27 beta, so the CPU build
# is expected to become unnecessary once that ships. Each visitor downloads
# exactly one of the two.
#
# Both builds share the interpreter, so they differ by roughly the size of the
# emdawnwebgpu port (tens of KB), not by a meaningful fraction.
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
  -sEXPORTED_FUNCTIONS=_run_culebra,_get_output,_malloc,_free
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString
  -Iinclude
  -Ivendor/cpp-peglib -Ivendor/cpp-unicodelib -Ivendor/cpp-regexlib
  -Ivendor/cpp-tensorlib/include -Ivendor/cpp-tensorlib/kernels
)

echo "[playground] compiling culebra-cpu.wasm…"
emcc "${COMMON[@]}" playground/wasm_main.cc -o "$OUT/culebra-cpu.js"

# JSPI (not Asyncify) because it composes with -fwasm-exceptions, which the
# interpreter needs. JSPI_EXPORTS makes run_culebra return a Promise, so
# worker.js awaits it — harmless for the CPU build, where it stays synchronous.
echo "[playground] compiling culebra-gpu.wasm (WebGPU + JSPI)…"
emcc "${COMMON[@]}" \
  --use-port=emdawnwebgpu -DTENSORLIB_WEBGPU \
  -sJSPI=1 -sJSPI_EXPORTS=run_culebra \
  playground/wasm_main.cc -o "$OUT/culebra-gpu.js"

# Copy the static frontend alongside the wasm (brand.css lives in site/assets/).
cp playground/index.html playground/app.js playground/worker.js \
   playground/editor.js playground/culebra-lang.js playground/styles.css "$OUT/"

echo "[playground] done → $OUT/ (cpu $(du -h "$OUT/culebra-cpu.wasm" | cut -f1), gpu $(du -h "$OUT/culebra-gpu.wasm" | cut -f1))"
