#!/usr/bin/env bash
# Every script a release runs, CI runs on every push.
#
# Usage: tools/checks/check_release_covered_by_ci.sh
#
# Both release failures this project has had were the same failure: a command
# that only a `v*` tag reaches, broken by a batch that could not have run it.
# v0.1.0 linked Windows with an LTO setting release.yml alone had; v0.3.0 smoked
# the stripped binary with a bare launch that release.yml alone performed. In
# each case the change was made, every lane was green, and the tag was the first
# execution.
#
# This is the ratchet for that shape, in the only form that is decidable: the
# set of `misc/*.sh` invocations in release.yml must be a subset of ci.yml's.
# It says nothing about which branches of those scripts run — only that a push
# runs them at all, which is exactly what was missing both times.
#
# What it deliberately does not cover: `gh release` calls, the `prepare` job and
# the tag input are release-only by nature, and none is a `misc/` script. The
# remaining hole is a build command spelled inline in a workflow rather than in
# a script — release.yml's Windows cmake line is the live example. Moving one
# into misc/ brings it under this check; that is the argument for doing so.
set -eu

cd "$(dirname "$0")/../.."
ci=.github/workflows/ci.yml
rel=.github/workflows/release.yml

scripts_in() {  # every misc/<name>.sh or misc/<dir>/<name>.sh named anywhere in the file
  grep -oE 'misc/([A-Za-z0-9_]+/)?[A-Za-z0-9_]+\.sh' "$1" | sort -u
}

missing=$(comm -23 <(scripts_in "$rel") <(scripts_in "$ci"))

if [ -n "$missing" ]; then
  echo "release runs scripts CI does not:" >&2
  printf '  %s\n' $missing >&2
  echo >&2
  echo "A release is then the first execution of each — which is how the last" >&2
  echo "two releases broke. Call it from a ci.yml job, or explain here why a" >&2
  echo "tag is the only place it can run." >&2
  exit 1
fi

echo "release-covered-by-ci OK ($(scripts_in "$rel" | wc -l | tr -d ' ') scripts)"
