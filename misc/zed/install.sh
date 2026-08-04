#!/bin/sh
# Set up Culebra for Zed: syntax highlighting AND debugging.
#
# Neither can be pure config in Zed: highlighting needs a Tree-sitter grammar,
# and a debug adapter must be registered by an extension (a small WASM shim that
# tells Zed to launch `culebra dap`). So both ship as one dev extension. This
# script generates that extension, points you at it to install, and writes the
# project's debug scenarios to .zed/debug.json.
#
# Usage: install.sh [extension-output-dir]

set -eu

SRC_DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(git -C "$SRC_DIR" rev-parse --show-toplevel)
REV=$(git -C "$REPO" rev-parse HEAD)
EXT=${1:-"${XDG_DATA_HOME:-$HOME/.local/share}/culebra-zed-extension"}

# The grammar is fetched from the repo by commit, so it must be committed.
if ! git -C "$REPO" cat-file -e "$REV:misc/zed/tree-sitter-culebra/src/parser.c" 2>/dev/null; then
  echo "error: the grammar (misc/zed/tree-sitter-culebra/src/parser.c) is not committed at HEAD." >&2
  echo "Commit it first — Zed fetches the grammar from the repo by commit." >&2
  exit 1
fi

# Zed compiles the debug-adapter shim to wasm32-wasip2 when installing the dev
# extension; without that Rust target the build fails ("can't find crate for
# core") and the adapter never registers. Warn early — we can't add it for you.
if command -v rustup >/dev/null 2>&1; then
  if ! rustup target list --installed 2>/dev/null | grep -q '^wasm32-wasip2$'; then
    echo "warning: Rust target wasm32-wasip2 is not installed — Zed needs it to" >&2
    echo "         build the debug adapter. Run:  rustup target add wasm32-wasip2" >&2
  fi
else
  echo "warning: rustup not found — Zed needs Rust + the wasm32-wasip2 target to" >&2
  echo "         build the debug adapter (highlighting still works without it)." >&2
fi

# 1. Generate the dev extension (grammar via git, language config + Rust adapter
#    shim copied in; Zed compiles the Rust to WASM on install).
rm -rf "$EXT"
mkdir -p "$EXT/languages" "$EXT/src" "$EXT/debug_adapter_schemas"
cp -R "$SRC_DIR/languages/." "$EXT/languages/"
cp "$SRC_DIR/Cargo.toml" "$EXT/Cargo.toml"
cp "$SRC_DIR/src/culebra.rs" "$EXT/src/culebra.rs"
# Zed reads a JSON Schema for the adapter's launch config from this default path
# (debug_adapter_schemas/<adapter>.json); without it the adapter fails to load.
cp "$SRC_DIR/debug_adapter_schemas/culebra.json" \
  "$EXT/debug_adapter_schemas/culebra.json"
sed -e "s|{{REPOSITORY}}|file://$REPO|" -e "s|{{REV}}|$REV|" \
  "$SRC_DIR/extension.toml.template" > "$EXT/extension.toml"

# 2. Write the project's debug scenarios. The adapter binary is supplied by the
#    extension above, so the scenario only names the adapter + the program.
DBG_DIR="$PWD/.zed"
mkdir -p "$DBG_DIR"
if [ -f "$DBG_DIR/debug.json" ]; then
  cp "$DBG_DIR/debug.json" "$DBG_DIR/debug.json.bak"
  echo "note: existing $DBG_DIR/debug.json backed up to debug.json.bak — merge if needed."
fi
cp "$SRC_DIR/debug.json" "$DBG_DIR/debug.json"

echo "wrote Zed extension to:   $EXT  (grammar @ ${REV})"
echo "wrote debug scenarios to: $DBG_DIR/debug.json"
echo
echo "Install the extension in Zed (one time):"
echo "  1. Command palette -> 'zed: install dev extension' -> select: $EXT"
echo "     (Zed compiles the Rust adapter shim to WASM; needs a recent Zed.)"
echo "  2. Re-run this script and re-select the directory after grammar/adapter changes."
echo
echo "Then: open a .cul file -> syntax highlights; set a breakpoint and run"
echo "      'Debug current Culebra file' from the debug panel."
