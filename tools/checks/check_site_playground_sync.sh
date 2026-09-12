#!/usr/bin/env bash
# site/playground/ is what GitHub Pages serves and playground/ is its source;
# playground/build.sh carries one to the other, but needs emsdk, so it never
# runs in CI and the copies are committed. Nothing else compared the two
# trees, and they drifted (a9bf67a6 caught up on a highlighter and five
# examples that had been behind for weeks). This stages build.sh's own copy
# step (copy-frontend.sh, which needs no toolchain) into a scratch directory
# and diffs every file it produced against the committed one. The wasm is
# not staged and so not here — a stale engine is a rebuild decision, not a
# copy that was forgotten.
set -euo pipefail
cd "$(dirname "$0")/../.."

staged="$(mktemp -d "${TMPDIR:-/tmp}/pg-sync.XXXXXX")"
trap 'rm -rf "$staged"' EXIT
playground/copy-frontend.sh "$staged" >/dev/null

fail=0
while IFS= read -r -d '' f; do
  rel="${f#"$staged"/}"
  if ! diff -q "$f" "site/playground/$rel" >/dev/null 2>&1; then
    echo "site-playground-sync FAIL: site/playground/$rel is not what build.sh would write" >&2
    fail=1
  fi
done < <(find "$staged" -type f -print0)

if [ "$fail" != 0 ]; then
  echo "  run \`just site-build\` and commit site/playground/" >&2
  exit 1
fi
echo "site/playground matches playground/ and examples.json"
