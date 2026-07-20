#!/usr/bin/env bash
# Build the culebra playground (interp-only, WebAssembly) into site/playground/.
# The wasm artifacts are committed so GitHub Pages ("deploy from branch") serves
# them directly. Run from the repo root (via `just site-build`).
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

INCLUDES=(
  -Iinclude
  -Ivendor/cpp-peglib -Ivendor/cpp-unicodelib -Ivendor/cpp-regexlib
  -Ivendor/cpp-tensorlib/include
)

echo "[playground] compiling culebra.wasm…"
emcc -std=c++23 -O2 -fwasm-exceptions \
  -sUSE_ZLIB=1 -sSTACK_SIZE=16MB -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB \
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createCulebra \
  -sENVIRONMENT=web,worker \
  -sEXPORTED_FUNCTIONS=_run_culebra,_get_output,_malloc,_free \
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString \
  "${INCLUDES[@]}" \
  playground/wasm_main.cc \
  -o "$OUT/culebra.js"

# Copy the static frontend alongside the wasm (brand.css lives in site/assets/).
cp playground/index.html playground/app.js playground/worker.js \
   playground/editor.js playground/culebra-lang.js playground/styles.css "$OUT/"

echo "[playground] done → $OUT/ ($(du -h "$OUT/culebra.wasm" | cut -f1) wasm)"
