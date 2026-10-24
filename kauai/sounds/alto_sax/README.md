# Alto saxophone (synthesized)

An alto saxophone made from a model of the sound, with no recordings in it. Its
numbers are measured from a jazz alto (see Sources):

- the harmonics follow the instrument's spectral envelope in Hz (flat to about
  600 Hz, a small rise near 1.35 kHz, falling to -43 dB at 4.3 kHz), so the
  timbre changes with the pitch as a real instrument's does, and brightens with
  the dynamic;
- a breath of noise sits under the tone, with a burst of it at a tongued start;
- a tongued note rises in about 12 ms;
- a long note's vibrato is small (about 3 cents at its widest) and comes in late.

A slur is one phrase: its notes glide into one another with no break in the
sound, and only a gap or the phrase's end lets the sound go. `\scoop`, `\plop`,
`\doit` and `\fall` bend the pitch at a note's start or end.

```
culebra play --sounds kauai/sounds/alto_sax song.kau
```

```
band Smooth {
  voice Lead  external Alto  plays melody  vol 60
}
```

## Files

- `main.cul`: the model. `voices` gives the song the `Alto` instrument as a phrase
  (`hears`: `len`, `level` and `marks`).
- `songs/breeze.kau`: "Coast Road", a fusion tune in D (126 BPM, straight
  sixteenths, a room's echo): an intro, a verse on IVmaj7-III7-vi7, a chorus on
  IVmaj7-V/IV-iii7-vi7 with a harmony line under the alto, the chorus again a
  half step up, and an outro on bVI-bVII-I.

## Sources

Every sound is computed by `Audio.partials`, `Audio.filter` and the envelopes in
`main.cul`. The spectral envelope, the breath and the attack are measured with
`kauai/tools/measure.cul` from two alto saxophone recordings in the IBCC Digital
Archive (University of Lincoln, Bob Burns collection, dated 2022, "Alto
saxophone Jazz. One" and "Two"), licensed under CC BY-NC 4.0
(<https://creativecommons.org/licenses/by-nc/4.0/>). The recordings name no
performer or tune, and the archive publishes them as is. Only the measured
numbers are in this package, not the recordings.

## How close it is

Measured with the same tool on a scale of held notes, against the recordings:
the envelope is within about 3 dB up to 6 kHz; the breath is within 2 dB from
2 to 4 kHz and from 8 to 16 kHz, and about 10 dB under it from 0.5 to 1 kHz,
where the recordings' fast lines put the changes between notes into the
measure as well.
