# retro-run assets

Generated, not drawn by hand — run `python3 ../tools/gen_assets.py` to rebuild
them. The generator only needs the Python standard library.

These are **placeholders**. Upstream javascript-racer's `sprites.png` is lifted
from OutRun and its music may not be redistributed, so nothing from it is
vendored here. Redraw any of these by hand and the game will not notice, as long
as the sizes below stay exactly as they are.

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
- The three background layer rectangles, and that each layer's horizontal detail
  repeats with a period dividing 1280 — the layers scroll and wrap, so anything
  that doesn't divide shows a seam sliding past.
- That the four backgrounds are **geometry-identical**. The day cycle cross-fades
  them, so a ridge one pixel out would ghost through the blend.

**Free — redraw at will:** every colour and every shape inside those boxes.

The generator enforces the fixed parts rather than trusting them: it draws the
backgrounds once into a buffer of palette *keys* and bakes that same buffer four
times, so a scene-specific shape is not expressible; it asserts each layer wraps;
and it asserts the four baked images share one alpha mask.

`../tools/check_assets.cul` re-checks the shipped files from the outside — it
loads them with `Canvas.Sprite.from_png` and verifies every rectangle is where
`common.js` says, that no sprite fills its whole box, and that the four
backgrounds still share one mask.

## Palette

Warm pastel, plum-ish ink instead of black, dithering instead of intermediate
tones. The base values, the saturation boost and the light/dark narrowing all
live at the top of `gen_assets.py`, which is the single source for both the art
and `retro-run.cul`'s road colours.

`sprites.png` is baked once with the **day** palette and is not recoloured per
scene — only the backgrounds have four variants.
