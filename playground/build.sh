#!/usr/bin/env bash
# Build the culebra playground (interp-only, WebAssembly) into site/playground/.
# The wasm artifacts are committed so GitHub Pages ("deploy from branch") serves
# them directly. Run from the repo root (via `just site-build`). Only a changed
# input triggers a recompile, so editing the frontend and re-running
# `just site-serve` costs a copy rather than two interpreter builds.
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
EMCC_VERSION="$(emcc --version | sed -n 1p)"

# emcc routes clang through EM_COMPILER_WRAPPER, so a content-identical TU (a
# fresh worktree, a reverted edit) reuses the object instead of recompiling.
# CCACHE_BASEDIR — exported by the justfile — is what makes entries shared
# across worktrees rather than keyed on each checkout's absolute paths.
if command -v ccache >/dev/null; then
  export EM_COMPILER_WRAPPER=ccache
fi

OUT="site/playground"
mkdir -p "$OUT"

COMMON=(
  -std=c++23 -O2 -fwasm-exceptions
  -sUSE_ZLIB=1 -sSTACK_SIZE=16MB -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createCulebra
  -sENVIRONMENT=web,worker
  -sEXPORTED_FUNCTIONS=_run_culebra,_malloc,_free
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,FS
  # A program reads its assets with FS.read, and resolves `import` against its
  # own path — both need a filesystem. Nothing is preloaded: worker.js fetches
  # what examples.json lists and writes it into MEMFS before the run.
  -sFORCE_FILESYSTEM=1
  -Iinclude
  -Ivendor/cpp-peglib -Ivendor/cpp-unicodelib -Ivendor/cpp-regexlib
  -Ivendor/cpp-tensorlib/include -Ivendor/cpp-tensorlib/kernels
  -Ivendor/stb
)

# Each build is the whole interpreter in one TU, and `just site-serve` asks for
# one on every invocation, so skip a compile whose inputs are all unchanged.
# The file list comes from the compiler (-MD) rather than being enumerated
# here, which is what covers the generated preambles and the vendored headers;
# a header that newly enters the TU can only arrive through a file already in
# the list, so a stale list cannot hide an edit.
fingerprint() {  # $1 = dep file, $2 = output .js, $3… = the emcc command line
  local dep=$1 out=$2; shift 2
  { printf '%s\n' "$EMCC_VERSION" "$@"
    # "out.o: a.cc b.h \" + continuation lines → one path per line. The outputs
    # ride along because they are committed: checking out an older wasm has to
    # invalidate the stamp too, not just an edited input.
    { sed -e 's/^[^:]*://' -e 's/\\$//' "$dep"
      printf '%s\n%s\n' "$out" "${out%.js}.wasm"; } |
      tr ' ' '\n' | sed '/^$/d' | sort -u | tr '\n' '\0' | xargs -0 shasum -a 256 2>/dev/null
  } | shasum -a 256 | cut -d' ' -f1
}

compile() {  # $1 = label, $2 = output .js, $3… = flags on top of COMMON
  local label=$1 out=$2; shift 2
  local dep="${out%.js}.d" stamp="${out%.js}.stamp" want
  local cmd=("${COMMON[@]}" "$@" playground/wasm_main.cc -o "$out")
  # A vanished input makes fingerprint fail, which reads as "rebuild".
  if [ -s "$dep" ] && [ -f "$stamp" ] && [ -f "${out%.js}.wasm" ] &&
     want=$(fingerprint "$dep" "$out" "${cmd[@]}") && [ "$want" = "$(cat "$stamp")" ]; then
    echo "[playground] $label is up to date"
    return
  fi
  echo "[playground] compiling ${label}…"
  emcc "${cmd[@]}" -MD -MF "$dep"
  fingerprint "$dep" "$out" "${cmd[@]}" >"$stamp" || rm -f "$stamp"
}

compile "culebra-basic.wasm" "$OUT/culebra-basic.js"

# JSPI (not Asyncify) because it composes with -fwasm-exceptions, which the
# interpreter needs. JSPI_EXPORTS makes run_culebra return a Promise, so
# worker.js awaits it — harmless for the basic build, where it stays
# synchronous. CULEBRA_WASM_JSPI switches on the real Term.read_key (term.h)
# and IO.*_is_terminal (os_compat.h) instead of their "not interactive" stubs.
compile "culebra-full.wasm (WebGPU + TUI, JSPI)" "$OUT/culebra-full.js" \
  --use-port=emdawnwebgpu -DTENSORLIB_WEBGPU -DCULEBRA_WASM_JSPI \
  -sJSPI=1 -sJSPI_EXPORTS=run_culebra

# Copy the static frontend alongside the wasm (brand.css lives in site/assets/).
cp playground/index.html playground/app.js playground/worker.js \
   playground/editor.js playground/culebra-lang.js playground/styles.css \
   playground/examples.json "$OUT/"

# Stamp the version into the copy, reading the one place that defines it. The
# source index.html keeps the placeholder — it is never served directly, only
# this copy is. Not `sed -i`: its in-place syntax differs between BSD and GNU.
version="$(sed -n 's/^#define CULEBRA_VERSION "\([^"]*\)"/\1/p' include/culebra.h)"
[ -n "$version" ] || { echo "error: no CULEBRA_VERSION in include/culebra.h" >&2; exit 1; }
sed "s/{{CULEBRA_VERSION}}/v$version/g" "$OUT/index.html" >"$OUT/index.html.tmp"
mv "$OUT/index.html.tmp" "$OUT/index.html"

# examples.json's "path" and "assets" fields double as both the source location
# (relative to the repo root) and the fetch path the browser uses (relative to
# $OUT) — mirror everything they name under $OUT so both readings hold. The
# worker fetches from this same list, so nothing can be copied but not fetched,
# or fetched but not copied.
echo "[playground] copying example sources and assets…"
python3 - "$OUT" <<'PY'
import json, pathlib, shutil, sys

out = pathlib.Path(sys.argv[1])
catalog = json.loads((out / "examples.json").read_text())
missing, n = [], 0
for category in catalog["categories"]:
  for example in category["examples"]:
    for rel in [example["path"]] + example.get("assets", []):
      src = pathlib.Path(rel)
      if not src.is_file():
        missing.append("%s (%s)" % (rel, example["title"]))
        continue
      dst = out / rel
      dst.parent.mkdir(parents=True, exist_ok=True)
      shutil.copyfile(src, dst)
      n += 1
if missing:
  sys.exit("examples.json names files that do not exist:\n  " + "\n  ".join(missing))
print("[playground]   %d file(s)" % n)
PY

echo "[playground] done → $OUT/ (basic $(du -h "$OUT/culebra-basic.wasm" | cut -f1), full $(du -h "$OUT/culebra-full.wasm" | cut -f1))"
