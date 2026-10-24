# Alto saxophone (from recordings)

An alto saxophone played from real notes. Each note of a phrase borrows the
recorded note nearest to it (in pitch, length, loudness, how it is entered and
left, and how much vibrato it has), keeps that note's own moves of pitch and
loudness and its timbre, and sounds them as harmonics with the breath between.

```
culebra play --sounds kauai/sounds/alto_sax_recorded song.kau
```

```
band Smooth {
  voice Lead  external Alto  plays melody  vol 60
}
```

## Sources

- **Michael Giles, alto saxophone, November 1997**, recorded in an anechoic
  chamber for the University of Iowa Electronic Music Studios' Musical
  Instrument Samples collection (<https://theremin.music.uiowa.edu/MISaltosaxophone.html>).
  The collection's recordings are freely available for any project, without
  restrictions, and its maintainer asks users to let him know how they are used.
  Three files are used: the mf (no vibrato) recordings at D♭3–B3, C4–B4 and
  C5–A♭5, each a run of single notes.

## Limits

The samples are single notes, each a separate take, so the playing has no
recorded slurs and no recorded tongued attacks. A slur borrows each note from
a different take, which the engine joins with a crossfade; expect a join to
sound less like one player than in the soprano package. The loudness reference
is set from the soprano package's relation, not fitted to the alto's targets.

## Files

- `main.cul`: the playing (the shared engine, with this package's data).
- `data/`: the notes, attacks and breath made from the recordings, and
  `instrument.json` with the style (`play`) and the measuring and making
  settings (`make`).
- `work/` (not in git): the downloaded recordings, the WAV conversions and the
  measurements.
