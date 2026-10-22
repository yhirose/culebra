#!/usr/bin/env bash
# Every Linux job names its Ubuntu, and all of them the same one.
#
# A runner label like ubuntu-latest moves when GitHub says so: when it moved
# to 26.04 (2026-10), the new CMake linked the static OpenSSL with a library
# the image did not have, and every Linux job of the next push failed, the
# release workflow's included. CI and the release build have to run on the
# toolchain they were checked on, so the version is written down, once per
# job, and this check keeps those the same. Moving on means changing PIN here
# and every label it lists, in one commit.
set -euo pipefail
cd "$(dirname "$0")/../.."

PIN=ubuntu-24.04
shopt -s nullglob
files=(.github/workflows/*.yml .github/actions/*/action.yml)

# Any Ubuntu runner label, wherever a workflow writes one (runs-on, a matrix
# entry, a step's `if:`).
labels=$(grep -nHoE "ubuntu-[0-9a-z.]+" "${files[@]}" || true)
bad=$(printf '%s\n' "$labels" | grep -v ":$PIN\$" | grep . || true)
n=$(printf '%s\n' "$labels" | grep -c ":$PIN\$" || true)

if [[ -n "$bad" ]]; then
  printf 'runner-pins FAIL: %s\n' $bad >&2
  echo "  Every Linux job runs on $PIN (see the comment above ci.yml's jobs)." >&2
  exit 1
fi
# A check that finds no label checks nothing.
if (( n < 10 )); then
  echo "runner-pins FAIL: expected the workflows to name $PIN, found $n" >&2
  exit 1
fi
echo "runner-pins OK ($n labels, all $PIN)"
