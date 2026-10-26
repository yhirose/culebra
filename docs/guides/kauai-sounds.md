# Writing a sound in Kauai

A guide to the sounds a Kauai song plays: the four built-in voices, how to
shape them, and how a program or a folder supplies an `external` instrument.
It starts from the built-in sounds, which need nothing else, and ends with
where an instrument made from recordings comes from. The complete reference is
[the language specification](../kauai/language.md), and the songs themselves
are in [Writing a song in Kauai](kauai-songs.md).

## The built-in voices

A band names its voices. Each voice takes a source, one of four built-in
ones, and then the options that shape it:

```kauai
band Run {
  voice Lead    pulse     plays melody            duty 1/4  vol 15  env 1 2 4  gap 2
  voice Second  pulse2                            duty 1/8  vol 9   env 1 2 6  gap 2
  voice Bass    saw       plays roots in E2..D#3            vol 18  env 0 3 3  gap 3
  voice Arp     triangle  plays roots in C3..B3            vol 7   env 1 4 6  gap 4
}
```

| Source | The sound |
|---|---|
| `pulse` | a pulse wave, whose shape is set by `duty` |
| `pulse2` | a second pulse wave, for a harmony beside a `pulse` voice |
| `triangle` | a triangle wave |
| `saw` | a sawtooth wave |

A voice takes these options. `vol` is required.

| Option | Meaning | Default |
|---|---|---|
| `duty 1/8`, `1/4`, `1/2`, `3/4` | the pulse wave's duty cycle | `1/2` |
| `vol N` | the level at `mf`, from `0` to `100` | (required) |
| `env A D R` | attack, decay and release, in ticks of 1/60 second | `0 0 0` |
| `gap N` | ticks cut from the end of every note, so notes sound apart | `0` |
| `poly` | for an external voice that sounds several notes at once | one note at a time |

A channel plays one note at a time, and two voices of a band cannot share a
channel. A voice that says `plays melody` plays the melody, `plays roots`
plays the bass line under each chord, and `plays chords from NOTE` plays the
chord itself, voiced from that note up. Give a voice no `plays` and it plays
only what it echoes or harmonizes (a derived part).

Play the band in a program, and check what it sounds like:

```culebra
let song = Audio.Kauai.new(`
band Run {
  voice Lead    pulse     plays melody            duty 1/4  vol 15  env 1 2 4  gap 2
  voice Second  pulse2                            duty 1/8  vol 9   env 1 2 6  gap 2
  voice Bass    saw       plays roots in E2..D#3            vol 18  env 0 3 3  gap 3
  voice Arp     triangle  plays roots in C3..B3            vol 7   env 1 4 6  gap 4
}

song Sound {
  tempo 100
  band Run
  groove Pad
  melody in C5..B5

  Line
}

groove Pad {
  Bass  1 - 5 -
  Arp   5 - 1 -
}

section Line {
  C  e4 g c'2
  F  a4 c' f2
}
`)
println(song.length())             # => 4.8
println(song.events()[0])          # => {at: 0.0, len: 0.6, by: 'Lead', pitch: 76, vol: 15.0}
```

`Second` has no `plays`, so it sounds only what a derived part gives it, such
as a `harmonizes` line under `Lead` ([Derived parts](../kauai/language.md#derived-parts)).

## Shaping a sound

Four options do most of the shaping:

- **`env`** gives the note its attack, decay and release, in sixtieths of a
  second. The default, `0 0 0`, adds no shaping.
- **`gap`** cuts the end off each note, so that the notes of a line are heard
  as separate notes. The default, `0`, cuts nothing.
- **`duty`** sets the duty cycle of a pulse wave. It has no effect on the other
  sources.
- **`vol`** is the level, and the dynamics (`\pp` to `\ff`, see the
  language specification) scale it from there.

A direction changes the band's dynamic, or one part's, from its note on. The
dynamic in a bar line is the whole band's; the same mark inside a part's line
sets that part alone.

## External voices

A `voice` can take its sound from a program instead: `external NAME` names an
instrument the program supplies. The program gives the song a function for
each name, under `voices`. The function takes a pitch and the instrument's
parameters, and answers the sound of that note:

```culebra
let song = Audio.Kauai.new(`
band Lead {
  voice Ep  external Ep bright=0.5  plays melody  vol 40
}

song Test {
  tempo 120
  band Lead
  melody in C4..B4

  Line
}

section Line {
  C  c4 e g c4
}
`, {ep: |pitch, params| Audio.Sound([0.0, 0.5, 0.0, -0.5], rate: 44100)})
println(song.length())             # => 2.0
println(song.events()[0])          # => {at: 0.0, len: 0.5, by: 'Ep', pitch: 60, vol: 40.0}
```

Here the function is a stand-in: it returns four samples, and a real
instrument would synthesise the note at `pitch`, shaped by `params`. The
parameters come from the band's line (`external Ep bright=0.5`), and a score
can change them from a note on, with a direction such as `\bright=1.3`.

An external voice plays each note's sound through to its end, so it takes no
`env`, `gap` or `duty`. The note's length applies only to an instrument that
hears it, and `vol` still sets the level.

### Playing a song with an external sound from the command line

`culebra play` has no instrument of its own for an `external` voice. Give it a
module that exports `voices`, and it plays the song with them:

```culebra
# doctest: skip (a module for --sounds; electric_piano is the program's own)
let voices = {ep: |pitch, params| Audio.Sound(electric_piano(pitch, params.bright))}
export {voices}
```

```sh
culebra play --sounds sounds.cul song.kau
```

A song that names an external sound the module does not make is refused before
anything plays, so you see the mistake at once.

## Making a sound from a model

An external instrument can also be a model of a sound, written in Culebra
with `Audio.partials`, which builds a harmonic tone from a spectrum and
envelopes. An instrument that follows how a note is played, such as a horn,
is given as an object: `phrase` is its function, called with the notes of a
phrase, and `hears` names what of each note it is told (`len`, `level`, `next`,
and the others in the [language specification](../kauai/language.md#10-playing-from-culebra)).

The function below makes a plain organ-like tone, held for each note's length:

```culebra
fn freq_of(pitch) { 440.0 * Math.exp((pitch - 69) * Math.log(2.0) / 12) }

let organ = |notes, params| {
  let seconds = notes[0].len + 0.2
  Audio.Sound(Audio.partials(
    seconds,
    freq: [(0.0, freq_of(notes[0].pitch))],
    spectrum: [(0.0, [1.0, 0.5, 0.25])],
    gain: [(0.0, 0.0), (0.02, 0.5), (notes[0].len, 0.5), (seconds, 0.0)],
  ))
}

let song = Audio.Kauai.new(`
band Pipe {
  voice Organ  external Organ  plays melody  vol 60
}

song Test {
  tempo 100
  band Pipe
  melody in C5..B5

  Line
}

section Line {
  C  c4 e g c4
}
`, {organ: {phrase: |notes, params| organ(notes, params), hears: ["len"]}})
println(song.length())             # => 2.4
```

The pieces, in the order a model needs them:

- **`freq`, `gain` and `spectrum`** are envelopes: a list of `(at, value)` in
  seconds, from the start of the tone. `spectrum` is a list of `(at, amps)`,
  where the k-th amplitude is the partial at k times the frequency.
- **`Audio.partials(seconds, ...)`** takes the length first. It leaves out the
  partials above 0.45 of the sample rate on its own.
- **`Math.pow` takes whole numbers only**; a fractional power is
  `x ** y` with a Float operand.
- **`range(n).map(...)` is lazy**: call `.collect()` to get an array to pass on.
- The model's own changes over a phrase, such as a glide or a vibrato, are
  written as points in the envelopes: for example, one point every 10 ms.

[The standard library reference](../stdlib.md) lists every `Audio` function.
`Audio.filter`, `Audio.envelope` and `Audio.mix` shape a sound further, and
`Audio.Graph` builds one from parts.

## Making a recorded instrument with the harness

An instrument can also be made from recordings of a real player, so that it
sounds like the player and not like a model. The notes of each recording are
measured, and a song's notes are played by borrowing the recorded notes nearest
to them. The engine does the playing; the work is in choosing the recordings and
setting the numbers, which the culebra repository's Kauai harness automates.

Its flow has these steps, each done by an agent with one job:

1. **Design** what the recordings must cover: the range, the notes per
   semitone, the long notes per octave, the loud, middle and quiet notes, and
   the slurs the songs use. Do this before searching, so that the search has a
   target. Check the coverage later with the coverage tool.
2. **Find** recordings that meet the design. Each source's licence must allow
   reuse. A collection that asks for a credit and to be told how it is used is
   usable with that credit in the package's README.
3. **Compose** the songs the instrument is tuned and judged by.
4. **Measure** the recordings with the measuring tool, which reads each one's
   pitch, loudness, harmonics and breath every 10 ms, and the notes in it.
5. **Build** the instrument's data from the measurements (`make_data`), and its
   style: the `play` numbers in `data/instrument.json`, found by fitting them to
   the targets.
6. **Judge** by a distance: the comparison tool measures the rendering of the
   tuning songs against their targets, and the builder changes one thing at a
   time until the distance stops falling.
7. **Skeptic**: the evaluation songs, which were not used for tuning, are played
   to check that a good number is not met by a trick.
8. **Listen.** The numbers cannot say whether a join sounds right. The listening
   tool plays two versions of a passage, hidden as 1 and 2 at the same loudness,
   and the person keeps the one they prefer.

The fitting in steps 5 and 6 needs targets: recordings of the same songs played
by the instrument's player. A recorded instrument without such targets can be
made from its data and a style set by hand, as the engine's defaults allow, but
the numbers cannot then be tuned, and the listening step carries all the
judgment. Say so in the package's README.

A package made this way is a folder of its own, with a `main.cul` that passes
its data to the shared engine, the `data/` folder, and a README that names
every source and its licence.
