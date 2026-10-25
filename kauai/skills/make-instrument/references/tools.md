# The tools, as this work uses them

`culebra docs -g '<name>'` prints the reference for a Culebra name; look a
signature up there rather than guessing it. Each tool below has its own
README section (`kauai/tools/README.md`) with every option.

## Rendering a song or a passage

```sh
culebra play --sounds kauai/sounds/alto_sax --wav out.wav song.kau
culebra play --sounds kauai/sounds/alto_sax --wav part.wav --from Theme --to Bridge song.kau
```

`--from` and `--to` are a mark's name, seconds (`83.5`), or minutes and
seconds (`1:23.5`). A part starts a second before `--from`, rising from
silence over it, and runs until what started before `--to` has rung out;
from `--from` on it is sample for sample the whole song's. Only the
external sounds the part plays are made, so a passage renders much faster
than the song.

From a program, `song.render(from:, to:)` gives the samples, `song.marks()`
where each mark is, `song.seconds(at)` a point in seconds, and
`song.external_sounds(from:, to:)` what each instrument is asked for in
the passage and where it plays.

## Checking before listening

```sh
culebra kauai/tools/check.cul --sounds kauai/sounds/alto_sax --from Theme song.kau
```

Each external sound is checked against its notes: pitch off the score,
dropouts, dips that come back up, bumps over the level around them, abrupt
starts and ends, clicks that are not the wave's own steep edges, clipping. A
note is looked at past its attack and before its release, and from its start
or to its end where a slur joins it to its neighbour (a join is where one
recording meets another, and where the engine once put half a dip's depth on
the next note: a G5 12 dB over its neighbours, heard as a sudden loud note
in a fast run). The mix is checked for clipping and silence where notes
sound. A sound heard in several places is one line.
Small edges and steps (a few percent of a sound's peak) are counted on one
line; `--all` lists them. The exit status is 1 when something that is not
small is found, so a script can stop on it. `--json FILE` gives every
place, for comparing two versions' findings.

The finished soprano saxophone, over a minute of an upbeat test song, gives
one finding (a sharp swing inside a recorded attack, which may be the
tongue itself) and a few dozen small edges and steps: that is the level to
expect from an instrument the person is happy with, not zero.

## Listening blind

```sh
culebra kauai/tools/listen.cul start NAME --out work/listen --song S --from A --to B \
  --sounds before.cul --sounds after.cul     # two modules
culebra kauai/tools/listen.cul start NAME --out work/listen --song S \
  --sounds pkg --set breath=0.5               # a parameter set on the new one
culebra kauai/tools/listen.cul pick NAME 1    # or 2, or same
culebra kauai/tools/listen.cul next NAME --out work/listen --set breath=0.8
```

`references/listening.md` says how to use it with the person.

## Making the sounds beforehand

```sh
culebra kauai/tools/make_sounds.cul --sounds kauai/sounds/alto_sax --out made song.kau
```

For a program (a game) that cannot wait while a slow instrument makes its
phrases, and for the browser, where only the program's own folder is
served. It keeps the sound Kauai weighs the instrument by, so the made
sounds play as loud as the instrument's own.

## measure.cul

```sh
culebra kauai/tools/measure.cul --low 200 --high 1400 --out work/measured work/sources/*.wav
```

Each recording's pitch, loudness, 40 harmonics and breath in five bands
every 10 ms (NAME.f32) and the notes read from them (NAME.json: each
note's start, length, semitone and own pitch, how it starts and ends,
level, vibrato depth and onset).
`kauai/tools/README.md` has every field. `--low` and `--high` are the
instrument's lowest and highest notes with a little room; the narrower,
the fewer notes read an octave off. Look at the notes' count and how they
start before building on them: a recording whose notes are mostly `rest`
was read with gaps (too quiet, or the range wrong).

## make_data.cul

```sh
culebra kauai/tools/make_data.cul --sounds kauai/sounds/alto_sax work/measured/*.json
```

The package's `data/` from what `measure.cul` wrote: the notes it borrows
(pitch, loudness, harmonics' shapes, breath and fine gain, frame by frame),
the attacks (cleaned of the air on the microphone) and how the breath
pulses with the wave. The recordings are read again from `work/sources`
(`--sources` says otherwise). The engine's `data_format.cul` lays the
numbers out, the same for every instrument. The package's
`data/instrument.json` holds what is the instrument's own: `make` (which
notes are left out, `left_out`: a harsh take, a note measured along half its
pitch; the noise its breath is made from) and `play` (the style the engine
plays the data in). The soprano's limits were set by ear, so check them
against the new instrument's recordings. Three 192 s
recordings take about a minute. `kauai/tools/README.md` says how each part
is made.

## Measuring in Culebra

| Function | Gives |
|---|---|
| `Audio.decode(bytes)` | a recording's samples as recorded: `{rate, channels}` |
| `Audio.resample(xs, from, to)` | the samples at another rate, band-limited |
| `Audio.spectrum(xs, size:, hop:)` | the magnitudes frame by frame (an STFT) |
| `Audio.stft(xs, size:, hop:)`, `Audio.istft(re, im, size:, hop:)` | the same frames' bins as they are (`{re, im}`), and the sound frames of bins stand for: work on a sound bin by bin (a mask, a band, a Hilbert transform) and put it back |
| `Audio.pitch(xs, low:, high:, rate:)` | the pitch every 10 ms, `{hz, clarity}`, `nil` where there is none |
| `Audio.harmonics(xs, hz, count:, rate:)` | the harmonics' amplitudes along that pitch, as `Audio.partials` takes them, and the `rest` without them |
| `Audio.loudness(xs, rate)` | the loudest 400 ms (BS.1770 momentary), the measure Kauai weighs an instrument by |
| `Audio.integrated_loudness(xs, rate)` | the whole, gated (BS.1770 integrated) |

## Sweeping single notes for what a few songs miss

`check.cul` plays the songs there are. A fault that needs a particular
pitch, length and level shows only where a song happens to go: the alto's
borrowed attack began before its sound for a note of 0.195 s at F#5, and for
six other combinations, while two songs asked for one of them. `sweep.cul`
(`kauai/tools/README.md`) plays single notes over the range, the lengths a
tune uses, the levels and the next phrase (none, or at once), and measures
each: a sound starts from nothing (under 3% of its peak in its first
samples), does not clip and is in tune. Run it on a new instrument and again
after a fix. When it finds something, put the fix in one place that holds it
for every sound (the alto's exit fades the first 3 ms in, whatever the pieces
did), and the worst cases into the instrument's test, which must fail before
the change. A song's `check.cul` finding is a lead; the sweep is the proof
that it is gone. Vary also the note that follows (none, or a leap either way):
the same note must sound the same whatever comes after it.

To set a modelled instrument from recordings, or to judge it, measure both
with the same tool: `envelope.cul` gives the spectral envelope in Hz and the
breath of what `measure.cul` measured, of the recordings and of the
instrument's own sound written to a WAV.

One thing to read off the data before a sweep: how far each recorded note's
loudest frame stands over its level (the median of its middle, which the
engine sets to the loudness asked). A take that opens with a puff of breath
20 dB over its middle plays 20 dB over what the song asked. The alto's data
had 334 such notes of 2520 (over 8 dB); the soprano's, 43 of 1809. The
choice costs them (`crest_from`, `crest_db`), and a count of them is worth
having when the recordings are chosen.

