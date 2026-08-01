#!/usr/bin/env bash
# AOT-link a program that names Webview, and say whether it linked and ran.
#
# Usage: misc/probe_webview_aot_link.sh <culebra binary> [work dir]
#
# Webview is the one axis whose link pulls a DSO carrying libz, which is what
# makes ld diagnose the core archive's dead zlib references (see CMakeLists
# _webview_link). No tests/*.cul can host it: a driver built without the axis
# has no `Webview` name to scan for, and this is the sibling of
# probe_webview_window.sh — that one proves the window, this one the link.
#
# The name only has to be scanned, so no window is ever created. The link line
# is checked as well as the exit status: a driver that stopped force-loading
# the archive would build this program just as happily, and report nothing.
set -eu

bin=${1:?usage: probe_webview_aot_link.sh <culebra binary> [work dir]}
work=${2:-${TMPDIR:-/tmp}/culebra-webview-aot-link}

# The axis self-disables when the platform's WebView headers are missing, and
# CMakeCache is no proof of it (the option stays ON while the feature block
# turns it off in a normal variable) — the archive beside the driver is.
if [[ ! -f "$(dirname "$bin")/libculebra_rt_webview.a" ]]; then
  echo "probe_webview_aot_link: SKIP (driver has no Webview axis)"
  exit 0
fi

rm -rf "$work" && mkdir -p "$work"
printf 'if false { Webview.Window.new() }\nprint("webview link ok")\n' \
  > "$work/webview_link.cul"

if ! CULEBRA_VERBOSE=1 "$bin" build "$work/webview_link.cul" \
        -o "$work/webview_link" > "$work/build.log" 2>&1; then
  echo "probe_webview_aot_link: FAIL (link)" >&2
  cat "$work/build.log" >&2
  exit 1
fi
if ! grep -q "libculebra_rt_webview.a" "$work/build.log"; then
  echo "probe_webview_aot_link: FAIL (built without force-loading the axis)" >&2
  grep "link:" "$work/build.log" >&2 || true
  exit 1
fi
out=$("$work/webview_link")
if [[ "$out" != "webview link ok" ]]; then
  echo "probe_webview_aot_link: FAIL (binary printed [$out])" >&2
  exit 1
fi
echo "probe_webview_aot_link: OK"
