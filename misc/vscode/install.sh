#!/bin/sh
# Install the Culebra VSCode extension. It provides syntax highlighting for
# .cul files and registers the `culebra` debug type (the adapter itself is
# `culebra dap`, shipped in the culebra binary).
#
# This packages the extension as a .vsix and installs it with
# `code --install-extension` — the method VS Code documents. (Copying the
# folder into ~/.vscode/extensions is NOT supported and is often not detected.)

set -eu

SRC_DIR=$(cd "$(dirname "$0")" && pwd)

# Pick the editor CLI: VS Code, or a compatible fork if that's what's installed.
CLI=""
for c in code code-insiders cursor codium; do
  if command -v "$c" >/dev/null 2>&1; then CLI=$c; break; fi
done
if [ -z "$CLI" ]; then
  echo "error: no editor CLI found (looked for: code, code-insiders, cursor, codium)." >&2
  echo "In VSCode, run Command Palette -> 'Shell Command: Install code command in PATH', then retry." >&2
  echo "Or build the package yourself and install it from the Extensions view:" >&2
  echo "  $SRC_DIR/build-vsix.sh    # prints the .vsix path" >&2
  echo "  Extensions view -> ... menu -> Install from VSIX..." >&2
  exit 1
fi

# A stray copy from the old (unsupported) folder-drop installer shadows the
# packaged one by id — remove it so only the .vsix install remains.
rm -rf "$HOME/.vscode/extensions/culebra-debug"

VSIX=$("$SRC_DIR/build-vsix.sh")
"$CLI" --install-extension "$VSIX"
rm -f "$VSIX"

echo
echo "installed via $CLI. Next:"
echo "  1. Fully quit the editor (Cmd+Q) and reopen — a reload may not pick up"
echo "     a freshly installed extension."
echo "  2. Open a .cul file: syntax highlighting applies automatically."
echo "  3. To debug, add a .vscode/launch.json (see docs/tooling.md) and"
echo "     press F5. If 'culebra' isn't on PATH, set \"program\" to its"
echo "     absolute path in the launch config."
