# Sample songs

Songs written for Kauai to hear its instruments in, and to try one against
another.

| File | Song | Its band |
|---|---|---|
| [`window_rain.kau`](window_rain.kau) | Window Rain, a slow jazz ballad in Eb (2:32): two A sections, a bridge that turns to Gb, the A again at its height, and a soft close | `VintageElectricPiano` for the tune, the chords and the bass; ride, pedal and open hi-hats, kick and crash |
| [`late_terrace.kau`](late_terrace.kau) | Late Terrace, a bossa nova in F (1:00) | `VintageElectricPiano` for the tune, the comping and the bass; kick, hi-hat and cross-stick |
| [`cruise.kau`](cruise.kau) | Cruise, a fusion tune in C at 129 bpm (1:01), from an intro of its own to the last chorus's chord, held | `SopranoSax` for the tune and `ClearAnalogLead` on its harmony, `DistortionGuitar` cutting in sixteenths and a second one muted (`mute=1`) on the sixteenths between, `FmBass`; kick, snare, hi-hats, toms and crash |
| [`sprint.kau`](sprint.kau) | Sprint, a fusion tune in Eb at 150 bpm (0:56), its last chorus in F, closing on a kime | `AltoSax` for the tune and `FmBrass` on its harmony, `DistortionGuitar`, `FmBass`; kick, snare, hi-hat and crash |
| [`coast_road.kau`](coast_road.kau) | Coast Road, a slow ballad in Eb (1:29), with the sea before it and after it | `FmElectricPiano` for the tune, `VintageElectricPiano` for the chords, `FmBass`; pedal hi-hat, ride and `SeaWave` |

```bash
kauai kauai/sample_songs/window_rain.kau                       # play it
kauai kauai/sample_songs/window_rain.kau --wav rain.wav        # write it to a WAV
culebra kauai/tools/play.cul --sounds kauai/recipes kauai/sample_songs/late_terrace.kau
```

## Trying another sound

Each line of a song's band names its instrument and sets the
instrument's knobs with `name=value`:

```
voice Keys   VintageElectricPiano bright=0.8           plays melody  vol 44
```

Put another instrument in its place (`FmElectricPiano`,
`SparklingFmElectricPiano`, or any `kauai instruments` lists) and set its own
knobs: `tremolo=0` takes the vintage piano's tremolo away and `chorus=0` its
chorus, and `sparkle=1` makes `FmElectricPiano` sparkle as much as it can.
[`../recipe.md`](../recipe.md) lists each instrument and its knobs. A knob may
also change partway through a line, from a note on, as `\tremolo=0.8`
([Directions](../../docs/kauai/language.md#directions)). The drums are tried
the same way, from the kit in `../recipes/drums/`.
