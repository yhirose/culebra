#!/usr/bin/env bash
# AOT-link a program that names Canvas, and say whether it linked and ran.
#
# Usage: misc/aot_axes/probe_canvas_aot_link.sh <culebra binary> [work dir]
#
# Naming Canvas has to force-load libculebra_rt_canvas.a so its strong raylib
# bodies override culebra_rt's weak headless stubs. The driver links that
# archive directly, so a broken choke shows up here and nowhere else — no
# tests/*.cul can host it, because a driver built without the window axis has
# the same `Canvas` name and a different archive behind it.
#
# Sibling of probe_webview_aot_link.sh, and the same reason for existing: the
# window-ON CI jobs otherwise each carried their own copy of this heredoc.
# Headless throughout — the axis under test is the link, not the display.
set -eu

exe=${1:?usage: probe_canvas_aot_link.sh <culebra binary> [work dir]}
[ "${exe#/}" = "$exe" ] && exe="$PWD/$exe"
work=${2:-$(mktemp -d)}
cd "$work"

cat > canvas_aot.cul <<'EOF'
Canvas.init(8, 8)
Canvas.clear(Canvas.rgba(0, 0, 255))
Canvas.present()
println(Canvas.get_pixel(0, 0) == Canvas.rgba(0, 0, 255))
EOF

"$exe" build canvas_aot.cul -o canvas_aot
out=$(CULEBRA_CANVAS_HEADLESS=1 ./canvas_aot)
if [ "$out" != "true" ]; then
  [ -n "${GITHUB_ACTIONS:-}" ] && prefix="::error::" || prefix="ERROR: "
  echo "${prefix}AOT canvas run: [$out]" >&2
  exit 1
fi
echo "OK: AOT force-load resolved"
