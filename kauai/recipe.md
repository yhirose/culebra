# Recipes

A recipe is an instrument written as data: a JSON file that wires
`Audio.Graph`'s parts together. The engine
([`engine/recipe.cul`](engine/recipe.cul)) checks it whole when it is loaded,
then builds the graph for each note and renders it. No code is loaded, so a
recipe plays the same under `culebra play` and in the `kauai` binary.

The recipes Kauai comes with are in [`recipes/`](recipes). A file's name is the
instrument's, without its underscores: `mellow_fm_brass.json` is
`external MellowFmBrass`.

An instrument's name is plain English in its usual order: how it sounds, how
it is made, then what it is.

- What it is: a common instrument's name, as General MIDI's programs name
  them (`ElectricPiano`, `Strings`, `Brass`, `Lead`, `Bass`, `Guitar`).
- How it is made: `Fm` for frequency modulation, `Analog` for oscillators
  through a filter. An instrument made from recordings has none: it is the
  instrument itself (`SopranoSax`, `AltoSax`).
- How it sounds: for a preset, the end of its knob, `Mellow` or `Bright`
  for `bright` and `Dry` for `chorus` at 0; for a recipe of its own that is another tone of an
  instrument Kauai has, a word for how it differs, `Clear` for a clean, light
  one (`ClearAnalogLead` beside `AnalogLead`).

No name holds a product's, a maker's, a player's or a song's name.

| Recipe | A song names it | What it is | Knobs: default (range) |
|---|---|---|---|
| `analog_lead` | `external AnalogLead` | an analog lead, two saws a hair apart over a triangle through a low-pass that opens at the attack and follows the note, with a chorus | `bright` 0.5 (0 to 1): the low-pass opened wider, at about the same loudness; `chorus` 1 (0 to 1): a copy a little later, its delay swaying, and the saws drifting slowly in pitch, at the same loudness |
| `dry_analog_lead` | `external DryAnalogLead` | `analog_lead`, dry | `chorus` 0 (0 to 1) |
| `clear_analog_lead` | `external ClearAnalogLead` | a lighter, clearer lead, sounding an octave above the note written: two saws a hair apart over a triangle, with a sine an octave up for its shine, through a low-pass that opens at the attack and follows the note, with a chorus | `bright` 1 (0 to 1): the low-pass opened wider and more of the octave; `chorus` 1 (0 to 1): as `analog_lead`'s |
| `dry_clear_analog_lead` | `external DryClearAnalogLead` | `clear_analog_lead`, dry | `chorus` 0 (0 to 1) |
| `fm_brass` | `external FmBrass` | a synth brass, two FM pairs a hair apart and a third with its modulator an octave up | `bright` 0.5 (0 to 1): deeper modulation and more feedback, at the same loudness |
| `mellow_fm_brass` | `external MellowFmBrass` | `fm_brass`, mellow | `bright` 0 (0 to 1) |
| `bright_fm_brass` | `external BrightFmBrass` | `fm_brass`, bright | `bright` 1 (0 to 1) |
| `fm_strings` | `external FmStrings` | a synth string section, three FM voices a hair apart and an octave over them | `bright` 0.5 (0 to 1): deeper modulation and the octave louder, at about the same loudness |
| `mellow_fm_strings` | `external MellowFmStrings` | `fm_strings`, mellow | `bright` 0 (0 to 1) |
| `bright_fm_strings` | `external BrightFmStrings` | `fm_strings`, bright | `bright` 1 (0 to 1) |
| `fm_electric_piano` | `external FmElectricPiano` | the electric piano of the Kauai examples, as two FM pairs, its tone decaying faster the higher the note | `bright` 1 (0 to 2): the depth of its FM, most at the attack and settling as the note goes on |

A preset is its recipe with the knob's default set, so its knob still turns:
`external MellowFmBrass bright=0.2` is `fm_brass` at 0.2.

```
band Pads {
  voice Lead  external FmBrass    plays melody          vol 60  bright=0.3
  voice Pad   external FmStrings  plays chords from C3  vol 40
}
```

`culebra play --sounds kauai/recipes song.kau` plays a song with them, and the
`kauai` binary has them built in.

## A recipe

```json
{
  "recipe": 1,
  "knobs": {"bright": {"default": 0.5, "min": 0, "max": 1}},
  "parts": {
    "mod":   {"part": "wave", "feedback": "0.5 + 0.3 * bright"},
    "depth": {"part": "gain", "input": "mod",
              "gain": {"on": [[0, 0.5], [0.06, 1], [0.18, "3 + 1.5 * bright"], [0.45, 2.2]],
                       "off": [[0.12, 0.6]]}},
    "car":   {"part": "wave", "modulators": ["depth"]},
    "amp":   {"part": "gain", "input": "car",
              "gain": {"on": [[0, 0], [0.07, 1], [0.2, 0.85]],
                       "off": [[0.12, 0, -4]]}}
  },
  "out": "amp"
}
```

| Key | What it is |
|---|---|
| `recipe` | the format it is written in: `1` |
| `knobs` | what a song may turn, by name: each `{default, min, max}` |
| `parts` | the parts, by name |
| `out` | the part that is heard |

Every part is reached from `out`, and a part takes in only parts made before
it: one that `out` does not reach, a name no part has, or a part that takes
itself in through others is an error.

## Parts

Each part names what it is in `part`. The other fields are `Audio.Graph`'s
arguments, under the same names.

| `part` | Takes in | Values that may move | Made with once |
|---|---|---|---|
| `wave` | `modulators` (a list) | `freq` (`f`), `feedback` (0) | `kind`: `sine`, `saw`, `square` or `triangle` (`sine`); `phase` (0, radians) |
| `noise` | | | `seed` (1) |
| `filter` | `input` | `cutoff` (1000) | `kind`: `low`, `high` or `band` (`low`); `q` (0.707) |
| `gain` | `input` | `gain` (1) | |
| `mix` | `inputs` (a list) | `gain` (1) | |
| `pluck` | `input` | `freq` (`f`), `ring` (2) | |
| `delay` | `input`, `modulators` (a list) | `time` (0, seconds) | |

A field that takes in parts holds their names. A wave's `modulators` move its
phase and a delay's move its time. A wave's `freq` and a pluck's
are the note's own (`f`) unless the recipe says otherwise.

## Values

A value is a number, an expression, or an envelope.

**An expression** is a string, worked out for each note. It may name the
note's `pitch` (MIDI), `f` (its Hz), `dur` (how long it is held, s) and
`level` (1 at mf), and the recipe's knobs. It has `+ - * /`, `^` for a power,
parentheses, and the functions `exp`, `log`, `sqrt`, `abs`, `min`, `max`,
`clamp(x, lo, hi)` and `mix(a, b, t)` (from `a` at `t` 0 to `b` at 1). A
decay that is faster the higher the note is `"exp(-1.1 * sqrt(f / 261.63))"`.

**An envelope** is `{on, off}`, each a list of points `[time, value]` or
`[time, value, curve]`, whose times and values are numbers or expressions.

- `on` runs from the note's start while it is held. Its times are from the
  start; past its last point its value stays. Where the note ends, `on` is
  cut, wherever it is.
- `off` runs from where the note ends, from the value `on` was cut at. Its
  times are from the end. A recipe sounds past the note's end for as long as
  its longest `off`. `off` may be left out: the value then stays where it was
  cut.
- A point's `curve` shapes the stretch that arrives at it: 0 (the default) is
  a straight line, and otherwise it is SuperCollider's `Env` curve, `curve`
  under 0 falling early as a decay does. From `a` to `b` with a curve of
  `-k`, the stretch is `a * e^(-k u)` over it when `b` is `a * e^(-k)`: an
  exponential decay itself. A curved stretch is played as straight pieces
  10 ms long.

A note's level is in its samples already: the engine multiplies what `out`
plays by `level`. What `out` plays also passes a high-pass at 20 Hz, as a
synthesizer's output stage does, so a recipe holds no offset: a sine moved by
a wave of its own freq has one. A recipe may also use `level` for its tone, a brass brighter
when it is played louder.

## Recipes over recipes

A recipe with a `base` is that recipe with the rest of it over it, as JSON
Merge Patch (RFC 7386) puts it: an object goes over an object field by field,
`null` takes a field away, and anything else takes the place of what was
there.

```json
{"base": "fm_brass", "knobs": {"bright": {"default": 0}}}
```

```json
{"base": "fm_brass", "parts": {"m1": {"feedback": 1.0}, "m2": {"feedback": 1.0}, "l3": {"gain": 0}}}
```

A base may have a base of its own. What the recipe and its bases make is
checked whole, as any recipe is.

## Errors

A recipe is checked when it is loaded, and an error says where in it:

```
recipe fm_brass.parts.i1.gain.on[2][1]: `3 + brigt`: no name `brigt`; it may name pitch, f, dur, level, bright
```

What depends on the note is checked when the note plays: times that go back
for that note, or a value the graph refuses (a `freq` past half the rate for a
high note), with where it is in the recipe.
