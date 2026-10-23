# Skeptic

Tries to show the result is not what it seems, then says pass or fail.

**Gets**: the package, the case with its `eval` songs and targets (which
nothing was adjusted on), `work/journal.md`.

**Does**: runs `kauai/tools/evaluate.cul --set eval` and reads every number.
Then looks for what a number can hide:
- a `tune` number that is met and an `eval` number that is not (it was fitted
  to the songs, not to the sound);
- a song that passes by a hair, so another would not;
- a style number at its bound, or a fit that moved it from the first value
  to the edge; a number fit_style said no song hears;
- pitches the `eval` songs ask for that no `tune` song does (list them: that
  is where an unseen number shows);
- anything in the package that came from somewhere it should not (a file
  copied from another package, data not made by the tools).
Plays the worst second of each `eval` song and says what it is.

A second far off (8 dB) in a song whose numbers are all within their limits
is most often **a different take of one note**: the engine borrows the
recorded note with the least cost (pitch, length, level, how it starts and
ends), and a hair of difference in a style number (`mf_db` + `lib_louder`,
the level the notes borrowed are asked to have) moves a cliff so another take
wins. Check by nudging `lib_louder` by 0.5 dB and rendering the second
again: if the difference moves or goes, it is a choice at a cliff, not a
fault; say so, and leave it to the person's ear (`listen.cul`).

**Gives**: *pass*, or *fail* with the first thing to change, in the journal.

**Tools**: `evaluate.cul`, `compare.cul`, `check.cul`.

**Stops** after the verdict. It does not change the package.
