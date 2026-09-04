#!/usr/bin/env bash
# Name every Scene method once, on both engines, and assert the two runs agree.
#
# Usage: tests/scene_api_test.sh <culebra binary>
#
# Scene has no headless mode, so this needs a display (CI runs it under
# xvfb-run; locally a small window opens for a moment). What it checks is the
# API surface, not the picture: that each method binds, that its arguments are
# marshalled the same way by the VM and the JIT, that the handle-typed
# arguments reject the wrong handle, and that the answers a display-less input
# state gives are the deterministic ones. Rendering is
# misc/aot_axes/probe_scene_window.sh's job.
set -u
CULEBRA="${1:?usage: scene_api_test.sh <culebra binary>}"
[ "${CULEBRA#/}" = "$CULEBRA" ] && CULEBRA="$PWD/$CULEBRA"
# Under TMPDIR: macOS's mktemp otherwise picks /var/folders, which a sandbox
# may not let us write.
OUT=$(mktemp -d "${TMPDIR:-/tmp}/scene-api.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
cd "$OUT"

cat > scene_api.cul <<'EOF'
let view = Scene.View.new(160, 120, "scene api")
view.target_fps(60)
view.background(20, 24, 30)
view.sky(120, 160, 210, 190, 205, 225)
view.sun(0.5, -0.8, -0.3, 1.2, 255, 245, 230)
view.ambient(0.4, 180, 200, 220)
view.fog(50.0, 400.0, 190, 205, 225)

# --- textures: generated, and drawn by the 2D calls ---
let checks = view.checker(64, 4, 200, 200, 200, 40, 40, 40)
let grain = view.grain(32, 90, 100, 80, 40)
let painted = view.canvas(32, 32)
view.rect(0.0, 0.0, 16.0, 16.0, 255, 0, 0)
view.circle(24.0, 24.0, 6.0, 0, 0, 255)
view.line(0.0, 31.0, 31.0, 0.0, 2.0, 0, 255, 0)
view.text("x", 2.0, 2.0, 8, 0, 0, 0)
view.canvas_end()
println("checker {checks.width()}x{checks.height()} grain {grain.width()} canvas {painted.width()}x{painted.height()}")

# --- materials: fluent handles, a texture by handle or nil ---
let gold = view.add_material().rgb(230, 180, 60).pbr(0.9, 0.3)
let livery = view.add_material().texture(painted)
let bare = view.add_material().texture(checks).texture(nil)

# --- scene graph ---
let box = view.add_box(2.0, 2.0, 2.0).material(gold)
let ball = view.add_sphere(0.5).move(3.0, 0.5, 0.0).material(livery)
view.add_cylinder(0.3, 1.0).move(-3.0, 0.5, 0.0).material(bare).tint(200, 60, 60)
view.add_plane(20.0, 20.0).move(0.0, -1.0, 0.0).material(view.add_material().texture(grain))
let root = view.add_node().move(0.0, 3.0, 0.0)
let child = root.add_box(0.5, 0.5, 0.5).yaw(0.3).pitch(0.1).roll(0.2).spin(0.0, 1.0, 0.0, 0.5)
child.scale(1.0).scale3(1.0, 2.0, 1.0).name("child").hide()
child.show()
root.add_sphere(0.2).add_cylinder(0.1, 0.1).add_plane(1.0, 1.0)
let mesh = view.add_mesh()
mesh.vertex(-1.0, 0.0, -1.0, 0.0, 1.0, 0.0)
mesh.vertex(1.0, 0.0, -1.0, 0.0, 1.0, 0.0)
mesh.vertex_uv(1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 1.0)
mesh.tri(0, 1, 2)
mesh.build()
root.add_mesh().tri(0, 0, 0)   # nothing pushed: build is a no-op, never an error
println("ball {ball.x()} {ball.y()} {ball.z()}")

# --- a 3D frame with an overlay, then a 2D-only frame ---
view.camera(4.0, 3.0, 5.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 55.0)
view.render_3d()
view.alpha(128)
view.text("hud", 4.0, 4.0, 10, 255, 255, 255)
view.rect(4.0, 20.0, 40.0, 8.0, 0, 0, 0)
view.alpha(255)
view.present()
view.begin_2d()
view.rect(0.0, 0.0, 160.0, 120.0, 30, 30, 30)
view.present()
println("frame {view.width()}x{view.height()} dt>=0 {view.dt() >= 0.0}")

# --- the window: a resize is drawn at the new size, not stretched from the old ---
view.title("scene api (resized)")
view.resizable(true)
view.size(320, 240)
view.render_3d()
view.present()   # the resize is noticed here at the latest (the event pump)
view.render_3d()
view.screenshot("scene_api_resized.png")   # a frame at the new size
view.present()
let r1 = view.resized()   # the edge is per frame; a Bool either way
view.set_clipboard("scene")
let clip = view.clipboard()
println("window {view.width()}x{view.height()} fps>=0 {view.fps() >= 0} time>0 {view.time() > 0.0} resized {r1 == true || r1 == false} clipboard '{clip}'")
view.supersample(1)
view.clip_planes(0.5, 500.0)
view.render_3d()
view.present()
view.fullscreen(true)
let fs = view.is_fullscreen()
view.render_3d()
view.present()
view.fullscreen(false)
view.vsync(true)
view.cursor(false)
view.cursor(true)
view.mouse_capture(true)
view.mouse_capture(false)
println("fullscreen-was {fs == true || fs == false}")
let mx = view.mouse_x()
let mwheel = view.mouse_wheel()
println("mouse {mx >= 0.0 || mx < 0.0} dx {view.mouse_dx()} wheel {mwheel} {view.mouse("left")} {view.mouse_pressed("right")} {view.mouse("wheel")}")

# --- input: nothing is pressed on a machine nobody is touching ---
let k1 = view.key("escape")
let k2 = view.key_pressed("a")
let k3 = view.key_released(" ")
let k4 = view.key("nosuchkey")
println("keys {k1} {k2} {k3} {k4}")
let p1 = view.pad_available()
let p2 = view.pad("a")
let p3 = view.pad_pressed("start", 1)
let p4 = view.pad_axis("lx")
let p5 = view.pad_axis("bogus", 2)
println("pad {p1} {p2} {p3} {p4} {p5} '{view.pad_name()}'")
view.rumble(0.5, 0.5, 0.1)
view.rumble(0.5, 0.5, 0.1, 1)

# --- the exit is quit(), not Esc ---
let c0 = view.closing()
view.quit()
let c1 = view.closing()
println("closing {c0} -> {c1}")

# --- a relative screenshot path means the working directory ---
view.begin_2d()
view.screenshot("scene_api.png")
view.present()
println("screenshot {FS.exists("scene_api.png")}")

# --- the handle types are the contract ---
let wrong = try {
  view.add_material().texture(gold)
  "accepted"
} catch e {
  e.kind
}
println("texture(material): {wrong}")
let wrong2 = try {
  view.add_box(1.0, 1.0, 1.0).material(checks)
  "accepted"
} catch e {
  e.kind
}
println("material(texture): {wrong2}")
let stale = view.checker(8, 2, 0, 0, 0, 255, 255, 255)
stale.drop()
let dropped = try {
  view.add_material().texture(stale)
  "accepted"
} catch e {
  e.kind
}
println("texture(dropped): {dropped}")
view.canvas(8, 8)
let nested = try {
  view.canvas(8, 8)
  "accepted"
} catch e {
  e.kind
}
view.canvas_end()
println("nested canvas: {nested}")
view.drop()

# The frame drawn after the resize has content at the new size (read back
# through Canvas's headless framebuffer, as the render probe does).
Canvas.init(320, 240)
let shot = Canvas.Sprite.from_png(FS.read("scene_api_resized.png"))
shot.draw(0, 0)
println("resized frame drew: {Canvas.get_pixel(160, 120) != Canvas.get_pixel(2, 2)}")
EOF

[ -n "${GITHUB_ACTIONS:-}" ] && prefix="::error::" || prefix="ERROR: "
fail=0
for engine in "--vm" "--jit"; do
  if ! CULEBRA_CANVAS_HEADLESS=1 "$CULEBRA" "$engine" scene_api.cul > "out${engine}.txt" 2>&1; then
    echo "${prefix}scene_api_test: $engine exited non-zero:" >&2
    cat "out${engine}.txt" >&2
    fail=1
  fi
done
[ $fail -eq 0 ] || exit 1

if ! diff "out--vm.txt" "out--jit.txt" > diff.txt; then
  echo "${prefix}scene_api_test: the VM and the JIT disagree:" >&2
  cat diff.txt >&2
  exit 1
fi

# The answers that are fixed by construction, whichever engine ran them.
expect() {
  if ! grep -qxF "$1" "out--vm.txt"; then
    echo "${prefix}scene_api_test: expected line missing: $1" >&2
    cat "out--vm.txt" >&2
    fail=1
  fi
}
expect "checker 64.0x64.0 grain 32.0 canvas 32.0x32.0"
expect "ball 3.0 0.5 0.0"
expect "keys false false false false"
expect "closing false -> true"
expect "screenshot true"
expect "window 320.0x240.0 fps>=0 true time>0 true resized true clipboard 'scene'"
expect "fullscreen-was true"
expect "mouse true dx 0.0 wheel 0.0 false false false"
expect "resized frame drew: true"
expect "texture(material): TypeError"
expect "material(texture): TypeError"
expect "texture(dropped): ClosedError"
expect "nested canvas: RuntimeError"
[ $fail -eq 0 ] || exit 1

echo "OK: every Scene method binds and the engines agree ($(wc -l < out--vm.txt | tr -d ' ') lines)"
