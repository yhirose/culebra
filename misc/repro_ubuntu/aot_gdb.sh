#!/usr/bin/env bash
# Stack trace for a `culebra build` output that dies at run time, inside the
# container image. `--keep-symbols` so the trace has names. Not part of the
# build; a reproduction aid.
set -u
cd /src
BIN=build-linux/culebra
OUT=${OUT:-/tmp/culebra-aot-gdb}
mkdir -p "$OUT"
f=$1
name=$(basename "$f" .cul)
"$BIN" build --keep-symbols "$f" -o "$OUT/$name" || exit 1
gdb -batch \
  -ex run \
  -ex 'bt 40' \
  -ex 'info registers rip rsp' \
  --args "$OUT/$name" 2>&1 | tail -60
