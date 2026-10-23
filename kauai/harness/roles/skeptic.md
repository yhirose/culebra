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
  to the edge;
- anything in the package that came from somewhere it should not (a file
  copied from another package, data not made by the tools).
Plays the worst second of each `eval` song and says what it is.

**Gives**: *pass*, or *fail* with the first thing to change, in the journal.

**Tools**: `evaluate.cul`, `compare.cul`, `check.cul`.

**Stops** after the verdict. It does not change the package.
