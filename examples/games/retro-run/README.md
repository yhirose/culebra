# Retro Run

A culebra port of Jake Gordon's
[javascript-racer](https://github.com/jakesgordon/javascript-racer)
(`v4.final.html`, MIT) — the pseudo-3D road, the segment/curve/hill geometry,
the AI traffic, collision, and lap timing all come from the original. What's
different: the art (generated, not lifted — see `assets/README.md`), a
four-scene day cycle that gives each scene a quarter of the track, and the
shape of the hot loops, rewritten for a tree-walking interpreter rather than a
JIT'd browser (see `retro-run.cul`'s header comment for why).

## Run

```sh
culebra --jit examples/games/retro-run/retro-run.cul  # native window, smoothest
culebra examples/games/retro-run/retro-run.cul        # interpreter (also fine)
```

Or serve `site/playground` (`bash playground/build.sh` first, if it isn't
already built) and open it in a browser — pick "Canvas: Retro Run" from the
Examples menu. It runs the same source; the art streams in over `fetch`
instead of the local filesystem, everything else is identical. Open it as
`http://localhost:...`, not `http://[::]`: the latter is treated as
non-secure and breaks WebGPU.

Controls are arrow keys or WASD. A bare number overrides the draw distance
(default 300, matching v4's own default; its tweak UI allows 100..500), and
`--assets <dir>` reads the art from somewhere else:

```sh
culebra --jit examples/games/retro-run/retro-run.cul 150
culebra --jit examples/games/retro-run/retro-run.cul --assets ~/art/racer 150
```

`culebra build` produces a standalone binary. It compiles rather than runs, so
the arguments above go to the binary, not to the build:

```sh
cd examples/games/retro-run
culebra build retro-run.cul -o retro-run
./retro-run --assets assets-racer
```

The binary has no `.cul` to sit beside, so without `--assets` it reads the art
from an `assets/` directory next to the executable — which is where building
in place puts it.

## Assets

`assets/` holds generated placeholder art — see `assets/README.md` for what's
fixed (sprite/background rectangles, which the game's scale and collision
depend on) versus free to redraw.

`--assets` reads from somewhere else instead — a copy of upstream's own art,
say (personal use, not redistribution — see the note in `assets/README.md`).
Point it anywhere; an `assets-*/` directory here is gitignored so a local copy
cannot be committed by accident. The Playground build passes no arguments, so
it always uses the generated set.

A directory needs `sprites.png` and either one `background.png` or a
`background_<scene>.png` per scene — a missing file names itself and the
directory it was looked for in. Upstream ships a single background, so pointing
at its art leaves the day cycle to the road, fog and sky colours while the
picture behind them stays put.

A `music.ogg` or `music.mp3` in the directory (looked for in that order) loops
under the run via `Canvas.music`, replacing the built-in chiptune riff —
upstream's own soundtrack, if you have it. It plays at `vol: 60`, under the
engine but clearly audible; edit the `Canvas.music` call to taste. The
generated set ships no music, so the chiptune is the default.

## Performance

640×480, `drawDistance` 300, 200 cars, `-O3`: the JIT runs at 2.4–4.5 ms a
frame (well past 60fps); the interpreter runs at 17–25 ms a frame (40–60fps),
built by discarding the C++ exceptions that `return`/`break`/`continue` cost
a tree-walking interpreter, culling a screen row's worth of road at a time
instead of a segment at a time, and skipping the `Canvas` preamble wrappers
in the draw loop.
