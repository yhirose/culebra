#!/usr/bin/env bash
# AOT-link a program that names Scene, and say whether it linked and ran.
#
# Usage: misc/probe_scene_aot_link.sh <culebra binary> [work dir]
#
# Naming Scene force-loads libculebra_rt_scene.a beside the core archive, and
# that archive reaches wrap.h: on PE, a `culebra_runtime_*` helper the compiler
# declined to inline meets the core archive's strong one and the link fails
# unless the Scene fragment carries --allow-multiple-definition (CMakeLists).
# The link is the subject here, so the Scene call sits under `if false` — the
# AOT scan still matches the name, nothing opens a window, and the check is the
# same on a runner with a desktop and one without. Rendering is
# probe_scene_window.sh's job.
#
# Sibling of probe_canvas_aot_link.sh and probe_webview_aot_link.sh.
set -eu

exe=${1:?usage: probe_scene_aot_link.sh <culebra binary> [work dir]}
[ "${exe#/}" = "$exe" ] && exe="$PWD/$exe"
work=${2:-$(mktemp -d)}
cd "$work"

cat > scene_aot.cul <<'EOF'
if false {
  let view = Scene.View.new(8, 8, "probe")
  view.add_box(1.0, 1.0, 1.0)
  view.drop()
}
println("scene aot ok")
EOF

"$exe" build scene_aot.cul -o scene_aot
out=$(./scene_aot)
if [ "$out" != "scene aot ok" ]; then
  [ -n "${GITHUB_ACTIONS:-}" ] && prefix="::error::" || prefix="ERROR: "
  echo "${prefix}AOT scene run: [$out]" >&2
  exit 1
fi
echo "OK: AOT force-load linked and ran"
