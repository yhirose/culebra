#!/usr/bin/env bash
# Pipefail-reader gate.
#
# Under `set -o pipefail` a pipeline answers with its last failing command, so
# a reader that leaves before its input ends hands the pipeline its writer's
# SIGPIPE. `grep -q` leaves at its first match, `head` at its last line,
# `awk ... exit` where it says so; a writer still writing then (its output
# takes more than one write, and a busy machine widens the gaps between them)
# dies with 141, and the pipeline fails or not by timing.
# `echo "$list" | grep -qx name` said "not there" for a name that was, and
# failed the landing gate on it.
#
# So where pipefail can hold (a script that sets it, and the justfile, the
# workflows and the composite actions, read whole, since a recipe's or a
# step's shell decides it) no pipe feeds such a reader. Test captured text from a here-string
# (`grep -qx -- "$1" <<<"$list"`) or let grep read a stream to its end
# (`cmd | grep -x 'ok' >/dev/null`), trim with `grep -m N` on a file or
# `sed -n '1,Np'`, and have awk remember its answer rather than exit.
set -euo pipefail
cd "$(dirname "$0")/../.."

mapfile -t files < <(
  git grep --no-recurse-submodules -l pipefail -- '*.sh' justfile
  git ls-files '.github/*.yml')

# A command continued over lines (a trailing `|` or `\`, or an awk program
# still open in its quotes) is read whole; a YAML block scalar's `run: |` is
# no pipe. Comment lines say what they like.
hits=$(awk -v sq="'" '
  BEGIN {
    into = "(^|[^|])[|]&?[[:space:]]*"
    to_head = into "head([^[:alnum:]_-]|$)"
    to_grep = into "grep[[:space:]]([^|]*[[:space:]])?(-[a-zA-Z]*[qlm]|--(quiet|silent|max-count|files-with-matches))"
    # The program is the quoted word: a | inside it is no pipe, an exit past it no awk exit.
    to_awk_exit = into "awk[[:space:]]([^|" sq "]|" sq "[^" sq "]*" sq ")*" \
      sq "([^" sq "]*[^a-zA-Z_])?exit([^a-zA-Z_]|$)"
  }
  FNR == 1 { buf = "" }
  buf == "" && /^[[:space:]]*#/ { next }
  {
    if (buf == "") start = FNR
    buf = buf " " $0
    if ($0 ~ /(\||\\)[[:space:]]*$/ && $0 !~ /:[[:space:]]*\|[-+]?[[:space:]]*$/) next
    if (buf ~ /\|[[:space:]]*awk[[:space:]]/ && gsub(sq, sq, buf) % 2) next
    if (buf ~ to_head || buf ~ to_grep || buf ~ to_awk_exit) print FILENAME ":" start ":" buf
    buf = ""
  }' "${files[@]}")

if [[ -n $hits ]]; then
  echo "pipefail-readers FAIL: a pipe into a reader that can leave early" >&2
  echo "  (test a here-string, or grep without -q into /dev/null; trim with" >&2
  echo "  grep -m N on a file or sed -n '1,Np'; let awk remember, not exit):" >&2
  printf '%s\n' "$hits" >&2
  exit 1
fi
echo "pipefail-readers OK (${#files[@]} files, no pipe into an early reader)"
