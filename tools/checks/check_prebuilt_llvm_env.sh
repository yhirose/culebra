#!/usr/bin/env bash
# misc/prebuilt_llvm.env is read by key (`<uname -s>_<uname -m>`, lower case),
# and a key misspelt there is not an error to misc/prebuilt_llvm.sh: the
# platform just has no prebuilt, and the build quietly links the LLVM cmake
# finds. This is where such a slip is caught: every key is a known platform and
# has all three of its lines.
set -euo pipefail

here=$(cd "$(dirname "$0")/../.." && pwd)
# PREBUILT_LLVM_ENV names another file: the checker is tested on broken copies.
env_file="${PREBUILT_LLVM_ENV:-$here/misc/prebuilt_llvm.env}"

# The platforms uname can report that culebra builds on.
known=" darwin_arm64 linux_x86_64 linux_aarch64 windows_x86_64 "

fail=0
keys=$(sed -nE 's/^PREBUILT_LLVM_(.*)_(TAG|ASSET|SHA256)=.*/\1/p' "$env_file" | sort -u)
[[ -n "$keys" ]] || { echo "prebuilt-llvm-env: no entries in $env_file" >&2; exit 1; }

# A line that is neither a comment, blank, nor a PREBUILT_LLVM_<key>_<field>.
stray=$(grep -vE '^(#|$|PREBUILT_LLVM_[a-z0-9_]+_(TAG|ASSET|SHA256)=)' "$env_file" || true)
if [[ -n "$stray" ]]; then
  echo "prebuilt-llvm-env: unrecognised line(s):" >&2
  echo "$stray" >&2
  fail=1
fi

for key in $keys; do
  if [[ "$known" != *" $key "* ]]; then
    echo "prebuilt-llvm-env: '$key' is not a platform uname reports here (known:$known)" >&2
    fail=1
    continue
  fi
  for field in TAG ASSET SHA256; do
    grep -qE "^PREBUILT_LLVM_${key}_${field}=.+" "$env_file" || {
      echo "prebuilt-llvm-env: $key has no $field" >&2
      fail=1
    }
  done
  sha=$(sed -n "s/^PREBUILT_LLVM_${key}_SHA256=//p" "$env_file")
  [[ "$sha" =~ ^[0-9a-f]{64}$ ]] || { echo "prebuilt-llvm-env: $key's SHA256 is not 64 hex digits" >&2; fail=1; }
  asset=$(sed -n "s/^PREBUILT_LLVM_${key}_ASSET=//p" "$env_file")
  [[ "$asset" == *.tar.zst ]] || { echo "prebuilt-llvm-env: $key's asset is not a .tar.zst" >&2; fail=1; }
done

(( fail == 0 )) || exit 1
echo "prebuilt-llvm-env OK ($(echo $keys | wc -w | tr -d ' ') platform(s): $(echo $keys))"
