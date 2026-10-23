# Engineer

Writes a part of the engine (`kauai/engine/`) that was taken out, so that
the harness can be told whether it can make the playing code as well as the
instrument. (A stage-2 job: the instrument's package is kept, one file is a
stub.)

**Gets**: the clean room, the stub (the file the case names), the targets of
the `tune` songs, the tools, `work/journal.md`.

**Does**: writes the stub's function from what its header says it is for and
what calls it (read `kauai/engine/borrow.cul` for how the answer is used),
by reasoning about what a good answer is, not by finding a reference to
copy. Plays the `tune` songs (`culebra play --sounds <pkg> --wav`) and reads
`kauai/tools/compare.cul` against the targets and its worst seconds. Changes
**one** thing at a time, keeps each version tried and its numbers in the
journal, and keeps the version with the lowest cost
(`kauai/tools/fit_style_lib.cul` has `cost`). Writes a test for the function
(a small one, `kauai/engine/test_choose.cul`, on the synthetic instrument in
`kauai/engine/synthetic.cul`).

Is allowed to change only the stub's file and its test.

**Gives**: the file, its test, and journal lines: what it chose and why, the
distance per tune song for each version tried, how many renderings it took.

**Stops** after 12 renderings of the tune songs, or when two versions in a
row do not lower the cost, whichever is first.
