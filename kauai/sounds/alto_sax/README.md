# Alto saxophone (synthesized)

An alto saxophone made from a model of the sound, with no recordings in it. Its
numbers are measured from a jazz alto (see Sources):

- the harmonics follow the instrument's spectral envelope in Hz (flat to about
  400 Hz, rises near 1.35 kHz and 2.3 kHz, falling to -32 dB at 4.3 kHz), so the
  timbre changes with the pitch as a real instrument's does, and brightens with
  the dynamic;
- a breath of noise sits under the tone at the level measured in each band, with
  a burst of it at a tongued start;
- a tongued note rises in about 12 ms;
- a long note's vibrato is small (about 3 cents at its widest) and comes in late.

A slur is one phrase: the sound goes on through it, the pitch changes in a few
milliseconds as a change of fingering does (a glide is a siren), with a brief dip
of the air, and only a gap or the phrase's end lets the sound go. `\scoop`,
`\plop` and `\doit` bend the pitch at a note's start or end, and the timbre is
drawn again every 20 ms while it is bent, from the pitch it has reached, so the
resonances stay where they are in Hz. `\fall` takes the course in
`engine/bend.cul`: the pitch gathers speed (nine semitones in 0.3 s) while the
loudness falls from the start and thins out softly, and the tone dims into
breath as it goes.

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
- `songs/seaside_drive.kau`: "Seaside Drive", a bright fusion tune in A (132
  BPM, a sixteenth-note pocket): an intro, a verse, a bridge that builds, a
  chorus on a motif that comes back, an interlude, and a last chorus.
- `songs/sunset_shore.kau`: "Sunset Shore", a ballad in F (66 BPM): long
  phrases, maj9 and m9 chords, soft drums, a bridge that rises and a last
  note that falls away.
- `songs/overdrive.kau`: "Overdrive", a driving E minor tune (154 BPM): a
  riff, a chorus that climbs, a quiet bridge, and a rock backbeat.

The melodies stay inside A4 to G#5 (the alto package's data and the
synthesized model's best range). The drums share one noise channel, so the
kick, the snare, the hats and the crash sit on different sixteenths.

## Sources

Every sound is computed by `Audio.partials`, `Audio.filter` and the envelopes in
`main.cul`. The spectral envelope and the breath are measured with
`kauai/tools/measure.cul` from the seven solo alto improvisations by
DieRechnug on Freesound that `kauai/sounds/alto_sax_recorded` plays from (one
CC0, six CC BY 4.0; that package's README lists them). Only the measured
numbers are in this package, not the recordings.

## How close it is

Measured with the same tool on every semitone from D3 to A5 held at mf, against
the recordings: the envelope is within about 3.5 dB from 150 Hz to 10 kHz, and
the breath within 1 dB in each band from 0.5 to 4 kHz and from 8 to 16 kHz, and
3.4 dB over it from 4 to 8 kHz.
