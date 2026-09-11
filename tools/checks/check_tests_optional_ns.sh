#!/usr/bin/env bash
# Optional-namespace ratchet for the tests/*.cul sweep. Every file there runs
# on every lane, so it may only name namespaces every lane's binary carries.
# Scene, Webview and Desktop are not among them: a CMake option takes each
# away and the name stops resolving, so the file dies with a NameError before
# its first assert. (Http and SQLite have options of their own but are not in
# this table — canon_sigs_table.h registers them either way, and only their
# methods go stubbed.) e7ce5d88 made this repair for Scene and d787a669 for
# Desktop, each after a green local run, where the axis is built.
set -euo pipefail
cd "$(dirname "$0")/../.."
shopt -s nullglob

# namespace:the CMake option whose OFF takes it away. A row whose option is
# gone guards a name nothing can remove any more — stale table, not clean
# tree — so the option is what this checks before trusting the row.
OPTIONAL='Scene:CULEBRA_ENABLE_SCENE
Webview:CULEBRA_ENABLE_WEBVIEW
Desktop:CULEBRA_ENABLE_WEBVIEW'

alt=''
while IFS=: read -r ns opt; do
  [[ -n "$ns" ]] || continue
  if ! grep -q "option($opt " CMakeLists.txt; then
    echo "optional-ns FAIL: CMakeLists.txt no longer declares option($opt) —" \
         "update the table in $0" >&2
    exit 1
  fi
  alt="${alt:+$alt|}$ns"
done <<<"$OPTIONAL"

# The sweep is a couple of hundred files; a glob that comes back nearly empty
# means it moved, and a ratchet reading nothing passes while measuring nothing.
files=(tests/*.cul)
if (( ${#files[@]} < 100 )); then
  echo "optional-ns FAIL: only ${#files[@]} tests/*.cul — did the sweep move?" >&2
  exit 1
fi

# Comments go first: test_namespace_attr.cul names these in prose, and prose is
# not a reference the loader has to resolve.
hits=$(awk -v re="(^|[^A-Za-z0-9_])($alt)([^A-Za-z0-9_]|\$)" '
  {
    line = $0
    sub(/\/\/.*/, "", line)
    sub(/#.*/, "", line)
    if (line ~ re) printf "%s:%d: %s\n", FILENAME, FNR, $0
  }' "${files[@]}")

if [[ -n "$hits" ]]; then
  echo "optional-ns FAIL: a tests/*.cul file names a namespace some builds" \
       "do not carry:" >&2
  echo "$hits" >&2
  cat >&2 <<'EOF'
  The sweep runs on lanes built without these, where the name does not
  resolve and the file dies with a NameError before its first assert.
  Use a namespace every build carries (e7ce5d88 swapped Scene.Image for
  __Foreign.Box and Scene.Texture for CodeGen.Program), or put the test
  where only a build that has the axis runs it (tests/gui/, scene_api_test.sh).
EOF
  exit 1
fi

echo "optional-ns OK (${#files[@]} file(s), none name ${alt//|/ / })"
