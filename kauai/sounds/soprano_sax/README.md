# Soprano saxophone

An `external` instrument for Kauai that plays a soprano saxophone from real
notes. Each note of a phrase borrows the recorded note nearest it in pitch,
length, loudness, the way it is entered and left, and how much vibrato it
has. The note keeps that recording's own moves of pitch and loudness and its
timbre, frame by frame, and sounds them as harmonics with the breath between
them. A note longer than the one it borrows is filled out with steady
stretches of long notes near it. A note that starts a phrase, or is
tongued, begins with the recording's own attack. Notes from the top of the
range up (82, A#5) are played as overtones of the fingering an octave down,
as a player does to keep them round.

```sh
culebra play --sounds kauai/sounds/soprano_sax song.kau
```

```kau
band Smooth {
  voice Lead  external Sax  plays melody  vol 60
}
```

From a program, import `main.cul` and hand its `voices` to Kauai:

```culebra
# doctest: skip (the path is the program's own)
import sax from './kauai/sounds/soprano_sax/main.cul'

let song = Audio.Kauai.load("song.kau", voices: sax.voices)
```

`phrase_samples(notes, params)` gives a phrase's samples (mono, `RATE` a
second), for a program that keeps them or writes them out. The voice's
parameters (`breath`, `vibrato` and the rest) are listed at the top of
`main.cul`. A melody written for a higher instrument plays an octave lower
with `plays melody octave down` on the band's line.

A song's phrases are made when Kauai prepares them: about ten seconds for
each minute the saxophone plays, after the data loads in about a second. A
program that cannot wait for that (a game starting a level) plays phrases
made beforehand instead.

## Files

| File | What it holds |
|---|---|
| `main.cul` | the instrument |
| `data_format.cul` | how `data/` lays out the notes and attacks as 16-bit numbers, for `main.cul` reading them and the tools writing them |
| `data/notes.i16`, `data/notes.json` | 1629 recorded notes, 10 ms frames: pitch, loudness, the breath in five bands, the harmonics' shapes, and the fine gain every millisecond, as 16-bit numbers, and where each note's are |
| `data/attacks.i16`, `data/attacks.json` | 496 attacks, 16-bit samples from 15 ms before each onset to 100 ms after |
| `data/breath.json` | how the breath pulses with the tone in each band |

## Sources

The recorded notes and attacks were measured from three takes of a soprano
saxophonist sketching a tune, released into the public domain:

- "Sax_soprano_1_Cmin_60(120) BPM" by Kataguruma,
  <https://freesound.org/s/734969/>, CC0 1.0
- "Sax_soprano_2_Cmin_60(120) BPM" by Kataguruma,
  <https://freesound.org/s/734970/>, CC0 1.0
- "Sax_soprano_3_Cmin_60(120) BPM" by Kataguruma,
  <https://freesound.org/s/734971/>, CC0 1.0

`data/` is made from them by `kauai/tools/measure.cul` (`--low 200 --high
1400`) and then `kauai/tools/make_data.cul`.

A few numbers in `main.cul` (the shape of a written scoop, how much vibrato
a long note asks for, how a note's loudness grows up the range) were
measured from:

- "soprano sax solo MASTER.wav" by vedas, <https://freesound.org/s/171197/>,
  licensed under CC BY 3.0 (<http://creativecommons.org/licenses/by/3.0/>)
