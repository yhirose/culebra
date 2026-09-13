#!/usr/bin/env bash
# Bundles the Playground's third-party front end into playground/vendor/,
# so the page serves CodeMirror and xterm itself rather than importing them
# from esm.sh at run time. Three reasons, in order: the page then loads with
# no third party up, works from a file:// checkout, and shows the same bytes
# to everyone who checks out the same commit. The security angle is last,
# and real: a CDN that turns hostile is the one way this page could run
# something it does not ship.
#
# Why not subresource integrity, which pins bytes without vendoring: esm.sh
# resolves codemirror's `^6.0.0` ranges afresh on each request, so the bytes
# it serves are not stable enough to hash, and pinning them exactly loads a
# second copy of a package and breaks instanceof across extensions (the
# history is in git for playground/editor.js). A bundle has one copy of
# everything, which is also why that problem is gone.
#
# --ignore-scripts: a package's install hooks are the one place code from the
# registry would run outside the browser, on the machine doing the build, and
# nothing here needs them — esbuild's platform binary arrives as an optional
# dependency of its own, and its postinstall only checks that it did.
#
# Inputs are vendor/package.json (exact versions) and vendor/package-lock.json
# (the resolution of everything under them, written on the first run and
# committed with the bundle). Outputs are the three files copy-frontend.sh
# carries and VERSIONS, the resolved tree for a reader. Needs node and npm;
# like build.sh's emsdk, that is a toolchain the CI does not have, and the
# outputs are committed. Run it as `just site-vendor`.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

VENDOR=playground/vendor
WORK="$(mktemp -d "${TMPDIR:-/tmp}/pg-vendor.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

cp "$VENDOR/package.json" "$WORK/"
if [ -f "$VENDOR/package-lock.json" ]; then
  cp "$VENDOR/package-lock.json" "$WORK/"
  (cd "$WORK" && npm ci --silent --no-audit --no-fund --ignore-scripts)
else
  (cd "$WORK" && npm install --silent --no-audit --no-fund --ignore-scripts)
  cp "$WORK/package-lock.json" "$VENDOR/"
fi

# One entry per bundle, naming what the Playground imports and nothing more.
# editor.js and culebra-lang.js import from codemirror.js; app.js from
# xterm.js. A name added to those imports is added here.
cat >"$WORK/codemirror.entry.js" <<'JS'
export { EditorView, basicSetup } from "codemirror";
export { keymap, Decoration } from "@codemirror/view";
export { StateField, StateEffect } from "@codemirror/state";
export { indentWithTab } from "@codemirror/commands";
export { syntaxHighlighting, StreamLanguage, HighlightStyle } from "@codemirror/language";
export { tags } from "@lezer/highlight";
JS
cat >"$WORK/xterm.entry.js" <<'JS'
export { Terminal } from "@xterm/xterm";
JS

ESBUILD="$WORK/node_modules/.bin/esbuild"
for name in codemirror xterm; do
  "$ESBUILD" "$WORK/$name.entry.js" --bundle --format=esm --minify \
    --target=es2020 --log-level=warning --outfile="$VENDOR/$name.js"
done
cp "$WORK/node_modules/@xterm/xterm/css/xterm.css" "$VENDOR/xterm.css"

# The resolved tree, for whoever wonders what is inside the minified files.
{
  echo "# Written by playground/vendor.sh. The bundle is built from these."
  # The first line names the scratch directory, which differs on every run.
  (cd "$WORK" && npm ls --all --depth=99 2>/dev/null | sed -e '1s|@ .*$||' -e 's/ deduped$//')
} >"$VENDOR/VERSIONS"

echo "[vendor] $(du -h "$VENDOR/codemirror.js" | cut -f1) codemirror.js, $(du -h "$VENDOR/xterm.js" | cut -f1) xterm.js"
