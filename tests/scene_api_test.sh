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
view.height_fog(0.0001, 0.001, 190, 205, 225)

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
# a size no image holds is refused before raylib counts it in int
fn size_err(f) {
  try {
    f()
    'accepted'
  } catch e {
    "{e.kind}: {e.message}"
  }
}
let big_checker = size_err(fn () {
  view.checker(1 << 16, 4, 0, 0, 0, 255, 255, 255)
})
let big_grain = size_err(fn () {
  view.grain(1 << 16, 90, 100, 80, 40)
})
let big_target = size_err(fn () {
  view.render_target(1 << 16, 1 << 16)
})
let neg_canvas = size_err(fn () {
  view.canvas(-1, 8)
})
println("sizes: {big_checker} | {big_grain} | {big_target} | {neg_canvas}")

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
mesh.color(255, 128, 0).vertex_uv(1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 1.0)
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
# an atlas past what raylib counts in int is refused before a glyph is drawn
let big_font = size_err(fn () {
  view.font(ttf, 100000, "0123456789:.")
})
let many_glyphs = size_err(fn () {
  view.font_bytes(FS.read(ttf), 1024)
})
println("font sizes: {big_font} | {many_glyphs}")
# raylib renders text into an image of its own, sized by the text and `size`
let big_text = try {
  Scene.Image.new(16, 16).text("7", 0, 0, 1000000, 255, 255, 255, 255, digits)
  'accepted'
} catch e {
  "{e.kind} {e.message.starts_with('img.text: ')} {e.message.ends_with(' is too large')}"
}
println("image text too large: {big_text}")
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

# --- culling that changes nothing visible ---
# Before the fullscreen toggle below: leaving fullscreen is asynchronous under
# a compositor, and a shot taken while it lands is at the wrong size.
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
# On a real display dx is wherever the pointer happened to be; only Xvfb pins it.
let mdx = view.mouse_dx()
println("mouse {mx >= 0.0 || mx < 0.0} dx {mdx >= 0.0 || mdx < 0.0} wheel {mwheel} {view.mouse("left")} {view.mouse_pressed("right")} {view.mouse("wheel")}")

# --- the graph as data ---
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
leaf.quat(0.0, 0.7071, 0.0, 0.7071).billboard().billboard(false).cull_radius(2.0).culling(false).culling(true)
leaf.remove()
println("removed {tree.child_count()} {view.has("fruit")}")
view.remove(tree)
println("root removed {view.has("tree")}")

# --- input: nothing is pressed on a machine nobody is touching ---
let k1 = view.key("escape")
let k2 = view.key_pressed("a")
let k3 = view.key_released(" ")
let k4 = view.key("nosuchkey")
let k5 = view.key_pressed("shift")
println("keys {k1} {k2} {k3} {k4} {k5}")
let p1 = view.pad_available()
let p2 = view.pad("a")
let p3 = view.pad_pressed("start", 1)
let p4 = view.pad_axis("lx")
let p5 = view.pad_axis("bogus", 2)
println("pad {p1} {p2} {p3} {p4} {p5} '{view.pad_name()}'")
# a device past the last one reads as nothing, whatever is plugged in
let j1 = view.joy_axis(0, 99)
let j2 = view.joy(0, 99)
let j3 = view.joy_hat(0, 99)
let j4 = view.joy_axes(99) + view.joy_buttons(99) + view.joy_hats(99)
let j5 = view.joy_count() >= 0
println("joy {j1} {j2} {j3} {j4} {j5} '{view.joy_name(99)}' '{view.joy_guid(99)}'")
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

# Per-vertex colour, judged by pixels: one quad pushed three ways. No colour
# at all (white, as before), red on the left edge to blue on the right (unlit),
# and white until color() is called then blue on the right (lit, under a sun).
cat > scene_vcolor.cul <<'EOF'
let view = Scene.View.new(200, 150, "scene vertex color")
view.background(20, 24, 30)
view.sun(0.0, 0.0, -1.0, 1.0, 255, 255, 255)
view.ambient(0.5, 255, 255, 255)
let flat = view.add_material().unlit().double_sided()
let lit = view.add_material().double_sided()
view.camera(0.0, 0.0, 4.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 50.0)
# The left edge is pushed first, then the right (the colour is sticky).
fn quad(node, left, right) {
  left(node)
  node.vertex(-1.5, -1.0, 0.0, 0.0, 0.0, 1.0)
  node.vertex(-1.5, 1.0, 0.0, 0.0, 0.0, 1.0)
  right(node)
  node.vertex(1.5, -1.0, 0.0, 0.0, 0.0, 1.0)
  node.vertex(1.5, 1.0, 0.0, 0.0, 0.0, 1.0)
  node.tri(0, 2, 3).tri(0, 3, 1).build()
}
let plain = view.add_mesh().material(flat)
quad(plain, |n| n, |n| n)
let ramp = view.add_mesh().material(flat)
quad(ramp, |n| n.color(255, 0, 0), |n| n.color(0, 0, 255))
let late = view.add_mesh().material(lit)
quad(late, |n| n, |n| n.color(0, 0, 255))
fn shot(node, path) {
  plain.hide()
  ramp.hide()
  late.hide()
  node.show()
  view.render_3d()
  view.screenshot(path)
  view.present()
}
shot(plain, "vc_plain.png")
shot(ramp, "vc_ramp.png")
shot(late, "vc_late.png")
view.drop()

Canvas.init(200, 150)
fn px(path, x, y) {
  Canvas.clear(Canvas.rgba(0, 0, 0))
  Canvas.Sprite.from_png(FS.read(path)).draw(0, 0)
  Canvas.get_pixel(x, y)
}
fn r_of(v) { v & 255 }
fn g_of(v) { (v >> 8) & 255 }
fn b_of(v) { (v >> 16) & 255 }
let p_l = px("vc_plain.png", 45, 75)
let p_r = px("vc_plain.png", 155, 75)
println("uncoloured stays white: {r_of(p_l) > 200 && r_of(p_l) == g_of(p_l) && g_of(p_l) == b_of(p_l) && p_l == p_r}")
let l = px("vc_ramp.png", 45, 75)
let m = px("vc_ramp.png", 100, 75)
let r = px("vc_ramp.png", 155, 75)
println("corners take their colour: {r_of(l) > 200 && b_of(l) < 60 && b_of(r) > 200 && r_of(r) < 60}")
println("centre is the blend: {r_of(m) > 80 && b_of(m) > 80 && g_of(m) < 40}")
let ll = px("vc_late.png", 45, 75)
let lr = px("vc_late.png", 155, 75)
println("lit, white then blue: {r_of(ll) > 60 && b_of(ll) > 60 && r_of(lr) < 30 && b_of(lr) > 60}")
EOF

# A second camera into a texture, and the one flip model behind render targets
# and canvases: a beacon only camera B sees lands in the top half of the target
# when drawn as a sprite (upright), a canvas painted red-over-blue reads red on
# top as a sprite, uv(-1, …) on a plate mirrors what the plain plate shows, and
# an image reads red-over-blue on a box, a sphere and a cylinder alike.
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
# the same red-over-blue as an image on each primitive, above the plates
let split_img = Scene.Image.new(32, 32).rect(0, 0, 32, 16, 255, 0, 0).rect(0, 16, 32, 16, 0, 0, 255)
let on_shape = view.add_material().texture(view.texture(split_img)).unlit()
view.add_box(1.6, 1.6, 0.2).move(-3.0, 2.6, 0.0).material(on_shape)
view.add_sphere(0.8).move(0.0, 2.6, 0.0).material(on_shape)
view.add_cylinder(0.6, 1.6).move(3.0, 2.6, 0.0).material(on_shape)
view.render_3d()
view.screenshot("mirror_shapes.png")
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
load("mirror_shapes.png")
fn red_over_blue(x) { is_red(Canvas.get_pixel(x, 43)) && is_blue(Canvas.get_pixel(x, 63)) }
println("image upright on a box: {red_over_blue(83)} sphere: {red_over_blue(160)} cylinder: {red_over_blue(237)}")
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
view.post_process(false)
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

# No sun() call: an unlit surface keeps its own colour. The default sun used to
# be stored unnormalised, which blew the reflection's sun disc up to infinity
# and, through mix(.., 0), blacked out a disc of the plane. A flat plane seen
# from behind-and-above sweeps the mirror direction through the sun's.
cat > scene_nosun.cul <<'EOF'
let view = Scene.View.new(160, 120, "scene nosun")
view.background(20, 24, 30)
let magenta = view.add_material().rgb(255, 0, 255).unlit()
view.add_plane(4000.0, 4000.0).material(magenta)
view.camera(0.0, 40.0, -30.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 90.0)
view.render_3d()
view.screenshot("nosun.png")
view.present()
view.drop()
Canvas.init(160, 120)
Canvas.clear(Canvas.rgba(0, 0, 0))
Canvas.Sprite.from_png(FS.read("nosun.png")).draw(0, 0)
mut dark = 0
for y in 0..120 {
  for x in 0..160 {
    let v = Canvas.get_pixel(x, y)
    if (v & 255) < 150 || ((v >> 16) & 255) < 150 { dark += 1 }
  }
}
println("unlit pixels not magenta: {dark}")
EOF

# point_mipmap: one-texel black/white stripes on a ground plane seen to the
# horizon. Near the camera a texel is many pixels wide, so every pixel of the
# bottom row is pure black or white under point_mipmap (trilinear blends the
# edges); at the far row texels are sub-pixel, so point_mipmap averages them
# to grey while plain point still aliases to pure pixels. Post pass off, so
# nothing blurs the rows.
cat > scene_pmip.cul <<'EOF'
let view = Scene.View.new(160, 120, "scene pmip")
view.background(20, 24, 30)
view.sun(0.0, -1.0, -0.3, 1.0, 255, 255, 255)
view.ambient(0.6, 255, 255, 255)
let img = Scene.Image.new(64, 64)
for y in 0..64 {
  for x in 0..64 {
    if x % 2 == 0 { img.pixel(x, y, 0, 0, 0) } else { img.pixel(x, y, 255, 255, 255) }
  }
}
let tex = view.texture(img, false, true)
# the plane's UV spans it once; 4000 m / 64 texels = a 1 m texel
let mat = view.add_material().texture(tex).unlit().uv(62.5, 62.5, 0.0, 0.0)
view.add_plane(4000.0, 4000.0).material(mat)
view.camera(0.0, 3.0, 0.0, 0.0, 0.0, 20.0, 0.0, 1.0, 0.0, 60.0)
view.post_process(false)
Canvas.init(160, 120)
# pure (black or white) pixels in one row of the frame drawn with `mode`
fn pure_in_rows(mode) {
  tex.filter(mode)
  view.render_3d()
  view.screenshot("pmip_{mode}.png")
  view.present()
  Canvas.clear(Canvas.rgba(0, 0, 0))
  Canvas.Sprite.from_png(FS.read("pmip_{mode}.png")).draw(0, 0)
  fn pure(y) {
    mut n = 0
    for x in 0..160 {
      let r = Canvas.get_pixel(x, y) & 255
      if r <= 40 || r >= 215 { n += 1 }
    }
    n
  }
  (pure(119), pure(55))
}
let (pm_near, pm_far) = pure_in_rows("point_mipmap")
let (tri_near, tri_far) = pure_in_rows("trilinear")
let (pt_near, pt_far) = pure_in_rows("point")
println("near stays sharp: {pm_near == 160 && pt_near == 160 && tri_near < 160}")
println("far is averaged: {pm_far == 0 && tri_far == 0 && pt_far > 0}")
tex.filter("no_such_mode")
view.render_3d()
view.present()
println("unknown name keeps the last filter: {pure_in_rows("point_mipmap")[0] == 160}")
EOF

# texture_mips: a red full-size level and green levels below it. The plane is
# seen to the horizon, so the bottom row samples level 0 and the far row a deep
# level. Averaging a red level 0 would stay red all the way out; green there
# means the levels passed in are the ones the GPU read. Then the refusals.
cat > scene_tmips.cul <<'EOF2'
let view = Scene.View.new(160, 120, "scene tmips")
view.background(20, 24, 30)
view.sun(0.0, -1.0, -0.3, 1.0, 255, 255, 255)
view.ambient(0.6, 255, 255, 255)
fn level(n, r, g) { Scene.Image.new(n, n).fill(r, g, 0, 255) }
fn chain() {
  [level(64, 255, 0), level(32, 0, 255), level(16, 0, 255), level(8, 0, 255),
   level(4, 0, 255), level(2, 0, 255), level(1, 0, 255)]
}
let tex = view.texture_mips(chain())
let mat = view.add_material().texture(tex).unlit().uv(62.5, 62.5, 0.0, 0.0)
view.add_plane(4000.0, 4000.0).material(mat)
view.camera(0.0, 3.0, 0.0, 0.0, 0.0, 20.0, 0.0, 1.0, 0.0, 60.0)
view.post_process(false)
Canvas.init(160, 120)
view.render_3d()
view.screenshot("tmips.png")
view.present()
Canvas.clear(Canvas.rgba(0, 0, 0))
Canvas.Sprite.from_png(FS.read("tmips.png")).draw(0, 0)
let near = Canvas.get_pixel(80, 119)
let far = Canvas.get_pixel(80, 48)
println("size {tex.width()}x{tex.height()}")
println("near is level 0: {(near & 255) > 200 && ((near >> 8) & 255) < 50}")
println("far is a given level: {(far & 255) < 50 && ((far >> 8) & 255) > 200}")
fn err(f) {
  try {
    f()
    'accepted'
  } catch e {
    "{e.kind}: {e.message}"
  }
}
println(err(fn () { view.texture_mips([]) }))
println(err(fn () { view.texture_mips(chain()[..3]) }))
println(err(fn () { view.texture_mips([level(64, 0, 0), level(16, 0, 0)]) }))
println(err(fn () { view.texture_mips([level(64, 0, 0), level(32, 0, 0), level(8, 0, 0), level(8, 0, 0),
                               level(4, 0, 0), level(2, 0, 0), level(1, 0, 0)]) }))
println(err(fn () { view.texture_mips(5) }))
println(err(fn () { view.texture_mips([level(1, 0, 0), 5]) }))
let rect = view.texture_mips([Scene.Image.new(4, 2), Scene.Image.new(2, 1), Scene.Image.new(1, 1)], false)
println("rect {rect.width()}x{rect.height()}")
EOF2

# node.load: one quad pushed vertex by vertex and the same quad handed over as
# packed bytes draw the same frame; a bad length or index is refused; and a
# million vertices go up in well under a second (16 nodes of 65535).
cat > scene_mload.cul <<'EOF2'
let view = Scene.View.new(160, 120, "scene mload")
view.background(20, 24, 30)
view.sun(0.0, -1.0, -0.3, 1.0, 255, 255, 255)
view.ambient(0.6, 255, 255, 255)
view.camera(0.0, 0.0, 0.0, 0.0, 0.0, 5.0, 0.0, 1.0, 0.0, 60.0)
view.post_process(false)
Canvas.init(160, 120)
let checks = view.checker(8, 2, 255, 255, 255, 40, 40, 40)
let mat = view.add_material().texture(checks).unlit()
let pos = [-1.0, -1.0, 5.0,  1.0, -1.0, 5.0,  1.0, 1.0, 5.0,  -1.0, 1.0, 5.0]
let nrm = [0.0, 0.0, -1.0,  0.0, 0.0, -1.0,  0.0, 0.0, -1.0,  0.0, 0.0, -1.0]
let uvs = [0.0, 1.0,  1.0, 1.0,  1.0, 0.0,  0.0, 0.0]
let cols = [255, 255, 255, 255,  255, 128, 128, 255,  128, 255, 128, 255,  128, 128, 255, 255]
let idx = [0, 2, 1,  0, 3, 2]
fn shot(path) {
  view.render_3d()
  view.screenshot(path)
  view.present()
  FS.read(path)
}
let empty = shot("mload_e.png")
let pushed = view.add_mesh().material(mat)
for i in 0..4 {
  pushed.color(cols[4 * i], cols[4 * i + 1], cols[4 * i + 2])
  pushed.vertex_uv(pos[3 * i], pos[3 * i + 1], pos[3 * i + 2], 0.0, 0.0, -1.0, uvs[2 * i], uvs[2 * i + 1])
}
pushed.tri(0, 2, 1).tri(0, 3, 2).build()
let a = shot("mload_a.png")
pushed.hide()
let loaded = view.add_mesh().material(mat)
let packed_cols = String.pack("u8", cols)
loaded.load(String.pack("f32", pos), String.pack("u16", idx), String.pack("f32", nrm),
            String.pack("f32", uvs), packed_cols)
let b = shot("mload_b.png")
println("vertices {loaded.vertex_count()}")
println("same frame: {a == b}")
let bare = view.add_mesh().material(view.add_material().rgb(255, 0, 0).unlit())
bare.load(String.pack("f32", pos), String.pack("u16", idx))
loaded.hide()
let c = shot("mload_c.png")
println("drawn: {a != empty && c != empty && c != a}")
fn err(f) {
  try {
    f()
    'accepted'
  } catch e {
    "{e.kind}: {e.message}"
  }
}
let P = String.pack("f32", pos)
let short_p = String.pack("u8", [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11])
let short_i = String.pack("u8", [1, 2, 3, 4, 5])
let I = String.pack("u16", idx)
let n = view.add_mesh()
println(err(fn () { n.load(short_p, I) }))
println(err(fn () { n.load(P, short_i) }))
println(err(fn () { n.load(P, I, short_p) }))
println(err(fn () { n.load(P, I, "", P) }))
println(err(fn () { n.load(P, I, "", "", P) }))
n.load(P, String.pack("u16", [0, 1, 9]))
println("index past the vertices draws nothing: {shot("mload_d.png") == c}")
# a million vertices
mut flat = []
for i in 0..65535 { flat.push((i % 256) * 1.0); flat.push(0.0); flat.push(0.0) }
let big_p = String.pack("f32", flat)
mut seq = []
for i in 0..65535 { seq.push(i) }
let big_i = String.pack("u16", seq)
let t0 = Time.now()
for k in 0..16 { view.add_mesh().load(big_p, big_i) }
let fast = Time.now() - t0 < Time.seconds(1)
println("a million vertices load fast: {fast}")
EOF2

# height_fog: a magenta slab seen through white fog. The green channel is the
# fog's share (magenta has none, white has all). From 3000 m looking straight
# down at the ground the haze column is thin, so the slab stays magenta; from
# the slab's own height 30 km out (a level line, where the height difference
# is 0) the same fog is dense. fog() afterwards takes the fog back, and a bad
# argument leaves it alone.
cat > scene_hfog.cul <<'EOF'
let view = Scene.View.new(160, 120, "scene hfog")
view.background(20, 24, 30)
view.sun(0.0, -1.0, -0.3, 1.0, 255, 255, 255)
view.ambient(0.6, 255, 255, 255)
view.clip_planes(1.0, 60000.0)
view.post_process(false)
let magenta = view.add_material().rgb(255, 0, 255).unlit()
view.add_plane(4000.0, 4000.0).material(magenta)
view.add_box(4000.0, 4000.0, 10.0).move(0.0, 10.0, 30000.0).material(magenta)
Canvas.init(160, 120)
fn green_at_centre(ex, ey, ez, tx, ty, tz) {
  view.camera(ex, ey, ez, tx, ty, tz, 0.0, 1.0, 0.0, 40.0)
  view.render_3d()
  view.screenshot("hfog.png")
  view.present()
  Canvas.clear(Canvas.rgba(0, 0, 0))
  Canvas.Sprite.from_png(FS.read("hfog.png")).draw(0, 0)
  (Canvas.get_pixel(80, 60) >> 8) & 255
}
fn down() { green_at_centre(0.0, 3000.0, 0.0, 0.0, 0.0, 1.0) }
fn level() { green_at_centre(0.0, 10.0, 0.0, 0.0, 10.0, 30000.0) }
let clear_view = (down(), level())
view.height_fog(1.0 / 30000.0, 1.0 / 1200.0, 255, 255, 255)
let foggy = (down(), level())
println("clear without fog: {clear_view[0] < 8 && clear_view[1] < 8}")
println("down stays clear: {foggy[0] < 40} level hazes: {foggy[1] > 100 && foggy[1] < 220}")
view.height_fog(-1.0, 0.0, 0, 0, 0)
view.height_fog(0.001, Math.sqrt(-1.0), 0, 0, 0)
println("bad arguments keep the fog: {level() == foggy[1]}")
view.height_fog(1.0 / 30000.0, 0.0, 255, 255, 255)
println("falloff 0 is uniform: {level() > 100}")
view.fog(1.0e8, 2.0e8, 255, 255, 255)
println("fog() takes over: {level() < 8}")
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
# height rising down the image: the normal leans up (Y follows +V), X untouched
let slope = Scene.Image.new(4, 4).gradient(0, 0, 0, 255, 255, 255).to_normal(1.0)
println("downhill normal leans up {((slope.get(1, 1) >> 16) & 255) < 128} x flat {Math.abs((slope.get(1, 1) >> 24) - 128) <= 1}")
let c = img.copy().flip_h().flip_v()
println("copy flipped {c.get(7, 7)} {c.get(0, 0)}")
let big = Scene.Image.new(2, 2).fill(10, 20, 30).resize(6, 6).crop(1, 1, 3, 3)
println("resize+crop {big.width()}x{big.height()} {big.get(0, 0)}")
let rad = Scene.Image.new(9, 9).gradient_radial(0.0, 255, 255, 255, 0, 0, 0)
println("radial centre brighter {rad.get(4, 4) >> 24 > rad.get(0, 0) >> 24}")
let nz = Scene.Image.new(8, 8).fill(100, 100, 100).noise(7, 2.0, 128).cellular(4, 64).blur(1).tint(255, 200, 200).brightness(10).grayscale().invert().rotate(90)
println("passes ran {nz.width()}x{nz.height()}")
# grayscale is the one whole-image pass raylib implements as a reformat, and
# everything here reads RGBA8: the channels must come back equal with alpha
# intact, and to_normal() (which indexes by 4) must not walk off the buffer.
let gs = Scene.Image.new(8, 8).fill(20, 90, 200).grayscale()
let gp = gs.get(1, 1)
println("grayscale stays rgba8 {(gp >> 24) == ((gp >> 16) & 255)} {((gp >> 16) & 255) == ((gp >> 8) & 255)} alpha {gp & 255}")
println("grayscale then normal {gs.to_normal(1.0).get(1, 1)}")
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
# A size raylib cannot count in int is refused before raylib sees it: past
# that, it allocated a few bytes and filled all w*h pixels into them.
fn refused(f) {
  try {
    f()
    'accepted'
  } catch e {
    "{e.kind}: {e.message}"
  }
}
let past_raylib = refused(fn () {
  Scene.Image.new(24000, 24000)
})
println("image 24000x24000: {past_raylib}")
let past_int = refused(fn () {
  Scene.Image.new((1 << 32) + 4, 3)
})
println("image past int: {past_int}")
let negative = refused(fn () {
  Scene.Image.new(-1, 3)
})
println("image negative: {negative}")
let kept = Scene.Image.new(4, 4).fill(1, 2, 3)
let resized = refused(fn () {
  kept.resize(40000, 40000)
})
println("resize 40000x40000: {resized}")
println("refused resize keeps {kept.width()}x{kept.height()} {kept.get(0, 0)}")
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
            "downhill normal leans up true x flat true" \
            "copy flipped 4278190335 16711935" \
            "resize+crop 3.0x3.0 169090815" \
            "radial centre brighter true" \
            "passes ran 8.0x8.0" \
            "grayscale stays rgba8 true true alpha 255" \
            "grayscale then normal 2155937791" \
            "shapes 151587327 16711935" \
            "blit 65535 255" \
            "text without a window: 0" \
            "bad png: RuntimeError" \
            "image 24000x24000: ValueError: Scene.Image.new: 24000x24000 is too large" \
            "image past int: ValueError: Scene.Image.new: 4294967300x3 is too large" \
            "image negative: ValueError: Scene.Image.new: width and height must not be negative, got -1x3" \
            "resize 40000x40000: ValueError: img.resize: 40000x40000 is too large" \
            "refused resize keeps 4.0x4.0 16909311"; do
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
  if ! CULEBRA_CANVAS_HEADLESS=1 "$CULEBRA" "$engine" scene_alpha.cul > "alpha${engine}.txt" 2> "alpha${engine}.err"; then
    echo "${prefix}scene_api_test: transparency program $engine exited non-zero:" >&2
    cat "alpha${engine}.txt" "alpha${engine}.err" >&2
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
  if ! CULEBRA_CANVAS_HEADLESS=1 "$CULEBRA" "$engine" scene_mirror.cul > "mirror${engine}.txt" 2> "mirror${engine}.err"; then
    echo "${prefix}scene_api_test: mirror program $engine exited non-zero:" >&2
    cat "mirror${engine}.txt" "mirror${engine}.err" >&2
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
            "mirrored plate swaps: true" \
            "image upright on a box: true sphere: true cylinder: true"; do
  if ! grep -qxF "$want" "mirror--vm.txt"; then
    echo "${prefix}scene_api_test: mirror line missing: $want" >&2
    cat "mirror--vm.txt" >&2
    fail=1
  fi
done
[ $fail -eq 0 ] || exit 1
echo "OK: a second camera renders into a texture, upright, and uv(-1) mirrors it"

for engine in "--vm" "--jit"; do
  if ! CULEBRA_CANVAS_HEADLESS=1 "$CULEBRA" "$engine" scene_lut.cul > "lut${engine}.txt" 2> "lut${engine}.err"; then
    echo "${prefix}scene_api_test: LUT program $engine exited non-zero:" >&2
    cat "lut${engine}.txt" "lut${engine}.err" >&2
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

# A supersampled render target: a plate edge inside a pixel comes out grey at 2x
# and 4x (white or black at 1x), and a mipmapped texture keeps the output's
# level at any factor (bilinear picks one level; trilinear would blend two).
# One plate and camera for each, 100 units apart.
cat > scene_tss.cul <<'EOS'
let view = Scene.View.new(160, 120, "scene tss")
view.background(0, 0, 0)
view.post_process(false)
fn err(f) {
  try {
    f()
    'accepted'
  } catch e {
    "{e.kind}: {e.message}"
  }
}
let white = view.add_material().rgb(255, 255, 255).unlit()
view.add_box(20.0, 40.0, 0.1).move(-9.375, 0.0, 0.0).material(white)
mut tex_levels = []
mut side = 64
for c in [[255, 0, 0], [0, 255, 0], [0, 0, 255], [255, 255, 0], [0, 255, 255], [255, 0, 255], [255, 255, 255]] {
  tex_levels.push(Scene.Image.new(side, side).rect(0, 0, side, side, c[0], c[1], c[2]))
  side = Math.max(1, side / 2)
}
let mips = view.add_material().texture(view.texture_mips(tex_levels).filter("bilinear")).unlit()
view.add_box(20.0, 20.0, 0.01).move(100.0, 0.0, 0.0).material(mips)
let edges = [view.render_target(16, 16), view.render_target(16, 16, supersample: 2), view.render_target(16, 16, supersample: 4)]
let levels = [view.render_target(16, 16), view.render_target(16, 16, supersample: 2), view.render_target(16, 16, supersample: 4)]
for t in edges { view.render_to(t, 0.0, 0.0, 10.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 90.0) }
for t in levels { view.render_to(t, 100.0, 0.0, 10.0, 100.0, 0.0, 0.0, 0.0, 1.0, 0.0, 90.0) }
view.begin_2d()
for i in 0..3 {
  view.sprite(edges[i], 20.0 * i, 0.0, 16.0, 16.0)
  view.sprite(levels[i], 20.0 * i, 20.0, 16.0, 16.0)
}
view.screenshot("tss.png")
view.present()
let default_target = view.render_target(8, 4)
let bad_zero = err(fn () { view.render_target(16, 16, supersample: 0) })
let bad_five = err(fn () { view.render_target(16, 16, supersample: 5) })
let bad_size = err(fn () { view.render_target(1 << 16, 1 << 16, supersample: 2) })
view.drop()

Canvas.init(160, 120)
Canvas.Sprite.from_png(FS.read("tss.png")).draw(0, 0)
fn grey_at(i, x) { Canvas.get_pixel(20 * i + x, 8) & 255 }
fn level_at(i) {
  let v = Canvas.get_pixel(20 * i + 8, 28)
  [v & 255, (v >> 8) & 255, (v >> 16) & 255]
}
println("a 4x target is {edges[2].width()}x{edges[2].height()} to its readers: {edges[2].width() == 16.0 && edges[2].height() == 16.0}")
println("1x is the default: {default_target.width() == 8.0 && default_target.height() == 4.0}")
println("1x edge is white or black: {grey_at(0, 8) == 255 && grey_at(0, 9) == 0}")
println("2x and 4x average the edge pixel: {grey_at(1, 8) > 100 && grey_at(1, 8) < 155 && grey_at(2, 8) > 100 && grey_at(2, 8) < 155}")
println("solid inside, empty outside: {grey_at(2, 7) == 255 && grey_at(2, 10) == 0}")
println("level 2 at 1x, 2x and 4x: {level_at(0) == [0, 0, 255] && level_at(1) == [0, 0, 255] && level_at(2) == [0, 0, 255]}")
println(bad_zero)
println(bad_five)
println(bad_size)
EOS

# A queued 2D draw names its texture or font atlas by GPU id until the frame is
# presented, so a handle the script lets go of first must stay alive: the next
# canvases take the freed ids, and the sprite and the text would show them.
{
  echo "let ttf = \"$TTF\""
  cat <<'EOS'
let view = Scene.View.new(160, 120, "scene life")
view.background(0, 0, 0)
fn draw_sprite() {
  let c = view.canvas(16, 16)
  view.rect(0.0, 0.0, 16.0, 16.0, 255, 0, 0)
  view.canvas_end()
  view.sprite(c, 10.0, 10.0, 32.0, 32.0)
}
fn draw_text() {
  let f = view.font_bytes(FS.read(ttf), 40, "M")
  view.text("M", 80.0, 10.0, 40, 255, 255, 255, font: f)
}
view.begin_2d()
draw_sprite()
draw_text()
let t1 = view.canvas(16, 16)
view.rect(0.0, 0.0, 16.0, 16.0, 0, 0, 255)
view.canvas_end()
let t2 = view.canvas(16, 16)
view.rect(0.0, 0.0, 16.0, 16.0, 0, 0, 255)
view.canvas_end()
view.screenshot("life.png")
view.present()
view.drop()

Canvas.init(160, 120)
Canvas.Sprite.from_png(FS.read("life.png")).draw(0, 0)
let v = Canvas.get_pixel(26, 26)
println("a sprite outlives its handle: {(v & 255) > 200 && ((v >> 8) & 255) < 60 && ((v >> 16) & 255) < 60}")
mut white = 0
for y in 10..50 {
  for x in 80..120 {
    let p = Canvas.get_pixel(x, y)
    if (p & 255) > 200 && ((p >> 8) & 255) > 200 && ((p >> 16) & 255) > 200 { white += 1 }
  }
}
println("text outlives its font: {white > 100}")
EOS
} > scene_life.cul

# Run one program on both engines: it must exit 0 and the engines must print the
# same lines, each of which must include every expected line given after the name.
run_pair() {
  local name="$1"; shift
  for engine in "--vm" "--jit"; do
    if ! CULEBRA_CANVAS_HEADLESS=1 "$CULEBRA" "$engine" "scene_$name.cul" > "$name${engine}.txt" 2> "$name${engine}.err"; then
      echo "${prefix}scene_api_test: $name program $engine exited non-zero:" >&2
      cat "$name${engine}.txt" "$name${engine}.err" >&2
      exit 1
    fi
  done
  if ! diff -q "$name--vm.txt" "$name--jit.txt" > /dev/null; then
    echo "${prefix}scene_api_test: the VM and the JIT disagree on $name:" >&2
    cat "$name--vm.txt" "$name--jit.txt" >&2
    exit 1
  fi
  for want in "$@"; do
    if ! grep -qxF "$want" "$name--vm.txt"; then
      echo "${prefix}scene_api_test: $name line missing: $want" >&2
      cat "$name--vm.txt" >&2
      exit 1
    fi
  done
}

run_pair nosun "unlit pixels not magenta: 0"
echo "OK: with no sun() call an unlit surface keeps its colour"
run_pair pmip "near stays sharp: true" "far is averaged: true" \
  "unknown name keeps the last filter: true"
echo "OK: point_mipmap keeps near texels sharp and averages far ones"
run_pair tmips "size 64.0x64.0" "near is level 0: true" "far is a given level: true" \
  "ValueError: view.texture_mips: levels is empty" \
  "ValueError: view.texture_mips: a 64x64 texture has 7 levels down to 1x1, got 3" \
  "ValueError: view.texture_mips: a 64x64 texture has 7 levels down to 1x1, got 2" \
  "ValueError: view.texture_mips: levels[2] is 8x8, expected 16x16" \
  "TypeError: view.texture_mips: levels must be an Array of Scene.Image" \
  "TypeError: view.texture_mips: levels[1] is Long, not a Scene.Image" "rect 4.0x2.0"
echo "OK: texture_mips samples the levels it is given"
run_pair mload "vertices 4" "same frame: true" "drawn: true" "index past the vertices draws nothing: true" \
  "ValueError: node.load: positions is 11 bytes, expected 12 per vertex (3 x f32)" \
  "ValueError: node.load: indices is 5 bytes, expected 6 per triangle (3 x u16)" \
  "ValueError: node.load: normals is 11 bytes, expected 12 per vertex (3 x f32)" \
  "ValueError: node.load: uvs is 48 bytes, expected 8 per vertex (2 x f32)" \
  "ValueError: node.load: colours is 48 bytes, expected 4 per vertex (rgba u8)" \
  "a million vertices load fast: true"
echo "OK: node.load draws what vertex_uv pushes and refuses bad bytes"
run_pair hfog "clear without fog: true" "down stays clear: true level hazes: true" \
  "bad arguments keep the fog: true" "falloff 0 is uniform: true" "fog() takes over: true"
echo "OK: height_fog clears a view from above and hazes a far level one"
run_pair vcolor "uncoloured stays white: true" "corners take their colour: true" \
  "centre is the blend: true" "lit, white then blue: true"
echo "OK: vertex colours blend across a face, and an uncoloured mesh stays white"
run_pair tss "a 4x target is 16.0x16.0 to its readers: true" "1x is the default: true" \
  "1x edge is white or black: true" "2x and 4x average the edge pixel: true" \
  "solid inside, empty outside: true" "level 2 at 1x, 2x and 4x: true" \
  "ValueError: view.render_target: supersample must be 1 to 4, got 0" \
  "ValueError: view.render_target: supersample must be 1 to 4, got 5" \
  "ValueError: view.render_target: 65536x65536 is too large"
echo "OK: a supersampled render target averages its edges and keeps the output's mip levels"
run_pair life "a sprite outlives its handle: true" "text outlives its font: true"
echo "OK: a handle dropped before present() still draws what it was given"

for engine in "--vm" "--jit"; do
  if ! CULEBRA_CANVAS_HEADLESS=1 "$CULEBRA" "$engine" scene_api.cul "$TTF" > "out${engine}.txt" 2> "out${engine}.err"; then
    echo "${prefix}scene_api_test: $engine exited non-zero:" >&2
    cat "out${engine}.txt" "out${engine}.err" >&2
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
expect "sizes: ValueError: view.checker: 65536x65536 is too large | ValueError: view.grain: 65536x65536 is too large | ValueError: view.render_target: 65536x65536 is too large | ValueError: view.canvas: width and height must not be negative, got -1x8"
expect "ball 3.0 0.5 0.0"
expect "keys false false false false false"
expect "joy 0.0 false 0 0 true '' ''"
expect "closing false -> true"
expect "screenshot true"
expect "window 320.0x240.0 fps>=0 true time>0 true resized true clipboard 'scene'"
expect "fullscreen-was true"
expect "mouse true dx true wheel 0.0 false false false"
expect "resized frame drew: true"
expect "font 24.0 glyphs 12 bytes-form 24.0 12"
expect "font sizes: ValueError: view.font: 12 glyphs at size 100000 is too large | ValueError: view.font_bytes: 95 glyphs at size 1024 is too large"
expect "text_width grows true default true height true"
expect "image text too large: ValueError true true"
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
