#!/usr/bin/env bash
# misc/one_at_a_time.sh — the machine-wide lock around the heavy lanes. Two
# processes still queue; a command already under a lock that asks for the same
# one runs at once instead of waiting on itself forever (`one_at_a_time.sh just
# build` hung a session for an hour that way); a lock named with --lock does
# not leak into what it wraps.
#
# Usage: one_at_a_time_test.sh <path-to-one_at_a_time.sh>
set -u

LOCK_SH="${1:?usage: one_at_a_time_test.sh <one_at_a_time.sh>}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/one_at_a_time.XXXXXX")" || exit 1
trap 'rm -rf "$TMP"' EXIT
export CULEBRA_LOCK_PATH="$TMP/heavy.lock"
unset CULEBRA_GATE_LOCK CULEBRA_LOCKS_HELD

fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }

# Runs a command, giving up after $1 seconds; a hang reads as status 124.
within() {
  local limit=$(($1 * 100)) waited=0
  shift
  "$@" &
  local pid=$!
  while kill -0 "$pid" 2>/dev/null; do
    if [ "$waited" -ge "$limit" ]; then
      pkill -P "$pid" 2>/dev/null
      kill "$pid" 2>/dev/null
      wait "$pid" 2>/dev/null
      return 124
    fi
    sleep 0.01
    waited=$((waited + 1))
  done
  wait "$pid"
}

# Whether some other process holds the lock file right now.
held() {
  if command -v lockf >/dev/null 2>&1; then
    ! lockf -k -s -t 0 "$1" true
  else
    ! flock -n "$1" true
  fi
}
export -f held

# The same lock, asked for again inside, runs the command and passes its status.
within 10 "$LOCK_SH" "$LOCK_SH" sh -c 'exit 7'
[ $? -eq 7 ] || fail "nested same lock: want the command's status 7"

# Another spelling of the same file is the same lock.
ln -s "$TMP" "$TMP/link"
within 10 "$LOCK_SH" env CULEBRA_LOCK_PATH="$TMP/link/./heavy.lock" "$LOCK_SH" true \
  || fail "nested same lock through a symlinked path"

# --lock names its own lock; the command it wraps still queues on the heavy one.
within 10 "$LOCK_SH" --lock "$TMP/land.lock" \
  "$LOCK_SH" bash -c 'held "$1" && held "$2"' _ "$TMP/land.lock" "$TMP/heavy.lock" \
  || fail "--lock inside: want both the land lock and the heavy-lane lock held"

# Two processes still queue: the second says it waits, and runs only once the
# first has finished.
"$LOCK_SH" sh -c 'touch "$1/first"; while [ ! -e "$1/release" ]; do sleep 0.01; done; touch "$1/first-done"' _ "$TMP" &
first=$!
within 10 sh -c 'while [ ! -e "$1/first" ]; do sleep 0.01; done' _ "$TMP" \
  || fail "the first process never took the lock"
"$LOCK_SH" --wait-msg queued sh -c '[ -e "$1/first-done" ] && touch "$1/second"' _ "$TMP" 2>"$TMP/second.err" &
second=$!
within 10 sh -c 'until grep -q queued "$1"; do sleep 0.01; done' _ "$TMP/second.err" \
  || fail "the second process did not say it waits"
touch "$TMP/release"
wait "$first" "$second"
[ -e "$TMP/second" ] || fail "the second process ran before the first let go, or never ran"

# A lock in a folder that is not there fails at once, naming the folder; it
# does not turn into a lock at /, waiting.
"$LOCK_SH" --lock "$TMP/missing/dir/x.lock" true 2>"$TMP/missing.err"
[ $? -ne 0 ] || fail "a lock in a missing folder ran its command"
grep -q waiting "$TMP/missing.err" && fail "a lock in a missing folder said it waits"
grep -q "missing/dir" "$TMP/missing.err" || fail "the error did not name the missing folder"

[ "$fails" -eq 0 ] || exit 1
echo "one_at_a_time_test: ok"
