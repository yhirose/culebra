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
# examples.json is not copied but written below, each example's file list
# filled in.
cp playground/index.html playground/boot.js playground/app.js playground/worker.js \
   playground/editor.js playground/culebra-lang.js playground/share-link.js \
   playground/project.js playground/styles.css "$OUT/"

# Stamp the version into the copy, reading the one place that defines it. The
# source index.html keeps the placeholder — it is never served directly, only
# this copy is. Not `sed -i`: its in-place syntax differs between BSD and GNU.
version="$(sed -n 's/^#define CULEBRA_VERSION "\([^"]*\)"/\1/p' include/culebra.h)"
[ -n "$version" ] || { echo "error: no CULEBRA_VERSION in include/culebra.h" >&2; exit 1; }
sed "s/{{CULEBRA_VERSION}}/v$version/g" "$OUT/index.html" >"$OUT/index.html.tmp"
mv "$OUT/index.html.tmp" "$OUT/index.html"

# examples.json's "path" doubles as the source location (relative to the repo
# root) and the fetch path the browser uses (relative to $OUT), so mirroring
# under $OUT keeps both readings true. An example in its own directory
# (<name>/<name>.cul) brings the whole directory — imported modules and data
# alike — and the served copy gets that list as `assets` for the worker to
# fetch; the source examples.json names no file by hand. The directory is the
# unit because a program finds its files by its own path, so anything beside
# it is fair game; the naming rule is what keeps examples/tensor/tensors.cul
# from dragging mnist/weights/ along. Dotfiles are skipped so a .DS_Store
# cannot make one machine's copy differ from another's.
echo "[playground] copying example sources and assets…"
python3 - "$OUT" <<'PY'
import json, pathlib, shutil, sys

out = pathlib.Path(sys.argv[1])
catalog = json.loads(pathlib.Path("playground/examples.json").read_text())
missing, n = [], 0
for category in catalog["categories"]:
  for example in category["examples"]:
    entry = pathlib.Path(example["path"])
    if not entry.is_file():
      missing.append("%s (%s)" % (entry, example["title"]))
      continue
    own = entry.parent.name == entry.stem
    def visible(p):  # no dotfile in any segment: .DS_Store, .cache/, .venv/
      return p.is_file() and not any(s.startswith(".") for s in p.relative_to(entry.parent).parts)
    files = sorted(filter(visible, entry.parent.rglob("*")), key=str) if own else [entry]
    example["assets"] = [str(p) for p in files if p != entry]
    for src in files:
      dst = out / src
      dst.parent.mkdir(parents=True, exist_ok=True)
      shutil.copyfile(src, dst)
      n += 1
if missing:
  sys.exit("examples.json names files that do not exist:\n  " + "\n  ".join(missing))
(out / "examples.json").write_text(json.dumps(catalog, indent=2, ensure_ascii=False) + "\n")
print("[playground]   %d file(s)" % n)
PY
