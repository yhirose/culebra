# Alto saxophone (from recordings)

An alto saxophone played from real notes. Each note of a phrase borrows the
recorded note nearest to it (in pitch, length, loudness, how it is entered and
left, and how much vibrato it has), keeps that note's own moves of pitch and
loudness and its timbre, and sounds them as harmonics with the breath between.
The notes come from a jazz alto player's improvisations, so a slur borrows a
recorded slur's moves and a tongued note a recorded tongued attack.

```
culebra play --sounds kauai/sounds/alto_sax song.kau
```

```
band Smooth {
  voice Lead  external AltoSax  plays melody  vol 60
}
```

## Songs

Three tunes in `songs/`, for the alto over a band of the tone channels:

| Song | What it is |
|---|---|
| `ocean_highway.kau` | bright fusion, Eb major, 136 BPM: a kime intro, a syncopated 16-beat groove, a solo, and the last chorus a tone higher, in F |
| `harbor_lights.kau` | a calm ballad, Bb major, 72 BPM: long notes over broken chords |
| `fast_lane.kau` | driving minor-key fusion, G minor, 156 BPM: a riff the alto and the guitar play an octave apart |

Their melodies keep to the pitches the recordings hold many notes of (the
flat keys': the takes are in G minor and F minor). E, B, F#, C# and A5 have
few, and few or no long ones, so a tune that asks for them is played from a
semitone away. `kauai/tools/coverage.cul --song` says, pitch by pitch, what a
song asks against what is recorded.

## Sources

Seven solo alto saxophone improvisations by **DieRechnug** on Freesound, used
as their HQ previews (MP3) and measured with `kauai/tools/measure.cul`:

| Recording | License |
|---|---|
| [sax solo dry](https://freesound.org/people/DieRechnug/sounds/607303/) (F minor, 75 BPM, 1:56) | [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) |
| [autumn leaves 1.wav](https://freesound.org/people/DieRechnug/sounds/632031/) | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |
| [autumn leaves 2.wav](https://freesound.org/people/DieRechnug/sounds/632030/) | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |
| [autumn leaves 3.wav](https://freesound.org/people/DieRechnug/sounds/632029/) | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |
| [autumn leaves 4.wav](https://freesound.org/people/DieRechnug/sounds/632028/) | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |
| [autumn leaves 5.wav](https://freesound.org/people/DieRechnug/sounds/632033/) | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |
| [autumn leaves 6.wav](https://freesound.org/people/DieRechnug/sounds/632032/) | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |

The "autumn leaves" takes are improvisations over Autumn Leaves (G minor,
about 124 BPM, 2:48 each); their maker shares the ones they did not use and
asks to hear what is made with them. `data/` holds what was measured from them: the
notes' harmonics frame by frame, the attacks, and the breath, changed from the
recordings in that form.

## Measured

2520 notes from C3 to A#5 (the lowest and the highest few semitones have a
handful each), 145 of them a second or longer, and loud, middle and quiet notes in each register
(`kauai/tools/coverage.cul`). The six Autumn Leaves takes share one loudness
reference (`make.reference_groups`), so their dynamics stay as played.

## Limits

The style (`data/instrument.json`'s `play`) is set from the soprano package's
relations, not fitted: no recording plays the songs it is judged by, so there
are no targets to fit it to, and the ear is the judge.

## Files

- `main.cul`: the playing (the shared engine, with this package's data).
- `data/`: the notes, attacks and breath made from the recordings, and
  `instrument.json` with the style (`play`) and the measuring and making
  settings (`make`).
- `work/` (not in git): the downloaded recordings, the WAV conversions and the
  measurements.
