#!/usr/bin/env bash
# Vendor update checker/bumper, for everything this repository carries a copy
# of: the vendor/* submodules, and the Playground's bundled front end under
# playground/vendor (CodeMirror and xterm, which are vendored as bundles
# rather than as submodules — see playground/vendor.sh). One report, because
# two places to ask "what have we pinned that has moved on" is one place too
# many to remember.
#
# Default (no args): dry-run — fetch each vendor/* submodule's remote and
# report which ones are behind (current SHA, latest SHA, commit count, short
# log), then ask npm the same of the bundled packages. Nothing is modified.
#
# --run: for every outdated submodule, fetch + checkout the tracking target
# (a branch tip, same as `git submodule update --remote`, or a release tag —
# see TAG_TRACKED below), leaving the gitlink modified in the superproject
# working tree; and for the bundled packages, move each pin to the latest
# release and rebuild the bundles. It does NOT commit — review the submodule's
# log, rebuild/test, then commit with a `Bump <name>: ...` message summarizing
# what changed (see recent history for the convention).
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

# --- the Playground's bundled front end ------------------------------------
#
# npm resolves these, so "behind" is semver rather than a commit count, and a
# major bump is worth saying out loud: it is the one where the closing advice
# to rebuild and test stops being a formality. `npm outdated` exits 1 when it
# finds something, which is not a failure here.
PG_VENDOR=playground/vendor
pg_outdated_json() {
  local work
  work=$(mktemp -d "${TMPDIR:-/tmp}/pg-outdated.XXXXXX")
  cp "$PG_VENDOR/package.json" "$work/"
  [[ -f "$PG_VENDOR/package-lock.json" ]] && cp "$PG_VENDOR/package-lock.json" "$work/"
  (cd "$work" && npm outdated --json 2>/dev/null) || true
  rm -rf "$work"
}

# Fills PG_JSON once (npm's own view of the pins) and prints a line per
# package with a newer release. Returns 1 when there is nothing to do, so the
# caller can tell "nothing to update" from "here is what to update".
PG_JSON="{}"
pg_has_major=0
pg_report() {
  if ! command -v npm >/dev/null; then
    echo
    echo "playground/vendor: npm not on PATH, skipping (the submodules above are unaffected)."
    return 1
  fi
  PG_JSON=$(pg_outdated_json)
  export PG_JSON
  local out rc=0
  out=$(node -e '
    const o = JSON.parse(process.env.PG_JSON || "{}");
    const rows = [];
    for (const [name, v] of Object.entries(o)) {
      const pinned = v.wanted, latest = v.latest;
      if (!latest || latest === pinned) continue;
      const major = String(pinned).split(".")[0] !== String(latest).split(".")[0];
      rows.push(name.padEnd(20) + " " + String(pinned).padEnd(10) +
                String(latest).padEnd(10) + (major ? " MAJOR" : ""));
    }
    if (!rows.length) process.exit(3);
    console.log(rows.join("\n"));
  ') || rc=$?
  if (( rc == 3 )); then
    echo
    echo "playground/vendor: every pin is the latest release."
    return 1
  fi
  if (( rc != 0 )); then
    echo
    echo "playground/vendor: could not read the registry, skipping." >&2
    return 1
  fi
  echo
  printf '%-20s %-10s %s\n' "PACKAGE" "PINNED" "LATEST"
  echo "$out"
  if grep -q MAJOR <<< "$out"; then
    pg_has_major=1
  fi
  return 0
}

# Moves every pin to the latest release and rebuilds. The lockfile goes first
# so vendor.sh takes its install path and writes a fresh one; copy-frontend
# then carries the new bundles to site/, which is what the sync gate compares.
pg_apply() {
  echo
  echo "updating playground/vendor pins to the latest releases"
  node -e '
    const fs = require("fs"), file = process.argv[1];
    const pkg = JSON.parse(fs.readFileSync(file, "utf8"));
    const latest = JSON.parse(process.env.PG_JSON || "{}");
    for (const field of ["dependencies", "devDependencies"])
      for (const name of Object.keys(pkg[field] || {}))
        if (latest[name] && latest[name].latest) pkg[field][name] = latest[name].latest;
    fs.writeFileSync(file, JSON.stringify(pkg, null, 2) + "\n");
  ' "$PG_VENDOR/package.json"
  rm -f "$PG_VENDOR/package-lock.json"
  ./playground/vendor.sh
  ./playground/copy-frontend.sh site/playground >/dev/null
  echo "playground/vendor rebuilt; site/playground/vendor updated to match."
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
else
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
fi

pg_outdated=0
if pg_report; then
  pg_outdated=1
fi

if (( ${#outdated[@]} == 0 && pg_outdated == 0 )); then
  exit 0
fi

if (( run == 0 )); then
  echo
  echo "dry-run only. Re-run with --run to update the working tree (no commit)."
  if (( pg_has_major )); then
    echo "note: a MAJOR bump is listed; read its changelog before taking it."
  fi
  exit 0
fi

blob_before=$(git hash-object include/frontend/grammar_blob.gen.h)
if (( ${#outdated[@]} > 0 )); then
  echo
  for entry in "${outdated[@]}"; do
    IFS='|' read -r path label latest <<< "$entry"
    echo "updating $path -> ${latest:0:10} ($label)"
    git -C "$path" checkout --quiet "$latest"
  done

  # The blob is keyed to the cpp-peglib version, and a vendor bump's diff never
  # mentions grammar_blob.gen.h. Regenerating unconditionally is idempotent.
  # Never fatal: the checkouts above are already applied, and a peglib bump that
  # breaks the generator's own compile is exactly when the closing advice is
  # needed. Inside this branch because it belongs to the submodules: a run that
  # only moves an npm pin has not touched peglib at all.
  echo
  echo "regenerating include/frontend/grammar_blob.gen.h (keyed to the cpp-peglib version)"
  just gen-blob || echo "WARNING: gen-blob failed — run it before committing" >&2
fi

if (( pg_outdated )); then
  pg_apply
fi

echo
echo "done. Review each submodule's log, rebuild (\`just dev\`), run \`just test-dev\`,"
echo "then commit each bump separately (\`Bump <name>: ...\`, see git log for style)."
if (( pg_outdated )); then
  echo "playground/vendor: open the Playground and exercise the editor and a TUI example before committing."
fi
if [[ "$(git hash-object include/frontend/grammar_blob.gen.h)" != "$blob_before" ]]; then
  echo "NOTE: grammar_blob.gen.h changed — commit it with the bump that invalidated it."
fi
