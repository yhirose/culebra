#!/bin/sh
# Set up Culebra debugging for Zed.
#
# Zed has a built-in DAP client, so no extension is needed — it just needs a
# debug scenario pointing at `culebra dap`. Scenarios live in a project's
# .zed/debug.json (default), or pass --global to write the user-level
# ~/.config/zed/debug.json instead.
#
# The culebra path is baked in when it's on PATH. An existing debug.json is
# backed up to debug.json.bak rather than merged, so re-check it if you already
# had scenarios.
#
# Note: this sets up *debugging* only. Syntax highlighting in Zed needs a
# tree-sitter language extension, which is separate from the TextMate grammar
# used by VSCode and is not provided here.
#
# Usage: install.sh [--global]

set -eu

GLOBAL=0
[ "${1:-}" = "--global" ] && GLOBAL=1

CULEBRA=$(command -v culebra 2>/dev/null || echo culebra)

if [ "$GLOBAL" -eq 1 ]; then
  DEST_DIR="$HOME/.config/zed"
  WHERE="user-level ($DEST_DIR)"
else
  DEST_DIR="$PWD/.zed"
  WHERE="this project ($DEST_DIR)"
fi
DEST="$DEST_DIR/debug.json"

mkdir -p "$DEST_DIR"
if [ -f "$DEST" ]; then
  cp "$DEST" "$DEST.bak"
  echo "note: existing $DEST backed up to $DEST.bak — merge your old scenarios back if needed."
fi

cat > "$DEST" <<JSON
[
  {
    "label": "Debug current Culebra file",
    "adapter": "culebra",
    "request": "launch",
    "command": "$CULEBRA",
    "args": ["dap"],
    "program": "\$ZED_FILE",
    "stopOnEntry": false
  }
]
JSON

echo "installed Zed debug config into $WHERE (adapter: $CULEBRA dap)"
echo "  -> open a .cul file, set a breakpoint, and start \"Debug current Culebra file\""
echo "     from the debug panel (or run the 'debugger: start' action)."
