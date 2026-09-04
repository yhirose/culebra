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
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# A TTF the tree already carries (the MNIST demo's UI font), for the font path.
TTF="$ROOT/examples/tensor/mnist/assets/Inter-SemiBold-subset.ttf"
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

# --- the 2D layer: a font, shapes, sprites, a clip, paths ---
let ttf = Sys.argv[0]
let digits = view.font(ttf, 24, "0123456789:.")
let same = view.font_bytes(FS.read(ttf), 24, "0123456789:.")
println("font {digits.size()} glyphs {digits.glyphs()} bytes-form {same.size()} {same.glyphs()}")
let stamped = Scene.Image.new(16, 16).fill(0, 0, 0).text("7", 4, 2, 12, 255, 255, 255, 255, digits)
println("image text with a font drew: {stamped.get(8, 8) != stamped.get(0, 0) || stamped.get(7, 6) != stamped.get(0, 0) || stamped.get(9, 9) != stamped.get(0, 0)}")
let w20 = view.text_width("01:23.456", 20, digits)
let w40 = view.text_width("01:23.456", 40, digits)
let wdef = view.text_width("01:23.456", 20)
let h20 = view.text_height("01:23.456", 20, digits)
println("text_width grows {w40 > w20 && w20 > 0.0} default {wdef > 0.0} height {h20 > 0.0}")
view.begin_2d()
view.rect_round(20.0, 20.0, 280.0, 200.0, 0.3, 200, 200, 210)
view.rect_line(20.0, 20.0, 280.0, 200.0, 2.0, 0, 0, 0)
view.rect_round_line(30.0, 30.0, 100.0, 40.0, 0.5, 2.0, 255, 0, 0)
view.rect_gradient(140.0, 30.0, 100.0, 40.0, 255, 0, 0, 0, 0, 255)
view.rect_gradient(140.0, 80.0, 100.0, 40.0, 255, 0, 0, 0, 0, 255, true)
view.circle_line(60.0, 120.0, 20.0, 0, 0, 0)
view.circle_gradient(60.0, 120.0, 15.0, 255, 255, 0, 255, 0, 0)
view.ring(200.0, 150.0, 20.0, 30.0, 0.0, 270.0, 0, 120, 0)
view.triangle(30.0, 200.0, 60.0, 160.0, 90.0, 200.0, 0, 0, 255)     # one winding
view.triangle(100.0, 160.0, 130.0, 200.0, 160.0, 160.0, 0, 0, 255)  # the other
view.poly(250.0, 60.0, 6, 20.0, 30.0, 120, 0, 120)
view.text("01:23.456", 30.0, 90.0, 24, 0, 0, 0, digits)
view.text("01:23.456", 30.0, 120.0, 24, 0, 0, 0, digits, 2.0, 15.0)
view.text("plain", 200.0, 200.0, 10, 0, 0, 0)
view.sprite(checks, 240.0, 100.0, 40.0, 40.0)
view.sprite(checks, 280.0, 150.0, 30.0, 30.0, 45.0, 15.0, 15.0, 255, 128, 128)
view.sprite_rec(painted, 0.0, 0.0, 16.0, 16.0, 200.0, 110.0, 32.0, 32.0)
let baked = Scene.Image.new(16, 16).fill(0, 200, 0).text("7", 4, 2, 12, 0, 0, 0, 255, digits)
let up = view.texture(baked, false, false).filter("point").wrap("clamp")
view.sprite(up, 300.0, 200.0, 16.0, 16.0)
let frompng = view.texture_png(baked.to_png())
view.sprite(frompng, 300.0, 220.0, 16.0, 16.0)
view.clip(0.0, 0.0, 50.0, 50.0)
view.rect(0.0, 0.0, 320.0, 240.0, 255, 0, 255)   # only the clipped corner shows
view.clip_end()
view.path_begin()
view.path_to(160.0, 210.0)
view.path_to(180.0, 230.0)
view.path_to(200.0, 210.0)
view.path_to(180.0, 190.0)
view.path_close()
view.path_fill(0, 200, 200)
view.path_stroke(1.0, 0, 0, 0)
view.path_begin()
for i in 0..6 {
  view.path_to(20.0 + i * 50.0, 230.0 - (i % 2) * 6.0)
  view.path_to(20.0 + i * 50.0, 236.0 - (i % 2) * 6.0)
}
view.path_strip(200, 120, 0)
view.path_begin()
for i in 0..6 { view.path_to(20.0 + i * 50.0, 5.0 + (i % 2) * 10.0) }
view.path_spline(2.0, 0, 0, 0)
view.screenshot("scene_api_2d.png")
view.present()
let nofont = try {
  view.font("no/such/font.ttf", 12)
  "accepted"
} catch e {
  e.kind
}
println("missing font: {nofont}")

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
Canvas.clear(Canvas.rgba(0, 0, 0))
let flat = Canvas.Sprite.from_png(FS.read("scene_api_2d.png"))
flat.draw(0, 0)
println("2d frame drew: {Canvas.get_pixel(160, 120) != Canvas.get_pixel(315, 235)}")
println("clip cut: {Canvas.get_pixel(45, 45) != Canvas.get_pixel(60, 60)}")
EOF

# Transparency and draw order, judged by pixels: a red opaque plate behind two
# half-transparent plates (blue, green) that overlap each other. Three frames:
# the plates hidden, shown, and with the green one's order pulled ahead.
cat > scene_alpha.cul <<'EOF'
let view = Scene.View.new(200, 150, "scene alpha")
view.background(20, 24, 30)
view.sun(0.0, -1.0, -0.3, 1.0, 255, 255, 255)
view.ambient(0.6, 255, 255, 255)
let red = view.add_material().rgb(255, 0, 0).unlit()
let blue = view.add_material().rgb(0, 0, 255).unlit().opacity(128)
let green = view.add_material().rgb(0, 255, 0).unlit().opacity(128)
# every other knob, on a plate off to the side, so each binding runs
let odd = view.add_material().rgb(255, 255, 0).blend("add").depth_write(false).depth_test(true).double_sided()
odd.cutout(0.5).emissive(255, 128, 0, 0.5).fog(false).casts_shadow(false).uv(2.0, 2.0, 0.25, 0.0).blend("nosuch")
let back = view.add_box(6.0, 6.0, 0.2).material(red)
let plate_b = view.add_box(2.0, 2.0, 0.2).move(0.0, 0.0, 2.0).material(blue)
let plate_g = view.add_box(2.0, 2.0, 0.2).move(0.5, 0.0, 2.0).material(green)
view.add_box(1.0, 1.0, 0.2).move(-4.5, 0.0, 2.0).material(odd).opacity(200)
view.camera(0.0, 0.0, 10.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 50.0)
fn shot(path) {
  view.render_3d()
  view.screenshot(path)
  view.present()
}
plate_b.hide()
plate_g.hide()
shot("alpha_opaque.png")
plate_b.show()
plate_g.show()
shot("alpha_blend.png")
plate_g.order(-1)
shot("alpha_order.png")
view.drop()

Canvas.init(200, 150)
fn px(path, x, y) {
  Canvas.clear(Canvas.rgba(0, 0, 0))
  Canvas.Sprite.from_png(FS.read(path)).draw(0, 0)
  Canvas.get_pixel(x, y)
}
fn red_of(v) { v & 255 }
fn blue_of(v) { (v >> 16) & 255 }
let o = px("alpha_opaque.png", 100, 75)
let bl = px("alpha_blend.png", 100, 75)
println("blended: {red_of(bl) < red_of(o) && blue_of(bl) > blue_of(o)}")
println("depth kept: {px("alpha_opaque.png", 140, 75) == px("alpha_blend.png", 140, 75)}")
println("order matters: {px("alpha_blend.png", 105, 75) != px("alpha_order.png", 105, 75)}")
println("background untouched: {px("alpha_opaque.png", 5, 5) == px("alpha_order.png", 5, 5)}")
EOF

# Scene.Image needs no window, so this program opens none: the one part of Scene
# that runs with no display at all, and the one whose answers are exact pixels.
cat > scene_image.cul <<'EOF'
let img = Scene.Image.new(8, 8)
img.fill(0, 0, 0)
img.rect(0, 0, 4, 4, 255, 0, 0)
img.pixel(7, 7, 0, 255, 0)
println("px {img.get(1, 1)} {img.get(6, 6)} {img.get(7, 7)} size {img.width()}x{img.height()}")
let png = img.to_png()
let back = Scene.Image.from_png(png)
println("roundtrip {png.size() > 0} {back.get(1, 1) == img.get(1, 1)} {back.get(7, 7) == img.get(7, 7)} {back.width()}")
# channel rounding at a gradient's far end is libm's business; the direction is ours
let g = Scene.Image.new(4, 4).gradient(255, 0, 0, 0, 0, 255)
println("gradient v: top-red {g.get(0, 0) >> 24 == 255} bottom-blue {(g.get(0, 3) >> 8) & 255 >= 250}")
let gh = Scene.Image.new(4, 4).gradient(255, 0, 0, 0, 0, 255, 90)
println("gradient h: left-red {gh.get(0, 0) >> 24 == 255} right-blue {(gh.get(3, 0) >> 8) & 255 >= 250}")
let n = Scene.Image.new(4, 4).fill(128, 128, 128).to_normal(1.0)
println("flat normal {n.get(2, 2)}")
let c = img.copy().flip_h().flip_v()
println("copy flipped {c.get(7, 7)} {c.get(0, 0)}")
let big = Scene.Image.new(2, 2).fill(10, 20, 30).resize(6, 6).crop(1, 1, 3, 3)
println("resize+crop {big.width()}x{big.height()} {big.get(0, 0)}")
let rad = Scene.Image.new(9, 9).gradient_radial(0.0, 255, 255, 255, 0, 0, 0)
println("radial centre brighter {rad.get(4, 4) >> 24 > rad.get(0, 0) >> 24}")
let nz = Scene.Image.new(8, 8).fill(100, 100, 100).noise(7, 2.0, 128).cellular(4, 64).blur(1).tint(255, 200, 200).brightness(10).grayscale().invert().rotate(90)
println("passes ran {nz.width()}x{nz.height()}")
let l = Scene.Image.new(8, 8).line(0.0, 0.0, 7.0, 7.0, 1, 255, 255, 255).triangle(0.0, 7.0, 7.0, 7.0, 7.0, 0.0, 0, 0, 255).circle(4, 4, 1, 0, 255, 0).circle_line(4, 4, 3, 255, 0, 0).rect_line(0, 0, 8, 8, 9, 9, 9)
println("shapes {l.get(0, 0)} {l.get(4, 4)}")
let stamp = Scene.Image.new(4, 4).fill(0, 0, 255)
let onto = Scene.Image.new(8, 8).fill(0, 0, 0).blit(stamp, 2, 2).blit_rot(stamp, 6.0, 6.0, 45.0, 0.5)
println("blit {onto.get(3, 3)} {onto.get(0, 0)}")
let t = Scene.Image.new(16, 8).text("hi", 0, 0, 8, 255, 255, 255)
println("text without a window: {t.get(1, 1)}")
let bad = try {
  Scene.Image.from_png("not a png")
  "accepted"
} catch e {
  e.kind
}
println("bad png: {bad}")
EOF

[ -n "${GITHUB_ACTIONS:-}" ] && prefix="::error::" || prefix="ERROR: "
fail=0
for engine in "--vm" "--jit"; do
  if ! "$CULEBRA" "$engine" scene_image.cul > "img${engine}.txt" 2> "img${engine}.err"; then
    echo "${prefix}scene_api_test: Image program $engine exited non-zero:" >&2
    cat "img${engine}.txt" "img${engine}.err" >&2
    fail=1
  fi
done
[ $fail -eq 0 ] || exit 1
if ! diff "img--vm.txt" "img--jit.txt" > diff.txt; then
  echo "${prefix}scene_api_test: the VM and the JIT disagree on Scene.Image:" >&2
  cat diff.txt >&2
  exit 1
fi
# Exact pixels: the CPU raster is the same bytes on every platform.
for want in "px 4278190335 255 16711935 size 8.0x8.0" \
            "roundtrip true true true 8.0" \
            "gradient v: top-red true bottom-blue true" \
            "gradient h: left-red true right-blue true" \
            "flat normal 2155937791" \
            "copy flipped 4278190335 16711935" \
            "resize+crop 3.0x3.0 169090815" \
            "radial centre brighter true" \
            "passes ran 8.0x8.0" \
            "shapes 151587327 16711935" \
            "blit 65535 255" \
            "text without a window: 0" \
            "bad png: RuntimeError"; do
  if ! grep -qxF "$want" "img--vm.txt"; then
    echo "${prefix}scene_api_test: Image line missing: $want" >&2
    cat "img--vm.txt" >&2
    fail=1
  fi
done
# raylib's own log lines must not reach a script's stdout (they go to stderr).
if grep -q "INFO:\|WARNING:" "img--vm.txt"; then
  echo "${prefix}scene_api_test: raylib logged into stdout:" >&2
  cat "img--vm.txt" >&2
  fail=1
fi
[ $fail -eq 0 ] || exit 1
echo "OK: Scene.Image agrees on both engines, with no display ($(wc -l < img--vm.txt | tr -d ' ') lines)"

for engine in "--vm" "--jit"; do
  if ! CULEBRA_CANVAS_HEADLESS=1 "$CULEBRA" "$engine" scene_alpha.cul > "alpha${engine}.txt" 2>&1; then
    echo "${prefix}scene_api_test: transparency program $engine exited non-zero:" >&2
    cat "alpha${engine}.txt" >&2
    fail=1
  fi
done
[ $fail -eq 0 ] || exit 1
if ! diff "alpha--vm.txt" "alpha--jit.txt" > diff.txt; then
  echo "${prefix}scene_api_test: the VM and the JIT disagree on transparency:" >&2
  cat diff.txt >&2
  exit 1
fi
for want in "blended: true" "depth kept: true" "order matters: true" "background untouched: true"; do
  if ! grep -qxF "$want" "alpha--vm.txt"; then
    echo "${prefix}scene_api_test: transparency line missing: $want" >&2
    cat "alpha--vm.txt" >&2
    fail=1
  fi
done
[ $fail -eq 0 ] || exit 1
echo "OK: transparency blends, leaves the depth channel alone, and honours order"

for engine in "--vm" "--jit"; do
  if ! CULEBRA_CANVAS_HEADLESS=1 "$CULEBRA" "$engine" scene_api.cul "$TTF" > "out${engine}.txt" 2>&1; then
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
expect "font 24.0 glyphs 12 bytes-form 24.0 12"
expect "text_width grows true default true height true"
expect "image text with a font drew: true"
expect "missing font: RuntimeError"
expect "2d frame drew: true"
expect "clip cut: true"
expect "texture(material): TypeError"
expect "material(texture): TypeError"
expect "texture(dropped): ClosedError"
expect "nested canvas: RuntimeError"
[ $fail -eq 0 ] || exit 1

echo "OK: every Scene method binds and the engines agree ($(wc -l < out--vm.txt | tr -d ' ') lines)"
