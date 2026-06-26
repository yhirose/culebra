#!/bin/sh
# Build a .vsix package for the Culebra VSCode extension from this directory.
# Needs only `zip` (no npm / vsce). A .vsix is just an OPC (zip) container with
# an `extension.vsixmanifest`, `[Content_Types].xml`, and the extension under
# `extension/`. Prints the path of the built .vsix on stdout (messages go to
# stderr), so `install.sh` can capture it.
#
# Usage: build-vsix.sh [output.vsix]   (default: a temp file)

set -eu

SRC_DIR=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-"$(mktemp -d)/culebra-debug.vsix"}

# Version drives the installed folder name (local.culebra-debug-<version>), so
# read it from the manifest rather than hardcoding.
VERSION=$(sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
  "$SRC_DIR/package.json" | head -1)
VERSION=${VERSION:-0.0.1}

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/extension/syntaxes"
cp "$SRC_DIR/package.json" "$SRC_DIR/language-configuration.json" \
  "$STAGE/extension/"
cp "$SRC_DIR/syntaxes/culebra.tmLanguage.json" "$STAGE/extension/syntaxes/"

# The debug adapter is launched by the `program` field, which defaults to plain
# "culebra" (resolved on PATH). Bake in the absolute path when available so the
# adapter starts even when VSCode is launched without your shell's PATH.
CULEBRA=$(command -v culebra 2>/dev/null || true)
if [ -n "$CULEBRA" ]; then
  sed -i.bak "s|\"program\": \"culebra\"|\"program\": \"$CULEBRA\"|" \
    "$STAGE/extension/package.json"
  rm -f "$STAGE/extension/package.json.bak"
fi

cat > "$STAGE/extension.vsixmanifest" <<XML
<?xml version="1.0" encoding="utf-8"?>
<PackageManifest Version="2.0.0" xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011">
  <Metadata>
    <Identity Language="en-US" Id="culebra-debug" Version="$VERSION" Publisher="local" />
    <DisplayName>Culebra</DisplayName>
    <Description xml:space="preserve">Syntax highlighting and debugging for Culebra (.cul) programs.</Description>
    <Categories>Programming Languages</Categories>
    <Properties>
      <Property Id="Microsoft.VisualStudio.Code.Engine" Value="^1.70.0" />
    </Properties>
  </Metadata>
  <Installation>
    <InstallationTarget Id="Microsoft.VisualStudio.Code" />
  </Installation>
  <Dependencies/>
  <Assets>
    <Asset Type="Microsoft.VisualStudio.Code.Manifest" Path="extension/package.json" Addressable="true" />
  </Assets>
</PackageManifest>
XML

cat > "$STAGE/[Content_Types].xml" <<'XML'
<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="json" ContentType="application/json" />
  <Default Extension="vsixmanifest" ContentType="text/xml" />
</Types>
XML

mkdir -p "$(dirname "$OUT")"
# Zip from the staging root so paths inside the archive are extension/... etc.
( cd "$STAGE" && zip -q -r "vsix.zip" \
    "extension.vsixmanifest" "[Content_Types].xml" "extension" )
mv "$STAGE/vsix.zip" "$OUT"

echo "built $OUT (v$VERSION)" >&2
echo "$OUT"
