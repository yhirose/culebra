#!/usr/bin/env bash
# The Playground's static half, staged into $1: the frontend files, the
# version stamped into index.html, and the example sources examples.json
# names. Its own script because it needs no emsdk — build.sh runs it after
# the wasm, and the sync gate (tools/checks/check_site_playground_sync.sh)
# stages a copy to diff the committed site/playground/ against.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

OUT="${1:?usage: copy-frontend.sh <outdir>}"
mkdir -p "$OUT"

# The frontend alongside the wasm (brand.css lives in site/assets/).
cp playground/index.html playground/app.js playground/worker.js \
   playground/editor.js playground/culebra-lang.js playground/share-link.js \
   playground/styles.css playground/examples.json "$OUT/"

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
