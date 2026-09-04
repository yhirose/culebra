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

# --- the graph as data, and culling that changes nothing visible ---
let tree = view.add_node().move(1.0, 2.0, 3.0).name("tree")
let leaf = tree.add_box(0.2, 0.2, 0.2).move(1.0, 0.0, 0.0).name("leaf")
leaf.add_sphere(0.1).name("fruit")
println("graph {tree.child_count()} {tree.child_at(0).world_x()} {view.find("fruit").world_z()} {view.has("leaf")} {tree.has("nope")} {leaf.vertex_count()} {mesh.vertex_count()}")
let missing = try {
  view.find("nope")
  "accepted"
} catch e {
  e.kind
}
let oob = try {
  tree.child_at(5)
  "accepted"
} catch e {
  e.kind
}
println("find/child_at errors: {missing} {oob}")
leaf.quat(0.0, 0.7071, 0.0, 0.7071).billboard().billboard(false).cull_radius(-1.0)
leaf.remove()
println("removed {tree.child_count()} {view.has("fruit")}")
view.remove(tree)
println("root removed {view.has("tree")}")
view.add_box(1.0, 1.0, 1.0).move(1000.0, 0.0, 0.0)   # off screen: culled, or drawn to nothing
let bumps = Scene.Image.new(8, 8).fill(128, 128, 128).noise(1, 2.0, 255).to_normal(2.0)
let bumpy = view.add_material().rgb(180, 180, 180).normal_map(view.texture(bumps), 1.0)
view.add_box(1.0, 1.0, 1.0).move(-3.0, 2.0, 0.0).material(bumpy)
view.camera(4.0, 3.0, 5.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 55.0)
view.culling(true)
view.render_3d()
view.screenshot("cull_on.png")
view.present()
view.culling(false)
view.render_3d()
view.screenshot("cull_off.png")
view.present()
view.culling(true)
println("culling invisible: {FS.read("cull_on.png") == FS.read("cull_off.png")}")

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

# A second camera into a texture, and the one flip model behind render targets
# and canvases: a beacon only camera B sees lands in the top half of the target
# when drawn as a sprite (upright), a canvas painted red-over-blue reads red on
# top as a sprite, and uv(-1, …) on a plate mirrors what the plain plate shows.
cat > scene_mirror.cul <<'EOF'
let view = Scene.View.new(320, 240, "scene mirror")
view.background(20, 24, 30)
view.ambient(1.0, 255, 255, 255)
let magenta = view.add_material().rgb(255, 0, 255).unlit()
# behind camera A (which looks down -z from z = 10), above camera B's aim
view.add_box(1.0, 1.0, 1.0).move(0.0, 3.0, 20.0).material(magenta)
let target = view.render_target(64, 32)
view.render_to(target, 0.0, 1.0, 10.0, 0.0, 1.0, 20.0, 0.0, 1.0, 0.0, 60.0)
let painted = view.canvas(32, 32)
view.rect(0.0, 0.0, 32.0, 16.0, 255, 0, 0)
view.rect(0.0, 16.0, 32.0, 16.0, 0, 0, 255)
view.canvas_end()
let split = view.canvas(32, 32)
view.rect(0.0, 0.0, 16.0, 32.0, 255, 0, 0)
view.rect(16.0, 0.0, 16.0, 32.0, 0, 0, 255)
view.canvas_end()
let plain = view.add_material().texture(split).unlit()
let mirrored = view.add_material().texture(split).unlit().uv(-1.0, 1.0, 1.0, 0.0)
view.add_box(4.0, 2.0, 0.1).move(-2.5, 0.0, 0.0).material(plain)
view.add_box(4.0, 2.0, 0.1).move(2.5, 0.0, 0.0).material(mirrored)
view.camera(0.0, 0.0, 10.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 50.0)
view.render_3d()
view.screenshot("mirror_plates.png")
view.present()
view.begin_2d()
view.sprite(target, 0.0, 0.0, 64.0, 32.0)
view.sprite(painted, 100.0, 0.0, 32.0, 32.0)
view.screenshot("mirror_sprites.png")
view.present()
view.drop()

Canvas.init(320, 240)
fn load(path) {
  Canvas.clear(Canvas.rgba(0, 0, 0))
  Canvas.Sprite.from_png(FS.read(path)).draw(0, 0)
}
fn red_of(v) { v & 255 }
fn green_of(v) { (v >> 8) & 255 }
fn blue_of(v) { (v >> 16) & 255 }
fn is_magenta(v) { red_of(v) > 150 && blue_of(v) > 150 && green_of(v) < 100 }
fn is_red(v) { red_of(v) > 150 && blue_of(v) < 100 }
fn is_blue(v) { blue_of(v) > 150 && red_of(v) < 100 }
load("mirror_sprites.png")
mut top = 0
mut bottom = 0
for y in 0..32 {
  for x in 0..64 {
    if is_magenta(Canvas.get_pixel(x, y)) {
      if y < 16 { top += 1 } else { bottom += 1 }
    }
  }
}
println("beacon in the target: {top > 0} upright: {bottom == 0}")
println("canvas upright as a sprite: {is_red(Canvas.get_pixel(116, 4)) && is_blue(Canvas.get_pixel(116, 28))}")
load("mirror_plates.png")
let a_left = Canvas.get_pixel(60, 120)
let a_right = Canvas.get_pixel(130, 120)
let b_left = Canvas.get_pixel(190, 120)
let b_right = Canvas.get_pixel(260, 120)
println("plain plate split: {is_red(a_left) != is_red(a_right)}")
println("mirrored plate swaps: {is_red(a_left) == is_red(b_right) && is_red(a_right) == is_red(b_left)}")
EOF

# Colour grading and the post knobs: the same red box through no LUT, an
# identity LUT (must match) and a red/green-swapping LUT (must swap), then with
# every knob turned and with the post pass off. A 16-slice LUT quantizes to
# about 8 per channel, so the identity tolerance leaves room for that.
cat > scene_lut.cul <<'EOF'
let view = Scene.View.new(160, 120, "scene lut")
view.background(20, 24, 30)
view.ambient(1.0, 255, 255, 255)
let red = view.add_material().rgb(255, 0, 0).unlit()
view.add_box(3.0, 3.0, 3.0).material(red)
view.camera(0.0, 0.0, 8.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 50.0)
fn lut_image(swap) {
  let n = 16
  let img = Scene.Image.new(n * n, n)
  for b in 0..n {
    for g in 0..n {
      for r in 0..n {
        let rv = r * 255 / (n - 1)
        let gv = g * 255 / (n - 1)
        let bv = b * 255 / (n - 1)
        if swap {
          img.pixel(b * n + r, g, gv, rv, bv)
        } else {
          img.pixel(b * n + r, g, rv, gv, bv)
        }
      }
    }
  }
  img
}
let identity = view.texture(lut_image(false), false, false).filter("point").wrap("clamp")
let swapped = view.texture(lut_image(true), false, false).filter("point").wrap("clamp")
fn shot(path) {
  view.render_3d()
  view.screenshot(path)
  view.present()
}
shot("lut_none.png")
view.lut(identity)
shot("lut_identity.png")
view.lut(swapped)
shot("lut_swap.png")
view.lut(nil)
view.exposure(2.0)
view.saturation(1.5)
view.bloom(0.5, 2.0)
view.dof(0.5, 2.0)
view.ssao(0.3, 2.0)
view.vignette(0.5)
shot("lut_knobs.png")
view.post(false)
shot("lut_nopost.png")
view.drop()

Canvas.init(160, 120)
fn px(path) {
  Canvas.clear(Canvas.rgba(0, 0, 0))
  Canvas.Sprite.from_png(FS.read(path)).draw(0, 0)
  Canvas.get_pixel(80, 60)
}
fn red_of(v) { v & 255 }
fn green_of(v) { (v >> 8) & 255 }
fn blue_of(v) { (v >> 16) & 255 }
fn close(a, b) { Math.abs(a - b) <= 12 }
let none = px("lut_none.png")
let ident = px("lut_identity.png")
let sw = px("lut_swap.png")
let knobs = px("lut_knobs.png")
let nopost = px("lut_nopost.png")
println("identity lut matches: {close(red_of(none), red_of(ident)) && close(green_of(none), green_of(ident)) && close(blue_of(none), blue_of(ident))}")
println("swap lut swaps: {close(red_of(sw), green_of(none)) && close(green_of(sw), red_of(none))}")
println("knobs change the frame: {knobs != none}")
println("post off still draws: {red_of(nopost) > 150 && green_of(nopost) < 60}")
EOF

# PCM audio: the stream is fed from the script on the main thread, never from
# the audio thread, so this runs the same whether the machine has an audio
# device (locally) or none (CI): every answer below holds either way.
cat > scene_audio.cul <<'EOF'
let a = Scene.Audio.new(44100, 1, 1024)
println("latency ok {a.latency() > 0.046 && a.latency() < 0.047} ready is a bool {a.ready() == true || a.ready() == false}")
for i in 0..100 { a.push(Math.sin(i * 0.1) * 0.5) }
println("pending 100: {a.pending() == 100}")
let n = a.submit()
println("submit hands over: {n == 100 || n == 0} pending after {a.pending() <= 100}")
a.play()
a.volume(0.5)
a.pitch(1.0)
a.pan(0.5)
a.pause()
a.resume()
a.stop()
println("playing after stop: {a.playing()}")
let st = Scene.Audio.new(22050, 2, 512)
for i in 0..10 { st.push2(0.1, -0.1) }
println("stereo pending {st.pending()}")
for i in 0..3000 { a.push(0.0) }
println("surplus dropped: {a.dropped() > 0} pending capped: {a.pending() <= 1024}")
println("needed is a count: {a.needed() >= 0}")
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
  if ! CULEBRA_CANVAS_HEADLESS=1 "$CULEBRA" "$engine" scene_mirror.cul > "mirror${engine}.txt" 2>&1; then
    echo "${prefix}scene_api_test: mirror program $engine exited non-zero:" >&2
    cat "mirror${engine}.txt" >&2
    fail=1
  fi
done
[ $fail -eq 0 ] || exit 1
if ! diff "mirror--vm.txt" "mirror--jit.txt" > diff.txt; then
  echo "${prefix}scene_api_test: the VM and the JIT disagree on the mirror:" >&2
  cat diff.txt >&2
  exit 1
fi
for want in "beacon in the target: true upright: true" \
            "canvas upright as a sprite: true" \
            "plain plate split: true" \
            "mirrored plate swaps: true"; do
  if ! grep -qxF "$want" "mirror--vm.txt"; then
    echo "${prefix}scene_api_test: mirror line missing: $want" >&2
    cat "mirror--vm.txt" >&2
    fail=1
  fi
done
[ $fail -eq 0 ] || exit 1
echo "OK: a second camera renders into a texture, upright, and uv(-1) mirrors it"

for engine in "--vm" "--jit"; do
  if ! CULEBRA_CANVAS_HEADLESS=1 "$CULEBRA" "$engine" scene_lut.cul > "lut${engine}.txt" 2>&1; then
    echo "${prefix}scene_api_test: LUT program $engine exited non-zero:" >&2
    cat "lut${engine}.txt" >&2
    fail=1
  fi
done
[ $fail -eq 0 ] || exit 1
if ! diff "lut--vm.txt" "lut--jit.txt" > diff.txt; then
  echo "${prefix}scene_api_test: the VM and the JIT disagree on the LUT:" >&2
  cat diff.txt >&2
  exit 1
fi
for want in "identity lut matches: true" "swap lut swaps: true" \
            "knobs change the frame: true" "post off still draws: true"; do
  if ! grep -qxF "$want" "lut--vm.txt"; then
    echo "${prefix}scene_api_test: LUT line missing: $want" >&2
    cat "lut--vm.txt" >&2
    fail=1
  fi
done
[ $fail -eq 0 ] || exit 1
echo "OK: the LUT grades the frame and the post knobs bind"

for engine in "--vm" "--jit"; do
  if ! "$CULEBRA" "$engine" scene_audio.cul > "audio${engine}.txt" 2> "audio${engine}.err"; then
    echo "${prefix}scene_api_test: audio program $engine exited non-zero:" >&2
    cat "audio${engine}.txt" "audio${engine}.err" >&2
    fail=1
  fi
done
[ $fail -eq 0 ] || exit 1
if ! diff "audio--vm.txt" "audio--jit.txt" > diff.txt; then
  echo "${prefix}scene_api_test: the VM and the JIT disagree on audio:" >&2
  cat diff.txt >&2
  exit 1
fi
for want in "latency ok true ready is a bool true" \
            "pending 100: true" \
            "submit hands over: true pending after true" \
            "playing after stop: false" \
            "stereo pending 10" \
            "surplus dropped: true pending capped: true" \
            "needed is a count: true"; do
  if ! grep -qxF "$want" "audio--vm.txt"; then
    echo "${prefix}scene_api_test: audio line missing: $want" >&2
    cat "audio--vm.txt" >&2
    fail=1
  fi
done
[ $fail -eq 0 ] || exit 1
echo "OK: Scene.Audio binds and behaves with or without a device"

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
expect "graph 1 2.0 3.0 true false 0 3"
expect "find/child_at errors: RuntimeError RuntimeError"
expect "removed 0 false"
expect "root removed false"
expect "culling invisible: true"
expect "missing font: RuntimeError"
expect "2d frame drew: true"
expect "clip cut: true"
expect "texture(material): TypeError"
expect "material(texture): TypeError"
expect "texture(dropped): ClosedError"
expect "nested canvas: RuntimeError"
[ $fail -eq 0 ] || exit 1

echo "OK: every Scene method binds and the engines agree ($(wc -l < out--vm.txt | tr -d ' ') lines)"
