#!/usr/bin/env bash
# Run a command holding a machine-wide lock, so the heavy lanes (the -O3 build,
# the gate build, the full test gate) never run two at a time on one box.
#
# Several sessions work in several worktrees of this repo at once. They share
# one machine, and the heavy lanes each want every core: `just build-gate` is
# -j<ncpu> at ~11 GB peak on a 15 GB box, and the gate's own phases fan out to
# $JOBS. Two of them together do not go twice as fast — they swap and fight for
# cache, measured at 2.4-2.8x slower per run, and they make the desktop
# unusable meanwhile. Queueing is strictly better than overlapping: the same
# work finishes sooner and only one session is blocked at a time.
#
# The lock is advisory and per-machine (one file in TMPDIR, shared by every
# worktree). It is held only for the wrapped command and released when it
# exits, including on Ctrl-C — lockf/flock lock through the kernel, which drops
# the lock with the process, so a killed run cannot leave it behind.
#
# Escape hatch: CULEBRA_GATE_LOCK=0 (or "off") runs unlocked. The fast inner
# loop (`just dev` / `just test-dev`) is deliberately NOT wrapped: it has to
# stay responsive even while someone else's gate is running.

set -euo pipefail

if [ $# -eq 0 ]; then
  echo "usage: one_at_a_time.sh <command> [args...]" >&2
  exit 2
fi

case "${CULEBRA_GATE_LOCK:-1}" in
  ""|0|off) exec "$@" ;;
esac

lock="${TMPDIR:-/tmp}/culebra-heavy-lane.lock"

# BSD lockf (macOS) and util-linux flock (Linux) both take "lockfile command".
# Probing with `true` rather than with the real command keeps the two concerns
# apart: the probe only decides whether to announce a wait, so the wrapped
# command runs exactly once and its exit status is passed through untouched.
# Neither tool exists everywhere (a bare container, say); without one the
# command simply runs, which is the behaviour these recipes had before.
if command -v lockf >/dev/null 2>&1; then
  lockf -k -s -t 0 "$lock" true \
    || echo "waiting: another culebra build/gate holds this machine…" >&2
  exec lockf -k "$lock" "$@"
elif command -v flock >/dev/null 2>&1; then
  flock -n "$lock" true \
    || echo "waiting: another culebra build/gate holds this machine…" >&2
  exec flock "$lock" "$@"
else
  exec "$@"
fi
