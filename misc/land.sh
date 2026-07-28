#!/usr/bin/env bash
# Land a topic branch onto local master: rebase, rebuild + test-dev, then
# fast-forward merge. Meant to run as a single process under a machine-wide
# lock (`just land`, via one_at_a_time.sh) so only one session is ever in
# this sequence at a time.
#
# That lock is what closes the race this script exists for: several sessions
# each work in their own worktree+branch and land onto the same local master.
# Without serializing the *landing* step, a session could finish its
# post-rebase test-dev only to find master had moved again in the meantime
# (another session landed first) — the fast-forward merge then fails, and the
# whole rebase+test-dev has to be redone. Under the lock, master cannot move
# from a second session's landing while this one holds it, so that particular
# retry cause is gone; the loop below only fires if master moved from outside
# the protocol (a direct commit, or a session that skipped `just land`).
#
# Usage: misc/land.sh <branch>
#
# Resolves the branch's worktree and master's worktree via `git worktree
# list`, so it works regardless of which worktree it's invoked from.

set -euo pipefail

if [ $# -ne 1 ]; then
  echo "usage: land.sh <branch>" >&2
  exit 2
fi

branch="$1"

worktree_for_branch() {
  git worktree list --porcelain | awk -v want="refs/heads/$1" '
    /^worktree / { wt = substr($0, 10) }
    /^branch /   { if (substr($0, 8) == want) { print wt; exit } }
  '
}

main_repo="$(worktree_for_branch master)"
worktree="$(worktree_for_branch "$branch")"

if [ -z "$main_repo" ]; then
  echo "land: no worktree has 'master' checked out (git worktree list)" >&2
  exit 1
fi
if [ -z "$worktree" ]; then
  echo "land: no worktree checked out for branch '$branch' (git worktree list)" >&2
  exit 1
fi
if [ -n "$(git -C "$main_repo" status --porcelain)" ]; then
  echo "land: $main_repo (master) has uncommitted changes -- not touching it" >&2
  exit 1
fi

rebase_onto_master() {
  if ! git -C "$worktree" rebase master; then
    git -C "$worktree" rebase --abort
    echo "land: rebase conflict -- resolve manually (git -C \"$worktree\" rebase master), then rerun" >&2
    exit 1
  fi
}

max_attempts=5
for attempt in $(seq 1 "$max_attempts"); do
  echo "land: rebasing $branch onto master (attempt $attempt/$max_attempts)" >&2
  rebase_onto_master

  echo "land: build + test-dev" >&2
  ( cd "$worktree" && just test-dev )

  if git -C "$main_repo" merge --ff-only "$branch"; then
    echo "land: OK -- master is now $(git -C "$main_repo" rev-parse --short master)"
    exit 0
  fi
  echo "land: master advanced during test-dev -- retrying" >&2
done

echo "land: gave up after $max_attempts attempts -- master keeps moving faster than test-dev finishes" >&2
exit 1
