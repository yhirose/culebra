# Kauai tools

## listen.cul: two ways of a passage, heard as 1 and 2

`listen.cul` renders one passage of a song two ways that differ in one
thing, and writes them as two WAVs named only 1 and 2, at the same
loudness. Which is which is kept in a record and said once the listener
has picked one. The one picked is then heard against the next way to try,
round after round, as an optician keeps the lens that reads better and
offers the next.

```sh
culebra kauai/tools/listen.cul start breath --song cruise.kau \
  --from Theme --to Bridge --sounds kauai/sounds/soprano_sax --set breath=0.5
culebra kauai/tools/listen.cul pick breath 2
culebra kauai/tools/listen.cul next breath --set breath=1
```

`start NAME` renders the two ways. A way is a song, the instrument module
that makes its `external` sounds (as `culebra play --sounds` takes one),
and parameters set over the song's. `--song` and `--sounds` given once are
both ways'; given twice, the first is the one before and the second the
new one. `--set NAME=VALUE` is the new one's: every instrument of its
module plays with the parameter at that value, whatever the song's band
line and directions set it to. `--from` and `--to` choose the passage, each
a mark's name, seconds, or minutes and seconds (`1:23.5`), as
`culebra play --from` and `song.render(from:, to:)` take them.

| Command | What it does |
|---|---|
| `start NAME` | renders the one before and the new one, and writes round 1 |
| `pick NAME 1`, `2` or `same` | records the one picked, and says which was which; `same` keeps the one before |
| `next NAME` | the one kept with one more change (`--song`, `--sounds` or `--set`, the parameters set on top of the ones it has), heard against the one kept, as the next round |

| Option | What it does |
|---|---|
| `--out <dir>` | where the WAVs and the record go (`listen`) |
| `--same-gain` | with `start`, hear the two as they are made instead of at the same loudness: for a change to how loud something is |

Round R is `NAME-R-1.wav` and `NAME-R-2.wav` in `--out`, each round under
names of its own, so a player can keep the last ones open. Which one is
the one before is chosen at random each round. The record, `NAME.json`,
holds the passage and, for each round, the two ways in slots `one` and
`two`, which slot holds the one before (`before`), how many dB louder the
new one was before they were evened out (`differ`, `nil` where one is
silent) and what was picked (`picked`). The one kept is not rendered again:
the next round hears the samples it was heard as.

The two are heard at the same integrated loudness
(`Audio.integrated_loudness`, ITU-R BS.1770), measured from `--from` on: the
new one is made as loud as the one before, since a louder version tends to
sound better whatever else differs. Kauai has weighed each instrument by one
sound already, so what is evened out is how the passage plays. Where either
would then pass full scale, both are lowered alike. A song is rendered as
`culebra play --wav` renders it, on the VM, the two side by side.

## check.cul: what is wrong, measured before listening

`check.cul` plays a passage of a song with an instrument module and looks
for what can be found by measuring, so that listening is spent on what
cannot. Each external sound the passage plays is checked against the notes
it was asked for; the passage as the song mixes it is checked as a whole.

```sh
culebra kauai/tools/check.cul --sounds kauai/sounds/soprano_sax \
  --from Theme --to Bridge song.kau
```

```
0:12.40  E5  sax  pitch  +31 cents (score E5)
0:18.05  G5  sax  dropout  -34 dB for 25 ms  (also 0:42.05, 2 places)
0:21.98  -  sax  edge  starts at 40% of its peak
0:33.10  -  mix  clipping  12 samples past full scale
small: 13 edges (5-14%), 24 steps (2-5%) in 11 sounds; --all lists them
mix: -16.2 LUFS, peak -0.3 dBFS
4 found in 23 sounds (11.2 s)
```

| Found | In | When |
|---|---|---|
| `pitch` | a note | its middle repeats at a period more than `--pitch` cents (25) off the score's, searched three semitones either way so a harmonic is not taken for the note; a note under 100 ms, in a chord, or marked to slide (scoop, plop, doit, fall) is not measured |
| `dropout` | a note | its level falls `--dropout` dB (30) under the level around it for 20 ms or more, past its first 40 ms and before its last 30 ms |
| `dip` | a note | the same, `--dip` dB (6) for 30 ms or more, coming back to within 3 dB of where it was (a note fading toward its end is no dip), where it is no dropout |
| `edge` | a sound | its first or last sample is `--edge` (0.2) of its peak or more: it starts or stops abruptly; small from 0.05 |
| `jump` | a sound | a step between two samples `--step` (0.1) of its peak or more, `--jump` times (8) the steps in the 10 ms around it, and three times the largest a period of its note before and after (a wave's own steep edge recurs every period, a click does not); small from 0.01. A drum's sound is not checked |
| `clipping` | a sound, the mix | three samples in a row at full scale in a sound; samples past full scale in the mix |
| `silent` | a sound | nothing in it at all |
| `silence` | the mix | `--silence` seconds (1) under -60 dBFS where the score has notes sounding |

The small edges and jumps are counted on one line, and `--all` lists them
too: they were found in the soprano saxophone, whose sounds start at up to
14% where an attack is taken from inside a recording, and where the noise
before a recorded attack gives way to its silence. A sound that plays in
several places (Kauai hands the instrument the same each time, and plays
the one sound it makes) is checked once, and what is found in it is one
line, at the first place, with the others after it.

`--sounds`, `--set`, `--from` and `--to` are `listen.cul`'s. `--json FILE`
writes what is found in every place as JSON: `found` (each `{at, what, by,
external, note, detail, value, small, sound, in}`, `at` seconds into the
song, `value` how much, `sound` which sound it is in and `in` how far into
it), `sounds` and `seconds` (how many external sounds were checked, and for
how long they play), and the mix's `loudness` (`Audio.integrated_loudness`)
and `peak` (dBFS). The exit status is 1 when something that is not small
is found. A sound is checked as the instrument made it, before the song
weighs it and mixes it in; what Kauai hands the instrument for each, and
where it plays, is `song.external_sounds()`.

## measure.cul: what a recording plays, measured

`measure.cul` measures recordings of one instrument, as an instrument is
made from them: each one's pitch, loudness, harmonics and breath every 10
ms, and the notes read from them, each with how it starts and how it ends.

```sh
culebra kauai/tools/measure.cul --low 200 --high 1400 \
  --out work/measured work/sources/*.wav
```

```
freesound_734969: 565 notes (after a rest 99, tongued 186, legato 280), tuning +10 cents -> work/measured/freesound_734969.json
```

`--low` and `--high` bound the pitch searched, in Hz: the instrument's
lowest and highest notes with a little room (a soprano saxophone 200 to
1400), so that a note is not read an octave off. Each recording (WAV, MP3
or Ogg, its channels averaged) is written into `--out` (`measured`) as
NAME.json and NAME.f32, NAME its file's name without the extension.

The pitch is `Audio.pitch`'s, tidied as the notes are read from it:
stretches with a pitch shorter than 40 ms are dropped, each frame is the
median of the five about it, and a gap under 60 ms between two stretches is
the tongue's: each side's pitch is held up to the gap's quietest frame,
where the second note starts. A stretch starts after a rest; it is split
where the tongue stops it, and where the pitch moves 0.6 of a semitone or
more to another semitone and stays there 40 ms (legato), so a vibrato does
not split a note. A note under 80 ms within 2 semitones of the one beside it
is that note's scoop or fall, and joins it. A note whose level (over two
periods of its pitch, every millisecond) falls 6 dB under the highest it
reaches within 100 ms on either side, 40 ms or more from its ends, is one
pitch played twice with the tongue, and is cut there.

NAME.json holds where the recording came from (`source`, `rate`,
`seconds`), the `settings` it was measured with, its `tuning` (cents from
A = 440 Hz, the median of how far its frames stand from the nearest
semitone), `reference` (the dBFS of the loudest 5% of the frames with a
pitch), how the frames are laid out (`frames`: `{count, step, columns,
file}`), and the `notes`, one a line:

| Field | What it is |
|---|---|
| `t0` | its first frame's time, seconds |
| `frames` | how many frames (10 ms) it lasts |
| `semi` | its semitone (MIDI, 69 is A4), with the tuning taken off: the nearest to its middle (30% to 90% of the way through) |
| `own` | its middle's pitch, cents from `semi` |
| `from`, `to` | how it starts and ends: `rest`, `tongue` or `legato` |
| `level` | its middle's loudness, dB from `reference` |
| `vib` | its vibrato's depth in cents, over the part 180 ms in from either end (0 for a note under 360 ms) |
| `onset` | when a note entered after a rest or with the tongue starts to sound: from the quietest 2 ms within 120 ms before its first frame and 40 ms after, the first moment its level (over 2 ms) comes within 10 dB of its steady level. `null` for a legato note, one under 150 ms, and one that does not rise 6 dB |

The harmonics are `Audio.harmonics`' along the pitch where the notes have
one: the pitch as `Audio.pitch` read it, and across a stop of the tongue,
where it read none, the pitch held across it. The breath is what is left
without them (`Audio.harmonics`' `rest`): in each of five octave bands from
500 Hz to 16 kHz, its power against the harmonics' own, in dB, both through
a Hann window of 2048 samples (46 ms at 44100) about the frame. So the
noise right at a harmonic's frequency counts as the harmonic: band noise
reads about 1 dB under its power against the tone.

NAME.f32 is each frame's numbers, little-endian 32-bit floats, frame after
frame, in the order `frames.columns` names them: `hz`, the pitch the notes
are read from (0 where none); `db`, the loudness over the 10 ms about it in
dBFS (-120 for silence); `h1` to `h40`, the harmonics' amplitudes, as
`Audio.partials` takes them; and `b1` to `b5`, the breath in each band
(`settings.breath_bands`). Where `hz` is 0 the harmonics and the breath are
0 too. Frame `t` is `t * 10 ms`. A 192 s recording takes about 8 s.

## make_data.cul: an instrument's data/ from what was measured

`make_data.cul` makes the `data/` an instrument plays from, out of what
`measure.cul` wrote of its recordings: the notes it borrows, the attacks
it grafts, and how its breath pulses. The engine's `data_format.cul`
(`kauai/engine/`) lays the numbers out, and the engine reads them through
the same file. What the instrument is made with is its `data/instrument.json`'s
`make`: `left_out` (which notes are left out) and `breath_noise` (the noise
its breath is made from).

```sh
culebra kauai/tools/make_data.cul --sounds kauai/sounds/alto_sax \
  work/measured/*.json
```

```
freesound_734969: 565 notes
freesound_734970: 575 notes
freesound_734971: 549 notes
left out: 57 notes 9.0 dB or more brighter than those within a semitone, 7 measured along half their pitch
kauai/sounds/alto_sax/data: 1629 notes (2353300 bytes), 496 attacks (5031424 bytes), breath pulsed [0.029, 0.231, 0.517, 0.615, 0.548]
```

The recordings are read again (the fine gains, the attacks and the breath
need their samples) from `--sources`, by default the folder `sources`
beside the measured one (`work/measured` -> `work/sources`); `data/` goes
in the instrument's folder unless `--out` says. The three recordings above
(192 s each) take about a minute.

Each note, frame by frame (10 ms) from its first frame for as many as
`measure.cul` gave it:

- its pitch in cents from its semitone (the recording's tuning taken off),
  held through a frame without one;
- its loudness: the harmonics' power in dB against the recording's 95th
  percentile of it where it has a pitch, so the level a note borrows is
  the harmonics' and not the breath's;
- its harmonics' shapes: each harmonic's log a mean over the pitched frames
  within 70 ms of the frame in the note (150 ms in all), scaled to unit
  power (a frame without a pitch takes the nearest one's); a harmonic from
  the 6th on standing more than 10 dB over the median of the five about it
  brought down to it;
- its breath in each band: where it has a pitch, as measured; elsewhere a
  monotone cubic through those; then a median over 3 frames, no higher than
  +3 dB;
- and its fine gain every millisecond from 5 ms before its first frame:
  the recording's RMS over a period either side of its pitch against its
  harmonics' level there, at most 4.

Two kinds of note are left out, with their attacks, by limits the
package's `data/instrument.json` sets (`make.left_out`; the soprano's
below): a harsh
take, whose harmonics from 3 kHz up are 9 dB or more brighter (against all its
harmonics, in its median frame) than the median of the notes within a
semitone of it; and a note whose odd harmonics (1st, 3rd, 5th, 7th) are 12
dB or more under its even ones, its harmonics measured along half its
pitch (the pitch read an octave low). Either, borrowed, sounds cracked
rather than like the instrument; leaving out those 12 dB brighter instead
was heard as less round.

A spike (a value further from the median of the 7 about it than a floor
and five times what is usual for the note) is filled with that median in
the pitch (floor 25 cents), the loudness (4 dB) and the fine gain (3 dB),
leaving a note's first and last 3 frames (35 ms of gain) as they are: what
leaps there is a slur's move, an onset or a release. A note's `level` is
its middle's loudness (30% to 90% of the way through).

Each note `measure.cul` found an onset for gives an attack: its samples
from 30 ms before the onset for 150 ms, cleaned of the air that strikes the
microphone (first the recording's steady noise, each bin's 10th percentile
taken out twice over, at most 20 dB; then, in frames of 1024 samples every
64, each bin kept within a fifth of the spacing of a harmonic of the
frame's own pitch, falling to -14 dB between them, nothing under 0.8 of the
pitch, the bins left as they are above the harmonic the pitch's move shifts
by a tenth of the spacing), and its pitch 60 to 80 ms in (`f0_fade`, where
it is faded into the borrowed note). How the breath pulses is read from
every note of 0.8 s or more: over its middle 0.3 s, what lies between the
harmonics in each band, its envelope fitted against the phase of the first
harmonic as c0 + c1 sin(phase - shift); the median of c1 / c0, as the share
of the breath pulsed, `2 m / (1 + m)`.

## compare.cul: how far one rendering is from another

`compare.cul` measures how far a candidate rendering of a score is from a
reference one of the same score, in numbers a listener can weigh, so that a
change can be judged without listening first, and a build of an instrument
can be set against a target.

```sh
culebra kauai/tools/compare.cul --low 200 --high 1400 ref.wav cand.wav
culebra kauai/tools/compare.cul --low 200 --high 1400 --yardstick \
  --json work/compare.json ref.wav cand.wav
```

Both are read every 10 ms (the frames line up when the score is the same,
so render both with `culebra play --wav`), and the distance is taken over
the frames where either sounds (within 50 dB of its loudest). `--low` and
`--high` bound the pitch searched, as `measure.cul`'s do.

| Number | What it is | Limit |
|---|---|---|
| `spectral_db` | the log spectrum over 48 bands from 100 Hz to 16 kHz, rms over the bands, mean over the frames: the timbre as a whole | 3.0 |
| `loudness_db` | the level of the frame, mean absolute difference | 1.0 |
| `integrated_lu` | the programme loudness (BS.1770), absolute difference | 0.5 |
| `pitch_median_cents`, `pitch_p95_cents` | the pitch where both have one: the median and the 95th percentile of the absolute difference | 5, 30 |
| `voicing` | the share of frames where only one has a pitch | 0.03 |
| `centroid_cents` | where the spectrum's centre of mass lies: brightness | 60 |
| `breath_db` | the noise against the harmonics where both have a pitch (`Audio.harmonics` along each one's own pitch) | 3.0 |
| `vibrato_cents` | how much the pitch moves about its slow course inside a note (the rms of its quick moves; a jump of more than 60 cents between frames is a new note), per second where both hold a pitch, the mean absolute difference | 3.0 |

The limits are a starting point, about where a listener stops telling the
two apart; the exit status is 1 when a number is over its limit, and the
person has the last word. `worst` lists the five whole seconds where the
spectrum differs most, to cut out and listen to.

`--yardstick` also prints what four known degradations of the reference
score over its first 20 seconds (1 dB quieter, a 6 kHz low pass, noise at
-45 dB of the peak, 10 ms late), to read the candidate against: a 10 ms
delay already moves `voicing` and `centroid_cents`, so a build whose notes
start a few milliseconds off is not wrong for that alone. `--json FILE`
writes every number, what is over, and the yardstick. A minute of music
takes about 4 s.

## fit_style.cul: an instrument's style found from targets

`fit_style.cul` finds an instrument's style (`data/instrument.json`'s
`play`, the numbers the engine plays its recordings in) so that songs it
plays sound like targets: renderings of the same songs by whatever the
instrument should sound like.

```sh
culebra kauai/tools/fit_style.cul --sounds kauai/sounds/alto_sax \
  --measured work/measured --seconds 30 \
  --tune work/songs/tune_a.kau=work/targets/tune_a.wav \
  --tune work/songs/tune_b.kau=work/targets/tune_b.wav --log work/fit.json
```

The package's `data/` must be made and hold `instrument.json` with a `make`
(its `make.measure` is the pitch range). With no `play`, or with `--derive`,
a first style is read off the recordings themselves: where a note's onset
lies for each way in (the medians over the notes measure found an onset for),
the first note played as an overtone (the top of the long notes, plus one),
the vibrato of long notes. The level numbers are a guess (and stay one: Kauai
weighs an instrument by the loudness of its sounds, so how loud a note is
asked for, absolutely, does not reach a song). Then `db_per_octave`, the
vibrato's `depth` and `straight`, `own_kept`, `drift_kept` and `overtone_from`
are moved one at a time by a step while that lowers the cost (compare's
numbers over their limits, weighted), halving the step after a round that
moved nothing. `lib_louder` follows `mf_db`, so the notes borrowed do not
change when the level does. The style is written back unless `--dry`; `--log` keeps every style tried and its cost.
A 20 s excerpt of a song, one round, takes about a minute.

## evaluate.cul: does an instrument sound like its targets

`evaluate.cul` plays each song of a case with an instrument
(`culebra play --wav`, so any package, however it makes its sounds) and sets
it against the target rendering with `compare.cul`'s distance. The case is
`kauai/harness/cases/<name>/case.json` (its `range`, and the songs of `eval`
or, with `--set tune`, `tune`).

```sh
culebra kauai/tools/evaluate.cul --sounds kauai/sounds/alto_sax \
  --case kauai/harness/cases/alto_sax/case.json --targets work/targets
```

It prints a line a song and what is over its limit, keeps the renderings and
the numbers (`evaluate.json`) in `--out`, and exits 1 when a song is over a
limit. Two songs take about 25 s.

## make_sounds.cul: an instrument's sounds made beforehand

`make_sounds.cul` asks an instrument module for every sound Kauai songs need
from it, the way Kauai asks when it plays them, and keeps each one as a file,
with an index to look each one up in. A program that plays the songs then
hands Kauai those files instead of the instrument, so an instrument that is
slow to make its sounds (one played from recorded notes, say) plays at once,
and in the Playground, where only the program's own folder is served.

```sh
culebra kauai/tools/make_sounds.cul --sounds kauai/sounds/soprano_sax \
  --out made song.kau other_song.kau
```

| Option | What it does |
|---|---|
| `--sounds <module>` | the instrument module: a `.cul` file, or a folder and its `main.cul`, as for `culebra play --sounds` |
| `--out <dir>` | where the index and the sounds go |
| `--name <name>` | the index is `NAME.json` and the sounds go in `NAME/` (`sounds`) |
| `--use FILE=OTHER` | where the songs `use` FILE, read OTHER beside it: a band with an external lead in place of the one the songs name (any number of these) |
| `--ogg` | keep the sounds as Ogg Vorbis, by `ffmpeg` (`--quality`, 0 to 10, default 5) |
| `--pack` | all the sounds one after another in `NAME.bin`, in place of a file each |

The index says, for each instrument the songs asked, what it hears (an
instrument given as `{phrase:}` or `{hit:}`) and where each of its sounds
is, keyed by the JSON of what Kauai handed the instrument:

```json
{"voices": {"sax": {"hears": ["len", "level", "marks", "next", "params"],
                    "sounds": {"[[{\"pitch\":62,\"at\":0.0, ...}],{}]": "made/sax_0.ogg", ...}}},
 "drums": {"kick": {"sounds": {"[{}]": "made/kick_0.wav"}}}}
```

The key is `JSON.stringify([notes, params])` for a voice given as
`{phrase:}`, `[pitch, params]` for one given as a function, `[n, params]`
for a drum given as `{hit:}`, and `[params]` for one given as a function.
A sound is a file name beside the index or, with `--pack`, its
`[offset, size]` in `NAME.bin`.

Kauai weighs an instrument by one sound before it plays (a voice's note at the
middle of the pitches the voice plays, at mf, a second long; a drum's hit at
mf), and reads the loudness of a WAV on every backend but of an Ogg or an MP3
on none. That sound is kept as a WAV even with `--ogg`, so the songs play the
made sounds exactly as loud as the instrument's own. If Kauai stops asking
for that sound as its docs say, the tool stops rather than make sounds that
would play at another level.

### Playing them

A program gives Kauai an instrument of the same shape that answers from the
index. A voice given as `{phrase:}`, packed:

```culebra
# doctest: skip (the files are the program's own)
let made = JSON.parse(dir.read("made.json")).voices.sax
let bytes = dir.read("made.bin")
let sax = {
  phrase: fn (notes, params) {
    let at = made.sounds[JSON.stringify([notes, params])]
    Audio.Sound(bytes.slice(at[0], at[0] + at[1]).to_string())
  },
  hears: made.hears,
}
let song = Audio.Kauai.load("song.kau", dir: dir, voices: {sax: sax})
```

A key that is not there is a song changed since its sounds were made: make
them again. `examples/games/retro-run` plays its laps' saxophone this way,
and falls back to its chip lead when a phrase is missing.
