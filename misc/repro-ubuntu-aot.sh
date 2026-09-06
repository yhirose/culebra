#!/usr/bin/env bash
# The AOT lane `just test aot` runs, for one file at a time, inside the
# container image: `culebra build <file>` and then the binary it produced.
# Not part of the build; a reproduction aid.
#
# A built binary's stdout is fully buffered when it is redirected, so a crash
# loses everything it printed. Bisecting for the line that crashes needs
# `IO.eprintln` markers (stderr is unbuffered), not `inspect`.
set -u
cd /src
BIN=build-linux/culebra
OUT=${OUT:-/tmp/culebra-aot-repro}
mkdir -p "$OUT"

for f in "$@"; do
  name=$(basename "$f" .cul)
  if ! "$BIN" build "$f" -o "$OUT/$name" > "$OUT/$name.build.err" 2>&1; then
    echo "FAIL $name (build)"
    tail -10 "$OUT/$name.build.err"
    continue
  fi
  "$OUT/$name" > "$OUT/$name.out" 2>&1
  rc=$?
  if [ "$rc" -eq 0 ]; then
    echo "OK   $name"
  else
    echo "FAIL $name (rc=$rc)"
    tail -5 "$OUT/$name.out"
  fi
done
