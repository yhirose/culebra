#!/usr/bin/env bash
# Run a command holding a machine-wide lock, so the heavy lanes (the -O3 build,
# the gate build, the full test gate) never run two at a time on one box.
#
# Several sessions work in several worktrees of this repo at once. They share
# one machine, and the heavy lanes each want most of it: a full-tree build
# peaks at ~11 GB on a 16 GB box (misc/jobs_within.sh), and the gate's own
# phases fan out to $JOBS. Two of them together do not go twice as fast —
# they swap and fight for cache, measured at 2.4-2.8x slower per run, and they
# make the desktop unusable meanwhile. Queueing is strictly better than
# overlapping: the same work finishes sooner and only one session is blocked
# at a time.
#
# The lock is advisory. By default it's one file in TMPDIR, shared by every
# worktree; CULEBRA_LOCK_PATH overrides the path for every heavy lane. A lock
# of another kind is named with --lock (`just land` points it under the shared
# .git dir, so it's the same file regardless of each session's own TMPDIR);
# an option rather than the variable, because the command it wraps inherits
# the environment and may take the heavy-lane lock itself. It is held only for
# the wrapped command and released when it exits, including on Ctrl-C —
# lockf/flock lock through the kernel, which drops the lock with the process,
# so a killed run cannot leave it behind.
#
# Reentrant: a command already under a lock may run something that asks for
# the same one (`one_at_a_time.sh just build`, whose recipe wraps make again).
# The lock belongs to the outer process, so waiting for it would wait forever;
# CULEBRA_LOCKS_HELD lists the locks held up the process tree, and a lock on
# that list runs the command straight away.
#
# Whoever takes a lock notes itself beside it ($lock.who: its pid, folder,
# command or CULEBRA_LOCK_LABEL, and since when), and a command that has to wait
# says who that is: the wait may be for another checkout, or another project
# that takes this lock around its own heavy jobs.
#
# Escape hatch: CULEBRA_GATE_LOCK=0 (or "off") runs unlocked. The fast inner
# loop (`just dev` / `just test-dev`) is deliberately NOT wrapped: it has to
# stay responsive even while someone else's gate — or a `just land` — is
# running.
#
# Usage: one_at_a_time.sh [--lock <path>] [--wait-msg <text>] <command> [args...]

set -euo pipefail

lock="${CULEBRA_LOCK_PATH:-${TMPDIR:-/tmp}/culebra-heavy-lane.lock}"
wait_msg="waiting: another culebra build/gate holds this machine…"
while [ $# -gt 0 ]; do
  case "$1" in
    --lock) lock="${2:?--lock needs a path}"; shift 2 ;;
    --wait-msg) wait_msg="${2:?--wait-msg needs a text}"; shift 2 ;;
    *) break ;;
  esac
done

if [ $# -eq 0 ]; then
  echo "usage: one_at_a_time.sh [--lock <path>] [--wait-msg <text>] <command> [args...]" >&2
  exit 2
fi

case "${CULEBRA_GATE_LOCK:-1}" in
  ""|0|off) exec "$@" ;;
esac

# One spelling per file, so CULEBRA_LOCKS_HELD sees /tmp and /private/tmp
# (macOS) as one lock.
lockdir="$(cd "$(dirname "$lock")" && pwd -P)" || exit 1
lock="$lockdir/$(basename "$lock")"
case ":${CULEBRA_LOCKS_HELD:-}:" in
  *":$lock:"*) exec "$@" ;;
esac
export CULEBRA_LOCKS_HELD="${CULEBRA_LOCKS_HELD:+$CULEBRA_LOCKS_HELD:}$lock"

# BSD lockf (macOS) and util-linux flock (Linux) both take "lockfile command".
# Probing with `true` rather than with the real command keeps the two concerns
# apart: the probe only decides whether to announce a wait, so the wrapped
# command runs exactly once and its exit status is passed through untouched.
# Neither tool exists everywhere (a bare container, say); without one the
# command simply runs, which is the behaviour these recipes had before.
# The note is only believed while its writer is alive: a holder that did not
# write one (an older copy of this script) leaves the last one behind.
waiting() {
  local pid label
  if IFS=' ' read -r pid label 2>/dev/null <"$lock.who" && kill -0 "$pid" 2>/dev/null; then
    echo "$wait_msg held by $label" >&2
  else
    echo "$wait_msg" >&2
  fi
}
label="${CULEBRA_LOCK_LABEL:-$PWD: $*}"
unset CULEBRA_LOCK_LABEL
label="$(printf '%s' "$label" | tr '\n' ' ' | cut -c1-200)"
# Runs under the lock, notes who it is and gives its place to the command.
note='printf "%s %s (since %s)\n" "$$" "$1" "$(date +%H:%M:%S)" >"$2"; shift 2; exec "$@"'

if command -v lockf >/dev/null 2>&1; then
  lockf -k -s -t 0 "$lock" true || waiting
  exec lockf -k "$lock" sh -c "$note" note "$label" "$lock.who" "$@"
elif command -v flock >/dev/null 2>&1; then
  flock -n "$lock" true || waiting
  exec flock "$lock" sh -c "$note" note "$label" "$lock.who" "$@"
else
  exec "$@"
fi
