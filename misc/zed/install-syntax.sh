#!/bin/sh
# Generate a Zed dev extension that gives .cul files syntax highlighting, then
# point you at it to install.
#
# Zed needs a Tree-sitter grammar (it can't use the VSCode TextMate grammar).
# The grammar lives in this repo at misc/zed/tree-sitter-culebra (with a
# committed src/parser.c); Zed fetches it from the repo by commit. This script
# writes an extension directory whose extension.toml references the grammar via
# a file:// URL + the current commit, copies the language config, and tells you
# to install it as a dev extension.
#
# Usage: install-syntax.sh [output-dir]

set -eu

SRC_DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(git -C "$SRC_DIR" rev-parse --show-toplevel)
REV=$(git -C "$REPO" rev-parse HEAD)
OUT=${1:-"${XDG_DATA_HOME:-$HOME/.local/share}/culebra-zed-extension"}

# The grammar must exist at this commit (Zed clones the repo at REV), so warn if
# it isn't committed yet.
if ! git -C "$REPO" cat-file -e "$REV:misc/zed/tree-sitter-culebra/src/parser.c" 2>/dev/null; then
  echo "error: the grammar (misc/zed/tree-sitter-culebra/src/parser.c) is not committed at HEAD." >&2
  echo "Commit it first — Zed fetches the grammar from the repo by commit." >&2
  exit 1
fi

mkdir -p "$OUT/languages"
cp -R "$SRC_DIR/languages/." "$OUT/languages/"

cat > "$OUT/extension.toml" <<TOML
id = "culebra"
name = "Culebra"
version = "0.0.1"
schema_version = 1
description = "Syntax highlighting for the Culebra language."

[grammars.culebra]
repository = "file://$REPO"
rev = "$REV"
path = "misc/zed/tree-sitter-culebra"
TOML

echo "wrote Zed dev extension to: $OUT"
echo "  grammar: file://$REPO @ $REV (misc/zed/tree-sitter-culebra)"
echo
echo "Install it in Zed:"
echo "  1. Open the command palette and run: zed: install dev extension"
echo "  2. Select the directory: $OUT"
echo "  3. Open a .cul file — highlighting applies."
echo
echo "Re-run this after pulling grammar changes to bump the pinned commit."
