# retro-run assets

Generated, not drawn by hand — run `culebra --jit ../tools/gen_assets.cul` to
rebuild them (about a minute; a second argument of `backgrounds` or `sprites`
rebuilds one set while iterating on it). The generator needs nothing but
culebra itself: shapes are rasterised through `Canvas`, `Canvas.Sprite` bakes
the palette and `to_png` writes the files.

Nothing here is lifted from anywhere. Upstream javascript-racer's `sprites.png`
is taken from OutRun and its music may not be redistributed, so none of it is
vendored; these are original drawings in the same spirit — a coast road, a red
convertible with two riders, billboards, palms. Redraw any of them by hand and
the game will not notice, as long as the sizes below stay exactly as they are.

| file | size | contents |
| --- | --- | --- |
| `sprites.png` | 1492×1487 | 34 sprites at the atlas rectangles `common.js` names |
| `background_asayake.png` | 1290×1470 | dawn — sky / hills / trees, 1280×480 each |
| `background_hiru.png` | 1290×1470 | day |
| `background_yuugure.png` | 1290×1470 | dusk |
| `background_twilight.png` | 1290×1470 | twilight |

## What is fixed and what is free

**Fixed — changing these changes how the game plays:**

- Every sprite rectangle. `SPRITES.w` is what collision (`Util.overlap`) measures
  and what sets the world scale (`SPRITES.SCALE = 0.3 / PLAYER_STRAIGHT.w`), so a
  sprite that outgrows its box will collide wrongly at a distance the player can
  see but not explain.
- The three background layer rectangles, and that each layer wraps
  horizontally — the layers scroll, so a shape that changes abruptly across the
  seam shows as a line sliding past.
- That the four backgrounds are **geometry-identical**. The day cycle cross-fades
  them, so a ridge one pixel out would ghost through the blend.

**Free — redraw at will:** every colour and every shape inside those boxes.

The generator enforces the fixed parts rather than trusting them. It draws the
backgrounds once into a buffer of palette *keys*, then hands that one buffer to
`Canvas.Sprite` four times with four palettes — so a scene-specific shape is not
expressible, and the four images cannot disagree about geometry or about which
pixels are transparent. It also checks that each layer wraps.

`../tools/check_assets.cul` re-checks the shipped files from the outside — it
loads them with `Canvas.Sprite.from_png` and verifies every rectangle is where
`common.js` says, that no sprite fills its whole box, and that the four
backgrounds still share one mask.

## Layout of a background

The road's vanishing point is half-way down each 480-pixel layer, so
everything worth looking at sits above that line: the sky ramp with its clouds
and the sun, two mountain ridges over a strip of sea and sand, the palms and
scrub of the tree line. Below the line each layer is filled solid, which only
shows when the road dips.

## Palette

Saturated 16-bit-style colours, a dark blue-violet ink for outlines instead of
black, ordered (Bayer) dithering for gradients and shading. Every colour lives
at the top of `gen_assets.cul`: `COMMON` holds what the sprite sheet uses and
does not change with the time of day; each scene table holds the sky ramp,
clouds, sun, ridges, sea and tree line, plus the road-side colours (`GRASS_*`,
`ROAD_*`, `RUMBLE_*`, `FOG`, `SKY0`) that `retro-run.cul`'s `SCENE_COLORS`
copies. The generator prints that table at the end of a run, so the two cannot
drift silently.

`sprites.png` is baked once with the **day** palette and is not recoloured per
scene — only the backgrounds have four variants.
