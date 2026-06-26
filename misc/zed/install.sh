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

# 1. Generate the dev extension (grammar via git, language config + Rust adapter
#    shim copied in; Zed compiles the Rust to WASM on install).
rm -rf "$EXT"
mkdir -p "$EXT/languages" "$EXT/src"
cp -R "$SRC_DIR/languages/." "$EXT/languages/"
cp "$SRC_DIR/Cargo.toml" "$EXT/Cargo.toml"
cp "$SRC_DIR/src/culebra.rs" "$EXT/src/culebra.rs"
cat > "$EXT/extension.toml" <<TOML
id = "culebra"
name = "Culebra"
version = "0.0.1"
schema_version = 1
description = "Syntax highlighting and debugging for the Culebra language."

[grammars.culebra]
repository = "file://$REPO"
rev = "$REV"
path = "misc/zed/tree-sitter-culebra"

[debug_adapters.culebra]
TOML

# 2. Write the project's debug scenarios. The adapter binary is supplied by the
#    extension above, so the scenario only names the adapter + the program.
DBG_DIR="$PWD/.zed"
mkdir -p "$DBG_DIR"
if [ -f "$DBG_DIR/debug.json" ]; then
  cp "$DBG_DIR/debug.json" "$DBG_DIR/debug.json.bak"
  echo "note: existing $DBG_DIR/debug.json backed up to debug.json.bak — merge if needed."
fi
cat > "$DBG_DIR/debug.json" <<'JSON'
[
  {
    "label": "Debug current Culebra file",
    "adapter": "culebra",
    "request": "launch",
    "program": "$ZED_FILE",
    "cwd": "$ZED_WORKTREE_ROOT",
    "stopOnEntry": false
  }
]
JSON

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
