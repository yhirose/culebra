#!/usr/bin/env bash
# Assert a build really has the Webview namespace in it.
#
# Usage: misc/verify_webview_axis.sh <build dir>
#
# The sibling of verify_canvas_window.sh, and needed for the same reason: a
# missing dev package turns the axis off and the build succeeds anyway, so a
# download can claim Webview and have no `Webview` name at all. Every later
# check passes on such a build — there is nothing left in it to fail.
#
# Unlike Canvas, the cache cannot answer here: CULEBRA_ENABLE_WEBVIEW stays ON
# in CMakeCache while the feature block turns it off in a plain variable. The
# feature archive beside the driver is the honest answer.
set -eu

build=${1:?usage: verify_webview_axis.sh <build dir>}

if [ ! -f "$build/libculebra_rt_webview.a" ]; then
  echo "ERROR: built without Webview — the platform's engine headers were missing" >&2
  grep -i webview "$build/CMakeCache.txt" >&2 || true
  exit 1
fi
echo "OK: Webview axis built"
