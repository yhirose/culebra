#!/usr/bin/env bash
# Drive the Webview event loop the way Desktop.run does, and say whether it
# came back.
#
# Usage: misc/aot_axes/probe_webview_window.sh <culebra binary>
#
# This is the one layer the rest of the gate cannot reach: a build that links
# and then dies, hangs, or ignores a quit the moment it opens a window. Both
# Webview bugs found by hand were here — an LLVM symbol clash that segfaulted
# inside Mesa on the first create, and a quit that never woke the loop.
#
# A machine with no display is a legitimate outcome and passes; a hang or a
# crash is not. Single-sourced across the two CI jobs that run it and `just
# smoke-webview`, so no copy can drift lenient.
set -eu

bin=${1:?usage: probe_webview_window.sh <culebra binary>}

# macOS has no `timeout`; CI brew-installs coreutils for `gtimeout`.
tmo=$(command -v timeout || command -v gtimeout)

fail=0
# One window per process, like Desktop.run — several in one process is not a
# supported shape. Each probe's marker is checked against its own name so a
# script printing the other one's cannot pass.
for pair in "webview_probe:WEBVIEW_PROBE_OK" \
            "webview_pending_quit:WEBVIEW_PROBE_PENDING_QUIT_OK"; do
  probe=${pair%%:*}
  want=${pair#*:}
  for mode in "--vm" "--jit"; do
    echo "=== culebra $mode $probe.cul ==="
    set +e
    out=$("$tmo" 60 "$bin" $mode "tests/gui/$probe.cul" 2>&1); rc=$?
    set -e
    echo "out=[$out] rc=$rc"
    # Hang first: a run that printed the no-display message and *then* hung
    # must not be waved through by the message arm below.
    if [ "$rc" -eq 124 ]; then
      echo "ERROR: probe hung" >&2
      fail=1
    else
      case "$out" in
        *"$want"*)
          [ "$rc" -eq 0 ] && echo "OK: window, event loop and quit behaved" \
            || { echo "ERROR: rc=$rc after the OK marker" >&2; fail=1; } ;;
        *"failed to create window"*)
          echo "OK: no window engine here — clean failure, not a crash" ;;
        *)
          echo "ERROR: probe failed (rc=$rc; 139 = crashed)" >&2; fail=1 ;;
      esac
    fi
  done
done
exit $fail
