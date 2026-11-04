# Recipes

A recipe is an instrument written as data: a JSON file that wires
`Audio.Graph`'s parts together. The engine
([`engine/recipe.cul`](engine/recipe.cul)) checks it whole when it is loaded,
then builds the graph for each note and renders it. No code is loaded, so a
recipe plays the same under `culebra play` and in the `kauai` binary.

The recipes Kauai comes with are in [`recipes/`](recipes), and its drums in
[`recipes/drums/`](recipes/drums) ([Drums](#drums)). A file's name is the
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

`culebra play --sounds kauai/recipes song.kau` plays a song with them and the
drums, and the `kauai` binary has them built in.

## Drums

A drum is a recipe in `recipes/drums/`, named as the others are, and a song
gives it as a drum: `drums/open_hat.json` is `drum OpenHat external OpenHat`.
A drum's name is the drum itself, as a drummer calls it: General MIDI's
Bass Drum is `Kick`, its Side Stick `CrossStick`.

| Recipe | A song names it | What it is | Knobs: default (range) |
|---|---|---|---|
| `kick` | `external Kick` | a bass drum: a sine falling from 150 Hz to 47 Hz, and the beater's thump, brighter the harder it is struck | |
| `snare` | `external Snare` | two modes of the head falling in pitch, and the wires' noise, brighter the harder it is struck | |
| `cross_stick` | `external CrossStick` | the stick laid on the head and struck on the rim: a wooden knock and a click | |
| `hat` | `external Hat` | a closed hi-hat: six square waves at unrelated pitches for the metal, and noise, high and short | |
| `pedal_hat` | `external PedalHat` | the hi-hat closed by its pedal: shorter and lower than `hat` | |
| `open_hat` | `external OpenHat` | an open hi-hat, ringing until it is closed | |
| `ride` | `external Ride` | a ride cymbal: the metal's wash, its ping and the stick | |
| `crash` | `external Crash` | a crash cymbal, darkening as it fades over 2.6 s | |
| `tom` | `external Tom` | a tom: the head falling to its pitch, a second mode over it, and the stick | `tune` 147 (60 to 400): the head's pitch, in Hz |
| `high_tom`, `mid_tom`, `low_tom` | `external HighTom`, `MidTom`, `LowTom` | `tom` tuned high, in the middle (as `Tom`) and low | `tune` 196, 147 and 98 |

A drum is struck at middle C (`f` is 261.63 Hz); one that is tuned has a
knob for it, as the toms' `tune`. A hit holds the recipe for as long as the
`on` of its values lasts, the longest of them, and then plays their `off`:
the `on` is what the stick or the pedal does, the `off` what rings after
it. The hit is cut short where the drum, or a drum of its `choke` group, is
struck next, so an open hi-hat, whose `on` is its ring, is closed by the
pedal:

```
band Kit {
  drum Hat       external Hat       vol 10  choke Hats
  drum PedalHat  external PedalHat  vol 8   choke Hats
  drum OpenHat   external OpenHat   vol 10  choke Hats
  drum Ride      external Ride      vol 9
}
```

Each drum stays under full scale at the loudest a song strikes it (an
accented `ff`). How loud it is against the band is its `vol`: Kauai weighs
each drum by one hit, as it does an instrument by one note.

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
| `gain` | `input`, `modulators` (a list) | `gain` (1) | |
| `mix` | `inputs` (a list) | `gain` (1) | |
| `pluck` | `input` | `freq` (`f`), `ring` (2) | |
| `delay` | `input`, `modulators` (a list) | `time` (0, seconds) | |

A field that takes in parts holds their names. A wave's `modulators` move its
phase, a delay's move its time and a gain's add to its gain: a slow wave
makes a tremolo, which starts with each note, as a synthesizer's LFO in key
sync does, since each note is a graph of its own. A wave's `freq` and a
pluck's are the note's own (`f`) unless the recipe says otherwise.

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
when it is played louder. The notes a song hands together, a chord among
them, are mixed 12 dB down, so that several at `ff` stay under full scale;
Kauai weighs an instrument by one of its phrases, so this does not change
how loud it plays in a song.

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
