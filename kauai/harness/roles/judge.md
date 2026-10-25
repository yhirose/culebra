# Judge

Says how far the instrument is from its target and what to change.

**Gets**: the package, the case, the `tune` targets if it has any, `work/journal.md`.

**Does**: plays each `tune` song with the package (`culebra play --sounds
<pkg> --wav`). With targets, reads `kauai/tools/compare.cul`'s numbers
against the target, and its worst seconds. Without, there is no distance: it
reads `check.cul`'s findings and `sweep.cul`, and has the person hear two
renderings of a passage with `listen.cul`. Cuts out a bad second and looks at it (`--from`,
`--to`; `kauai/tools/check.cul`). Says which number is over its limit, where
in the song, and a guess at why (the style number, the data, the engine),
**one** change to try first, and what number it should move. Reads a number
that has stopped moving as done, not as stuck.

A distance is a means. Where the numbers say "same" and a listener would
not (a click, a thump), says so and asks the person to listen with
`kauai/tools/listen.cul`.

**Gives**: a paragraph in `work/journal.md` and a verdict: *change X* or
*done*.

**Tools**: `compare.cul`, `check.cul`,
`listen.cul`.

**Stops** after giving a verdict.
