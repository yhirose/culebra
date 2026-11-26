#!/usr/bin/env bash
# The checker must fail on each way a prebuilt_llvm.env entry can be wrong, or
# it checks nothing. Each case breaks a copy of the real file one way.
set -uo pipefail

here=$(cd "$(dirname "$0")" && pwd)
check="$here/check_prebuilt_llvm_env.sh"
real="$here/../../misc/prebuilt_llvm.env"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fails=0
expect_fail() { # <name> <sed -E expression>
  sed -E "$2" "$real" > "$tmp/env"
  if PREBUILT_LLVM_ENV="$tmp/env" "$check" >/dev/null 2>&1; then
    echo "NOT CAUGHT: $1"; fails=$((fails + 1))
  else
    echo "caught: $1"
  fi
}

PREBUILT_LLVM_ENV="$real" "$check" >/dev/null || { echo "the real file fails its own check"; exit 1; }
expect_fail "a misspelt key"          's/darwin_arm64_ASSET/darwin_arm_ASSET/'
expect_fail "a missing TAG"           's/^PREBUILT_LLVM_darwin_arm64_TAG=.*//'
expect_fail "a SHA256 that is not hex" 's/(SHA256=)[0-9a-f]{3}/\1zzz/'
expect_fail "an asset that is not .tar.zst" 's/\.tar\.zst/.zip/'
expect_fail "an unknown platform"     's/darwin_arm64/macos_arm64/g'
expect_fail "a stray line"            's/^$/oops=1/'

(( fails == 0 ))
