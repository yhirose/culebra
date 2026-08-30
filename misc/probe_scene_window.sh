#!/usr/bin/env bash
# Render one frame through Scene and check something was actually drawn.
#
# Usage: misc/probe_scene_window.sh <culebra binary> [engine flag] [work dir]
#
# Scene has no headless mode: View.new opens a window or raises. So this needs
# a display (CI runs it under xvfb-run) and it is the only check that enters
# raylib's window, GL and shader path at all — everything else about the axis
# passes on a binary whose rendering never runs.
#
# The assertion is "the box is not the background", not a colour: the shading
# is a shader's output on whatever GL the machine has, and pinning its RGB
# would be a test of the driver's rounding. Read back through Canvas, whose
# framebuffer is headless, so the comparison itself needs no window.
set -eu

exe=${1:?usage: probe_scene_window.sh <culebra binary> [engine flag] [work dir]}
[ "${exe#/}" = "$exe" ] && exe="$PWD/$exe"
engine=${2:---vm}
work=${3:-$(mktemp -d)}
cd "$work"

cat > scene_window.cul <<'EOF'
let view = Scene.View.new(160, 120, "scene smoke")
view.background(20, 24, 30)
view.sun(0.5, -0.8, -0.3, 1.2, 255, 245, 230)
view.ambient(0.4, 180, 200, 220)

let gold = view.material_pbr(230, 180, 60, 0.9, 0.3)
view.add_box(2.0, 2.0, 2.0).material(gold)

for i in 0..10 {
  view.camera(4.0, 3.0, 5.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 55.0)
  view.render_3d()
  view.present()
}
# Absolute: raylib resolves a relative screenshot path against its own base
# path, not the working directory, and the file lands beside the binary.
let shot_path = Sys.getcwd() + "/scene_smoke.png"
view.screenshot(shot_path)
view.drop()

# Back through Canvas (headless framebuffer) to read the pixels.
Canvas.init(160, 120)
let shot = Canvas.Sprite.from_png(FS.read(shot_path))
shot.draw(0, 0)
println(Canvas.get_pixel(80, 60) != Canvas.get_pixel(2, 2) ? "scene drew" : "scene blank")
EOF

out=$(CULEBRA_CANVAS_HEADLESS=1 "$exe" "$engine" scene_window.cul)
if [ "$out" != "scene drew" ]; then
  [ -n "${GITHUB_ACTIONS:-}" ] && prefix="::error::" || prefix="ERROR: "
  echo "${prefix}scene render ($engine): [$out]" >&2
  exit 1
fi
echo "OK: Scene rendered a frame ($engine)"
