#!/usr/bin/env bash
# Vendor submodule update checker/bumper.
#
# Default (no args): dry-run — fetch each vendor/* submodule's remote and
# report which ones are behind (current SHA, latest SHA, commit count, short
# log). Nothing is modified.
#
# --run: for every outdated submodule, fetch + checkout the tracking target
# (a branch tip, same as `git submodule update --remote`, or a release tag —
# see TAG_TRACKED below), leaving the gitlink modified in the superproject
# working tree. It does NOT commit — review the submodule's log, rebuild/test,
# then commit with a `Bump <name>: ...` message summarizing what changed (see
# recent history for the convention).
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

# Submodules that cut real releases: track the newest release tag instead of
# the default branch's HEAD, so a vendor bump never pulls in unreleased work.
# Everything else (our own libs with no release process, plus SDL which
# already tracks a stable maintenance branch via .gitmodules' `branch =`)
# keeps tracking a branch.
declare -A TAG_TRACKED=(
  [vendor/cpp-peglib]=1
  [vendor/cpp-httplib]=1
  [vendor/raylib]=1
)

# latest_tag <path> — the newest tag by `sort -V`, skipping anything that
# looks like a prerelease (a `-` in the name, e.g. raylib's `4.6-dev`).
# Assumes plain-numeric release tags (`v1.2.3`, `5.5`); a repo with a
# different tag convention needs its own filter.
latest_tag() {
  local path="$1"
  git -c credential.helper= -C "$path" ls-remote --tags --refs origin 2>/dev/null \
    | awk '{print $2}' | sed 's#refs/tags/##' | grep -v -- '-' | sort -V | tail -1
}

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

# resolve_target <path> — echoes "<kind> <ref>" (kind is "tag" or "branch").
# Prints nothing if the target can't be resolved (network hiccup, no tags).
resolve_target() {
  local path="$1"
  if [[ -n "${TAG_TRACKED[$path]:-}" ]]; then
    local tag
    tag=$(latest_tag "$path")
    [[ -n "$tag" ]] && echo "tag $tag"
    return
  fi
  local branch
  branch=$(remote_branch "$path")
  [[ -n "$branch" ]] && echo "branch $branch"
}

paths=$(git config --file .gitmodules --get-regexp '\.path$' | awk '{print $2}')

outdated=()
printf '%-20s %-10s %-10s %6s  %s\n' "SUBMODULE" "CURRENT" "LATEST" "BEHIND" "TRACKING"
for path in $paths; do
  name=${path#vendor/}
  current=$(git -C "$path" rev-parse HEAD)
  target_line=$(resolve_target "$path")
  if [[ -z "$target_line" ]]; then
    printf '%-20s %-10s %-10s %6s  (could not resolve tracking target)\n' "$name" "${current:0:10}" "?" "?"
    continue
  fi
  read -r kind ref <<< "$target_line"
  label="$kind:$ref"
  if [[ "$kind" == "tag" ]]; then
    git -c credential.helper= -C "$path" fetch --quiet origin "refs/tags/${ref}:refs/tags/${ref}"
  else
    git -c credential.helper= -C "$path" fetch --quiet origin "$ref"
    ref="origin/$ref"
  fi
  latest=$(git -C "$path" rev-parse "$ref")
  behind=$(git -C "$path" rev-list --count "HEAD..$ref")
  printf '%-20s %-10s %-10s %6s  %s\n' "$name" "${current:0:10}" "${latest:0:10}" "$behind" "$label"
  if [[ "$behind" != "0" ]]; then
    outdated+=("$path|$label|$latest")
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
  IFS='|' read -r path label latest <<< "$entry"
  echo
  echo "--- $path ($label) ---"
  total=$(git -C "$path" rev-list --count "HEAD..$latest")
  git --no-pager -C "$path" log --oneline -n "$log_limit" "HEAD..$latest"
  if (( total > log_limit )); then
    echo "... and $((total - log_limit)) more (see: git -C $path log --oneline HEAD..$latest)"
  fi
done

if (( run == 0 )); then
  echo
  echo "dry-run only. Re-run with --run to update the working tree (no commit)."
  exit 0
fi

echo
for entry in "${outdated[@]}"; do
  IFS='|' read -r path label latest <<< "$entry"
  echo "updating $path -> ${latest:0:10} ($label)"
  git -C "$path" checkout --quiet "$latest"
done

# The blob is keyed to the cpp-peglib version, and a vendor bump's diff never
# mentions grammar_blob.gen.h. Regenerating unconditionally is idempotent. Never
# fatal: the checkouts above are already applied, and a peglib bump that breaks
# the generator's own compile is exactly when the closing advice is needed.
blob_before=$(git hash-object include/grammar_blob.gen.h)
echo
echo "regenerating include/grammar_blob.gen.h (keyed to the cpp-peglib version)"
just gen-blob || echo "WARNING: gen-blob failed — run it before committing" >&2

echo
echo "done. Review each submodule's log, rebuild (\`just dev\`), run \`just test-dev\`,"
echo "then commit each bump separately (\`Bump <name>: ...\`, see git log for style)."
if [[ "$(git hash-object include/grammar_blob.gen.h)" != "$blob_before" ]]; then
  echo "NOTE: grammar_blob.gen.h changed — commit it with the bump that invalidated it."
fi
