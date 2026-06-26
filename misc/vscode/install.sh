#!/bin/sh
# Install the Culebra VSCode extension into ~/.vscode/extensions.
# Provides syntax highlighting for .cul files and registers the `culebra`
# debug type so you can debug them with F5 (the adapter itself is
# `culebra dap`, shipped in the culebra binary).

set -eu

SRC_DIR=$(cd "$(dirname "$0")" && pwd)
SRC="$SRC_DIR/package.json"
DEST="$HOME/.vscode/extensions/culebra-debug"

if [ ! -f "$SRC" ]; then
  echo "error: $SRC not found" >&2
  exit 1
fi

mkdir -p "$DEST/syntaxes"
cp "$SRC" "$DEST/package.json"
cp "$SRC_DIR/language-configuration.json" "$DEST/language-configuration.json"
cp "$SRC_DIR/syntaxes/culebra.tmLanguage.json" \
  "$DEST/syntaxes/culebra.tmLanguage.json"

# VSCode launches the adapter by the `program` field, which defaults to plain
# "culebra" (resolved on PATH). If culebra is on PATH, bake in its absolute
# path so it works even when VSCode is launched without your shell's PATH.
CULEBRA=$(command -v culebra 2>/dev/null || true)
if [ -n "$CULEBRA" ]; then
  # BSD/macOS and GNU sed both accept -i with an explicit backup suffix.
  sed -i.bak "s|\"program\": \"culebra\"|\"program\": \"$CULEBRA\"|" \
    "$DEST/package.json"
  rm -f "$DEST/package.json.bak"
  echo "installed into $DEST (adapter: $CULEBRA dap)"
else
  echo "installed into $DEST"
  echo "note: 'culebra' is not on PATH — edit $DEST/package.json and set"
  echo "      \"program\" to the absolute path of your culebra binary."
fi

echo
echo "Next:"
echo "  1. Reload VSCode (Command Palette -> Developer: Reload Window)."
echo "  2. Add a .vscode/launch.json (see docs/debugging.md) and press F5."
