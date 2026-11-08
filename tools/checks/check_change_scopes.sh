#!/usr/bin/env bash
# ci.yml's push `paths-ignore` must be exactly the `none` globs of
# tools/checks/change_scopes.txt (written as GitHub globs: a trailing `/*`
# becomes `/**`). Both decide that nothing reads those paths; this keeps the
# one list the source of both.
set -euo pipefail
cd "$(dirname "$0")/../.."

want=$(awk '$1 == "none" { g = $2; sub(/\/\*$/, "/**", g); print g }' \
  tools/checks/change_scopes.txt | sort)
have=$(sed -n "s/^[[:space:]]*paths-ignore:[[:space:]]*\[\(.*\)\][[:space:]]*$/\1/p" \
  .github/workflows/ci.yml | tr ',' '\n' | tr -d " '\"" | sed '/^$/d' | sort)

if [ "$want" != "$have" ]; then
  echo "change-scopes FAIL: ci.yml push paths-ignore differs from the none scope in change_scopes.txt" >&2
  printf 'change_scopes.txt (none):\n%s\nci.yml paths-ignore:\n%s\n' "$want" "$have" >&2
  exit 1
fi
# The classifier decides which checks a change skips, so pin what it says.
# Each case: the paths of one change, then the scope it must get.
fail=0
expect() {
  local want=$1; shift
  local got
  got=$(printf '%s\n' "$@" | misc/change_scope.sh)
  if [ "$got" != "$want" ]; then
    echo "change-scopes FAIL: {$*} gave $got, want $want" >&2
    fail=1
  fi
}
expect none  .claude/skills/x/SKILL.md CLAUDE.md
expect light docs/language.md .claude/x
expect light site/index.html examples/a.cul kauai/songs/b.kau
expect full  docs/language.md include/rt/value.inc.h
expect full  CLAUDE.md.bak
expect full  docs
expect full  ''
[ "$fail" -eq 0 ] || exit 1
echo "change scopes in sync"
