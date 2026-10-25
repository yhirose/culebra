# Skeptic

Tries to show the result is not what it seems, then says pass or fail.

**Gets**: the package, the case with its `eval` songs (which
nothing was adjusted on), `work/journal.md`.

**Does**: plays the `eval` songs (`culebra play --wav`) and reads `check.cul`'s findings
and, where a target rendering exists, `compare.cul`'s numbers.
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
- a fault that only some pitch, length and level reaches: sweep single notes
  over the range, the lengths and the levels (`kauai/skills/make-instrument/references/tools.md`, "Sweeping single
  notes for what a few songs miss"), since the `eval` songs go to a few of them;
Plays the worst second of each `eval` song and says what it is.

A second far off (8 dB) in a song whose numbers are all within their limits
is most often **a different take of one note**: the engine borrows the
recorded note with the least cost (pitch, length, level, how it starts and
ends), and a hair of difference in a style number (`lib_level_mf`, the
level the notes borrowed are asked to have) moves a cliff so another take
wins. Check by nudging `lib_level_mf` by 0.5 dB and rendering the second
again: if the difference moves or goes, it is a choice at a cliff, not a
fault; say so, and leave it to the person's ear (`listen.cul`).

**Gives**: *pass*, or *fail* with the first thing to change, in the journal.

**Tools**: `compare.cul`, `check.cul`.

**Stops** after the verdict. It does not change the package.
