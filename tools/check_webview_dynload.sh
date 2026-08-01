#!/usr/bin/env bash
# Webview dynamic-load gate (Linux).
#
# The Linux engine is dlopen'd at window creation, not linked
# (src/runtime/webview_gtk_dynload.cc). That is what lets one binary carry
# Webview and still start on a machine with no desktop stack — a headless
# server, a container, a desktop without webkitgtk-6.0. Two ways to lose it
# silently, both checked here:
#
#   1. Something puts gtk4/webkitgtk back on the link line. The binary keeps
#      working on this machine and stops starting on every machine without the
#      engine — the exact regression the released Linux asset used to carry.
#   2. The forwarders reach the dynamic symbol table. Once dlopen brings the
#      real libraries in, GTK's and WebKit's own internal calls bind to ours
#      instead: every GObject cast in the process routes through
#      g_type_check_instance_cast.
#
# Both are properties of the linked output, so this reads the output rather
# than the build files. `culebra build` output is checked the same way and by
# the same rules — an AOT binary is where a user's desktop app actually ships.
#
# Skips (exit 0) where there is no Webview axis to check: a driver built
# without gtk4/webkitgtk headers, or a non-Linux host.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="${1:-build}"
bin="$BUILD_DIR/culebra"

if [[ "$(uname -s)" != Linux ]]; then
  echo "check_webview_dynload: SKIP (Linux-only: macOS links a system framework,"
  echo "  Windows lets webview.h's own loader find the runtime)"
  exit 0
fi
[[ -x "$bin" ]] || { echo "check_webview_dynload: no $bin" >&2; exit 1; }
if [[ ! -f "$BUILD_DIR/libculebra_rt_webview.a" ]]; then
  echo "check_webview_dynload: SKIP (driver has no Webview axis)"
  exit 0
fi

# The engine's stack, by substring rather than by exact soname: culebra names
# only gtk4 and webkitgtk-6.0, but linking those drags libsoup and
# libjavascriptcore in too, and a GTK3-era build would carry different sonames
# for the same mistake. Any of them in DT_NEEDED is a binary that will not start
# without a desktop. Nothing culebra legitimately links matches these.
engine='gtk|webkit|javascriptcore|libsoup'
# The forwarders, by prefix. gdk_/jsc_ are in the set too (gdk_x11_display_get_type
# from a cast macro, jsc_value_to_string from the message bridge).
forwarders=' (gtk_|gdk_|webkit_|jsc_|g_[a-z])'

fail=0

check_binary() {
  local what=$1 path=$2
  echo "=== $what: $path ==="

  if ldd "$path" 2>/dev/null | grep -iE "$engine"; then
    echo "ERROR: $what links the WebKit stack — it will not start without it" >&2
    echo "  (the engine belongs behind dlopen: webview_gtk_dynload.cc)" >&2
    fail=1
  else
    echo "OK: no engine in DT_NEEDED"
  fi

  local exported
  exported=$(nm -D --defined-only "$path" 2>/dev/null | grep -E "$forwarders" || true)
  if [[ -n "$exported" ]]; then
    echo "ERROR: $what exports the forwarders; they would interpose on the real" >&2
    echo "  libraries once dlopen loads them:" >&2
    echo "$exported" >&2
    fail=1
  else
    echo "OK: no forwarder in the dynamic symbol table"
  fi
}

check_binary "driver" "$bin"

# An AOT binary that names Webview: force-loads the feature archive, so it
# carries the same forwarders and must hold the same two properties. Built into
# a temp dir; no window is ever created (the name only has to be scanned).
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
printf 'if false { Webview.Window.new() }\nprintln("ok")\n' > "$work/w.cul"
if ! "$bin" build "$work/w.cul" -o "$work/w" > "$work/build.log" 2>&1; then
  echo "ERROR: AOT build of a Webview program failed" >&2
  cat "$work/build.log" >&2
  exit 1
fi
check_binary "AOT binary" "$work/w"

# ...and it still runs, which is the property the whole axis exists for: this
# host has the engine, but the binary must not need it to start.
out=$("$work/w")
if [[ "$out" != ok ]]; then
  echo "ERROR: the AOT binary printed [$out]" >&2
  fail=1
fi

[[ $fail -eq 0 ]] && echo "check_webview_dynload: OK"
exit $fail
