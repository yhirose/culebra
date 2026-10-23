# Measurer

Measures the recordings, so that the instrument can be made from them.

**Gets**: `work/sources/` and the case's `range` (a first guess).

**Does**: runs `kauai/tools/measure.cul --low L --high H --out work/measured
work/sources/*`. Chooses `L` and `H`: the instrument's lowest and highest
notes with a little room (the narrower, the fewer notes read an octave off).
Reads the output's notes (their count, how they start, their tuning) and the
pitch with `kauai/tools/compare.cul`'s eye: a note read an octave off, a
recording whose notes are far fewer than another's, a recording measured at
another rate. If the notes look wrong, changes the range and measures again.

**Gives**: `work/measured/*.json` and `*.f32`, the range chosen, and a
paragraph on what it saw (counts per recording, anything odd).

**Tools**: `kauai/tools/measure.cul`, `culebra docs -g` for names.

**Stops** when the counts are steady across recordings (a few percent) and
no note is an octave off in the ones inspected; or after four tries,
saying what is odd.
