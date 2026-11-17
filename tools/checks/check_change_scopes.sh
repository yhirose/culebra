#!/usr/bin/env bash
# The push `paths-ignore` of ci.yml and asan.yml must be exactly the `none`
# globs of tools/checks/change_scopes.txt (written as GitHub globs: a trailing
# `/*` becomes `/**`). All three decide that nothing reads those paths; this
# keeps the one list the source of them.
set -euo pipefail
cd "$(dirname "$0")/../.."

want=$(awk '$1 == "none" { g = $2; sub(/\/\*$/, "/**", g); print g }' \
  tools/checks/change_scopes.txt | sort)
for wf in ci.yml asan.yml; do
  have=$(sed -n "s/^[[:space:]]*paths-ignore:[[:space:]]*\[\(.*\)\][[:space:]]*$/\1/p" \
    ".github/workflows/$wf" | tr ',' '\n' | tr -d " '\"" | sed '/^$/d' | sort)
  if [ "$want" != "$have" ]; then
    echo "change-scopes FAIL: $wf push paths-ignore differs from the none scope in change_scopes.txt" >&2
    printf 'change_scopes.txt (none):\n%s\n%s paths-ignore:\n%s\n' "$want" "$wf" "$have" >&2
    exit 1
  fi
done
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
expect light site/index.html examples/a.cul examples/languages/pl0/pl0.cul
expect full  kauai/cli/main.cul
expect full  examples/wrap/demo.cul
expect full  examples/tensor/mnist/assets/Inter-SemiBold-subset.ttf
expect full  docs/language.md include/rt/value.inc.h
expect full  CLAUDE.md.bak
expect full  docs
expect full  ''
[ "$fail" -eq 0 ] || exit 1
echo "change scopes in sync"
