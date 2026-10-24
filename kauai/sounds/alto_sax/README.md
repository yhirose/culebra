# Alto saxophone (synthesized)

An alto saxophone made from a model of the sound, with no recordings: a harmonic
tone whose spectrum follows a measured alto's (the first ten harmonics as
measured, then falling) and brightens with the loudness; a breath of band noise;
a slow vibrato on the long notes; and a glide into each note. A slur is one
phrase, so its notes run into one another.

The spectrum and the vibrato depth were set from the IBCC Digital Archive's alto
recordings (see Sources), by measuring the harmonics of their long notes.

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
  (`hears`: `len` and `level`).
- `songs/breeze.kau`: a fusion-style tune in B-flat (108 BPM, a Head, a Lift in
  E-flat, and the Head again), with the alto on the melody over ii-V-I changes, a
  saw bass, an arpeggiated triangle, and a sixteenth-note pocket of noise drums.

## Sources

Every sound is computed by `Audio.partials`, `Audio.Sound` and the envelopes in
`main.cul`. The harmonic levels and the vibrato depth are measured from two
alto saxophone recordings in the IBCC Digital Archive (University of Lincoln,
Bob Burns collection, dated 2022, "Alto saxophone Jazz. One" and "Two"),
licensed under CC BY-NC 4.0 (<https://creativecommons.org/licenses/by-nc/4.0/>).
The recordings name no performer or tune, and the archive publishes them as is.
Only the measured numbers are in this package, not the recordings.
