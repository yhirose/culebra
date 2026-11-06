#!/usr/bin/env bash
# Every Linux job names its Ubuntu, and all of them the same one. The macOS
# jobs that build a download name the oldest macOS it supports (below).
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

# The macOS jobs that build a download, release.yml's and ci.yml's mirrors of
# them, run on the oldest macOS it supports, since a build starts on the macOS
# it was built on and later. The others stay on macos-latest, unpinned.
MAC_PIN=macos-$(sed -n 's/^oldest_macos=\([0-9]*\).*/\1/p' misc/verify_standalone_macho.sh)
mac_labels=$(grep -nHoE "macos-[0-9]+" "${files[@]}" || true)
mac_bad=$(printf '%s\n' "$mac_labels" | grep -v ":$MAC_PIN\$" | grep . || true)
mac_n=$(printf '%s\n' "$mac_labels" | grep -c ":$MAC_PIN\$" || true)
if [[ -n "$mac_bad" ]]; then
  printf 'runner-pins FAIL: %s\n' $mac_bad >&2
  echo "  A pinned macOS job runs on $MAC_PIN (oldest_macos in misc/verify_standalone_macho.sh)." >&2
  exit 1
fi
if (( mac_n < 4 )); then
  echo "runner-pins FAIL: expected the workflows to name $MAC_PIN, found $mac_n" >&2
  exit 1
fi
echo "runner-pins OK ($n labels, all $PIN; $mac_n labels, all $MAC_PIN)"
