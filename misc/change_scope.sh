#!/usr/bin/env bash
# Print how far a change reaches — `none`, `light` or `full` (see
# tools/checks/change_scopes.txt) — given the paths it touches, one per line
# on stdin. `just land` uses it to run only the checks a change can affect;
# CI uses it to skip the lanes a change cannot reach.
#
#   git diff --name-only master...HEAD | misc/change_scope.sh
set -euo pipefail

table="$(dirname "$0")/../tools/checks/change_scopes.txt"
rank() { case "$1" in none) echo 0 ;; light) echo 1 ;; *) echo 2 ;; esac; }

widest=-1
while IFS= read -r path; do
  [ -n "$path" ] || continue
  scope=full
  while read -r s glob; do
    case "$s" in ""|"#"*) continue ;; esac
    # shellcheck disable=SC2053  # the glob is the point
    if [[ $path == $glob ]]; then scope=$s; break; fi
  done < "$table"
  r=$(rank "$scope")
  (( r > widest )) && widest=$r
done

case "$widest" in
  0) echo none ;;
  1) echo light ;;
  *) echo full ;;  # also an empty change: nothing to go on, so check it all
esac
