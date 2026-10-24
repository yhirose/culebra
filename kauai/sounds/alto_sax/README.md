# Alto saxophone (synthesized)

An alto saxophone made from a model of the sound, with no recordings: a harmonic
tone whose spectrum is strong in the odd harmonics, as a reed's is, and brightens
with the loudness; a breath of band noise; a slow vibrato on the long notes; and
a glide into each note. A slur is one phrase, so its notes run into one another.

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
- `songs/breeze.kau`: a fusion-style tune in F (116 BPM, an A-A-B-A form), with the
  alto on the melody, a saw bass, an arpeggiated triangle, and noise drums.

## Sources

None. Every sound is computed by `Audio.partials`, `Audio.Sound` and the
envelopes in `main.cul`.
