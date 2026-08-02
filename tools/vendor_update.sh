#!/usr/bin/env bash
# Vendor submodule update checker/bumper.
#
# Default (no args): dry-run — fetch each vendor/* submodule's remote and
# report which ones are behind (current SHA, latest SHA, commit count, short
# log). Nothing is modified.
#
# --run: for every outdated submodule, fetch + checkout the remote HEAD
# (detached, same as `git submodule update --remote`), leaving the gitlink
# modified in the superproject working tree. It does NOT commit — review the
# submodule's log, rebuild/test, then commit with a `Bump <name>: ...`
# message summarizing what changed (see recent history for the convention).
set -euo pipefail
cd "$(dirname "$0")/.."
export GIT_PAGER=cat PAGER=cat

run=0
for arg in "$@"; do
  case "$arg" in
    --run) run=1 ;;
    *) echo "usage: $0 [--run]" >&2; exit 1 ;;
  esac
done

# remote_branch <path> — the tracking branch: .gitmodules' `branch =` if set,
# else the remote's default branch (via `ls-remote --symref HEAD`).
remote_branch() {
  local path="$1"
  local configured
  configured=$(git config --file .gitmodules --get "submodule.${path}.branch" 2>/dev/null || true)
  if [[ -n "$configured" ]]; then
    echo "$configured"
    return
  fi
  git -c credential.helper= -C "$path" ls-remote --symref origin HEAD 2>/dev/null \
    | awk '/^ref:/ { sub(".*/", "", $2); print $2; exit }'
}

paths=$(git config --file .gitmodules --get-regexp '\.path$' | awk '{print $2}')

outdated=()
printf '%-20s %-10s %-10s %6s  %s\n' "SUBMODULE" "CURRENT" "LATEST" "BEHIND" "BRANCH"
for path in $paths; do
  name=${path#vendor/}
  current=$(git -C "$path" rev-parse HEAD)
  branch=$(remote_branch "$path")
  if [[ -z "$branch" ]]; then
    printf '%-20s %-10s %-10s %6s  (could not resolve default branch)\n' "$name" "${current:0:10}" "?" "?"
    continue
  fi
  git -c credential.helper= -C "$path" fetch --quiet origin "$branch"
  latest=$(git -C "$path" rev-parse "origin/$branch")
  behind=$(git -C "$path" rev-list --count "HEAD..origin/$branch")
  printf '%-20s %-10s %-10s %6s  %s\n' "$name" "${current:0:10}" "${latest:0:10}" "$behind" "$branch"
  if [[ "$behind" != "0" ]]; then
    outdated+=("$path|$branch|$latest")
  fi
done

if (( ${#outdated[@]} == 0 )); then
  echo
  echo "all vendor submodules up to date."
  exit 0
fi

echo
echo "outdated: ${#outdated[@]}"
log_limit=20
for entry in "${outdated[@]}"; do
  IFS='|' read -r path branch latest <<< "$entry"
  echo
  echo "--- $path ($branch) ---"
  total=$(git -C "$path" rev-list --count "HEAD..origin/$branch")
  git --no-pager -C "$path" log --oneline -n "$log_limit" "HEAD..origin/$branch"
  if (( total > log_limit )); then
    echo "... and $((total - log_limit)) more (see: git -C $path log --oneline HEAD..origin/$branch)"
  fi
done

if (( run == 0 )); then
  echo
  echo "dry-run only. Re-run with --run to update the working tree (no commit)."
  exit 0
fi

echo
for entry in "${outdated[@]}"; do
  IFS='|' read -r path branch latest <<< "$entry"
  echo "updating $path -> ${latest:0:10}"
  git -C "$path" checkout --quiet "$latest"
done

# The blob is keyed to the cpp-peglib version, and a vendor bump's diff never
# mentions grammar_blob.h. Regenerating unconditionally is idempotent. Never
# fatal: the checkouts above are already applied, and a peglib bump that breaks
# the generator's own compile is exactly when the closing advice is needed.
blob_before=$(git hash-object include/grammar_blob.h)
echo
echo "regenerating include/grammar_blob.h (keyed to the cpp-peglib version)"
just gen-blob || echo "WARNING: gen-blob failed — run it before committing" >&2

echo
echo "done. Review each submodule's log, rebuild (\`just dev\`), run \`just test-dev\`,"
echo "then commit each bump separately (\`Bump <name>: ...\`, see git log for style)."
if [[ "$(git hash-object include/grammar_blob.h)" != "$blob_before" ]]; then
  echo "NOTE: grammar_blob.h changed — commit it with the bump that invalidated it."
fi
