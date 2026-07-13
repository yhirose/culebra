# `Graphics` — renderer-independent graphics facade (design note)

A contract extracted from the real SUZUKA (`~/Projects/racing`, Swift/SceneKit+SpriteKit,
16k lines) rendering surface, letting the backend (raylib / Filament / …) be swapped while
the culebra-side interface stays fixed.

> Revision history: 1st edition = naive extraction from SceneKit. 2nd edition (this one) =
> reflects the design-review meeting's points (retained as the base / material PBR-intent /
> drop / fixed coordinate system / capability / dynamic mesh). Qt, which was weighed and not
> adopted during backend selection, is covered in [`record.md`](record.md).
>
> The Japanese original is [`graphics_design.ja.md`](graphics_design.ja.md); the two must be
> kept in sync.

> Status (2026-06-28): this facade is **implemented and shipped** (master `1712c59`,
> `CULEBRA_ENABLE_GRAPHICS` opt-in-integrates raylib into culebra core, AOT usage-gated,
> single binary via `culebra build suzuka.cul`). This document is a **record of the design
> rationale**; the concrete signatures below are a pre-implementation sketch. The shipped API
> differs: because `wrap.h` cannot take kwargs defaults / tuple arguments, it settled on
> **fluent setters + flat scalars + colors 0..255** (e.g. not `make_material(color, metallic:, …)`
> but `view.material_tex_pbr(tex, r,g,b, metallic, roughness)` /
> `add_box(w,h,d).material(m).move(x,y,z)` / `view.camera(ex,ey,ez, tx,ty,tz, …)`). The real
> API is authoritative in `examples/graphics/suzuka.cul`. The switching mechanism also changed
> from "a separate binary per backend via a `culebra wrap` extension" to an **opt-in build into
> core**. The design rationale (renderer independence / retained base / PBR-intent / fixed
> coordinate system / the core⇔optional line / backend scope) still holds. Full reconciliation
> of the signature tables with the shipped API is left to the graphics implementation session.

## Requirements

1. Handle **both 3D and 2D**.
2. Make it possible to **switch to a different library with as little change to the culebra-side
   interface as possible**.

## Scope of target backends (important)

Swap compatibility has two layers:
1. **Drawing-API compatibility** (add_box, set_camera, text…) — absorbable by the facade.
2. **Frame-driving-model compatibility** (who owns the main loop) — not fully absorbable by the
   facade.

→ **For now the scope is limited to "the game owns the loop" backends**: **raylib / Filament(+GLFW) /
bgfx / sokol**. All follow the model where the game calls drawing each frame in
`while !view.should_close() { ... }`, compatible with both 1 and 2 above.


## Core design decisions

- **For both 3D and 2D, "retained is the base, immediate is sugar on top of it".**
  - Initially it was split asymmetrically as "3D = retained / 2D = immediate", but hard-coding
    immediate 2D drags down retained backends, and the HUD font re-draw cost recurs every frame.
    Unify the base on retained, and make the immediate helpers thin sugar over retained nodes.
  - Immediate-mode raylib keeps a node list inside the binding and replays it in `render()` (a
    thin adapter).
  - SUZUKA's HUD (SpriteKit) is originally retained too, so a retained base is natural.
- **Switching = the same `Graphics.*` namespace is implemented by each backend's `culebra wrap`
  binding.** The game depends only on `Graphics`, and switching is just changing which binary you
  build (`culebra-ray` / `culebra-filament`). Unused backends are not linked via the usage-gate
  (`aot_uses_any_name`, master `860c466`) = zero cost.
- **Degree of build-out**: a "swappable abstraction" cannot be written correctly from a single
  implementation. **We put the contract (the skin) on now, but build the implementation (below) the
  straightforward way with a single raylib layer.** The holes in the contract are only validated
  **once a second one (Filament) is loaded on top**. Do not draw speculative abstraction boundaries
  out of perfectionism (YAGNI).
- Materials are **PBR-intent** (write the intent, not the implementation model name; see below).
- Colors are **RGBA (r,g,b,a; 0.0..1.0)**. Alpha is mandatory.
- Input has **two families: held (`key_down`) and edge (`key_pressed`)**.
- **Fix a single coordinate system in the contract**: right-handed, Y-up, forward -Z, angles in
  radians, rotation ZYX Euler. Each binding converts internally (without fixing it, "swap the
  backend and you get a mirror-image world" accidents happen).

## The extracted contract

The entry point is a single object `Graphics.View` (holds window + 3D scene + camera + 2D + input +
frame; name tentative). 3D/2D objects return `Graphics.Node` / `Graphics.Node2D` handles.

### 3D (retained mode)

```
# Primitives (sugar; the real one is add_mesh)
add_box(w, h, d) -> Node                  # a sharp-edged box. Chamfer via add_mesh
add_sphere(r, segments) -> Node
add_cylinder(r, h, radial_segments) -> Node
add_plane(w, h) -> Node
# Custom triangle mesh (the real target for track surface / car-body loft)
add_mesh(vertices, indices, normals?, uvs?) -> Node    # normals/UV are optional
node.update_mesh(vertices, indices)       # dynamic mesh (tire smoke / debug lines / pools)

# Material (PBR-intent — intent, not the implementation model)
make_material(base_color,
              metallic? = 0.0, roughness? = 0.8,
              emissive? = none, unlit? = false,
              texture? = none, double_sided? = false,
              writes_depth? = true, uv_scale? = 1.0) -> Material
node.set_material(mat)
material.set_base_color(c) / set_texture(tex)    # runtime swap (signal lights)

# Node / transform / lifetime
make_node() -> Node                       # empty container
node.set_position(x, y, z)
node.set_euler(x, y, z)                    # radians, ZYX
node.set_rotation(ax, ay, az, angle)      # axis-angle (suspension arm)
node.set_scale(s) / set_scale3(x, y, z)
node.add_child(node)
node.set_name(s) / view.find(name) -> Node
node.set_casts_shadow(bool)
node.set_hidden(bool)
node.drop()                               # deterministic drop frees GPU resources (retire / pit)

# Camera (rig math is culebra-side = renderer-independent. Pass only the final pose)
set_camera(eye_x,eye_y,eye_z, tgt_x,tgt_y,tgt_z, up_x,up_y,up_z, fov)

# Environment
set_background(texture)
set_fog(start, end, color, density_exp)
add_ambient_light(intensity, color)
add_directional_light(dir_x,dir_y,dir_z, intensity, color, casts_shadow) -> Node
```

### 2D overlay (retained base + immediate sugar, screen coordinates)

```
# Retained nodes (the base) — update position/text/visibility, do not rebuild every frame
add_text(s, x, y, size, color, align) -> Node2D   # align: left|center|right
add_rect(x, y, w, h, color, corner_radius?) -> Node2D
add_sprite(texture, x, y, w, h) -> Node2D
node2d.set_position(x, y) / set_text(s) / set_color(c) / set_alpha(a) / set_hidden(b) / drop()

# Immediate sugar (a simple version called each frame; a thin wrapper over retained nodes)
text(s, x, y, size, color, align)
rect(x, y, w, h, color, corner_radius?) / rect_stroke(...)
circle(x, y, r, color) / circle_stroke(x, y, r, color, line_width)
line(x0, y0, x1, y1, color, width) / polyline(points, color, width)
polygon(points, color)                    # warning-flag triangle
sprite(texture, x, y, w, h, alpha)
push_scale(s) / pop_scale()               # UI scale for the whole HUD
```

### Textures (CPU-generated)

All of SUZUKA's textures are drawn in code with Core Graphics (no external images).

```
texture_from_pixels(rgba_bytes, w, h) -> Texture
```

### Frame / input / lifecycle / capability

```
should_close() -> Bool
begin_frame(); render_3d(); ... 2D draw calls ...; end_frame()
dt() -> Float
key_down(key) -> Bool       # held
key_pressed(key) -> Bool    # the instant of the press (edge)
screenshot(path)
width() -> Long / height() -> Long
capabilities() -> { d2: Bool, pbr: Bool, shadow: Bool, bloom: Bool, ... }   # see below
drop()                      # deterministic teardown in ~View
```

## The line between core and optional capability

To avoid "the narrowest backend is the ceiling", **core is limited to the common denominator**, and
high-quality effects are separated into **optional capability** (a no-op on backends that don't
support them). **Unsupported is not a silent no-op**: it is queryable via `capabilities()`, with a
one-time warning at startup ("this backend does not support 2D", etc.). Breaking silently is the
worst outcome.

| Category | Contents |
|---|---|
| **core** (required of every backend) | primitives / `add_mesh` / `update_mesh` / material (PBR-intent) / node (transform, lifetime) / camera / 2D (retained + immediate) / fog / ambient+directional light / texture |
| **optional capability** | bloom, SSAO, DoF, exposure/saturation/contrast, shadow details (shadowMapSize etc.), per-face materials (box 6 faces / cylinder 3 slots) |

Optional ones are exposed via `view.set_bloom(x)` etc.; if unsupported they no-op (distinguishable
via `capabilities()`).

## Not in the contract (pushed to the caller or handled at port time)

- **subdivisionLevel** (smoothing the car-body loft) = an asset problem. First avoid it by raising the
  loft's ring density, later upgrade to a Catmull-Clark implementation in culebra. **Either way the
  `add_mesh` contract is unchanged.**
- **Chamfered box** → `add_box` stays sharp-edged. If you want it rounded, use `add_mesh`.
- **The Blinn-Phong lighting-model name** → not baked into the contract. Translate once into
  PBR-intent (metal = high metallic / low roughness).
- **Per-face materials** → core is a single `set_material`. Multiple slots are optional; split nodes
  only where needed.

Principle: **the four primitives are sugar, `add_mesh` is the real one, optional may no-op**.

## Backend support table (in-scope only)

| Feature | raylib | Filament | bgfx / sokol |
|---|---|---|---|
| Game-driven loop | ✓ | ✓ (+GLFW) | ✓ |
| 3D core (mesh/primitive/node/camera) | ✓ (immediate→retained adapter) | ✓ (native retained) | ✓ (to implement) |
| 2D overlay | ✓ (**currently the only fully working one**) | ⬜ (unresolved blocker, needs a 2D View) | to implement |
| High quality (PBR/IBL/shadows/post-fx) | △ (reachable with own shaders) | ✓ (SceneKit-grade) | △ |
| CPU textures | ✓ | ✓ | ✓ |

→ The contract is common within scope. The difference is only "implementation maturity", and
Filament's 2D blocker is localized to the Filament backend's implementation problem, with the
interface unchanged (an honest abstraction). `capabilities()` lets the caller know the support status.

## The chunks to port to culebra renderer-independently (do not depend on Graphics)

Most of SUZUKA's 16k lines are unrelated to rendering. Once the Graphics contract is settled, they can
be ported in parallel:

- **CameraRig** (233 lines) = pure math for smoothing/presets/dolly/orbit. It only computes the final
  eye/target/fov and passes them to `set_camera`.
- **HUD layout/update logic** (626 lines) = rewrite to 2D retained nodes + per-frame updates.
- Physics / AI / track data / race progression / qualifying = completely renderer-independent.

## Sense of scale (reference for performance design)

- A car ≈ 62 nodes/car (dominated by 24 suspension cylinders) → ≈ 2000 nodes for 32 cars. A candidate
  for instancing/simplification.
- A track ≈ 300–400 nodes (mostly large static meshes fixed at the origin. Built once; the hot path is
  updating the cars' transforms).
- Measured raylib (existing PoC): 27 cars at 0.234 ms/frame (1.4% of the budget). Game-logic is not the
  bottleneck.

## Details of the switching mechanism

- Each backend = a `culebra wrap` extension. The binding registers **the same `Graphics` namespace and
  the same method names**.
- The game's `Graphics.View.new(...)` resolves to whichever one is built into the running binary.
- Normalization (immediate↔retained, naming, coordinates/colors/keycodes) lives on the **C++ binding
  side**, and the game side stays unchanged.

## Next steps

1. Write this contract as `wrap.h` declarations (first the raylib binding in `Graphics` shape; already
   fully working, so the shortest proof). — **No perfectionism. Build straightforwardly with a single
   raylib layer = the warm-up for loading a second one.**
2. Confirm that the `racer3d` equivalent runs through the contract (interp/JIT symmetric).
3. Port CameraRig / a simple HUD to culebra and prove "a game can be written with the contract alone".
4. **Load a second one (Filament 3D) and validate the contract's holes** → re-revise if needed. 2D
   after the blocker is resolved.
5. Fix the translation policy for SceneKit-specific things (subdivision / chamfer) at port time.

## References

- Port source: `~/Projects/racing/Sources/SuzukaGP/` (CarBuilder/TrackBuilder/CameraRig/HUD/GameView/Palette)
- Existing facades: `raylib-poc:examples/raylib/raylib_binding.cpp` (`Ray.Window`),
  `filament-lib:examples/filament/filament_binding.cpp` (`Filament.Viewer`)
- usage-gate: master `860c466` (`aot_uses_any_name` in `include/runtime/aot_scan.h`)
