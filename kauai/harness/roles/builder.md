# Builder

Makes the instrument's package and finds its style.

**Gets**: `work/measured/`, `work/sources/`, the case (`name`, `voice`,
`range`, and `tune` with its targets, if it has them), `kauai/harness/template/main.cul`, and the judge's notes
from earlier rounds.

**Does**:
1. Makes `kauai/sounds/<name>/data/instrument.json` with a `make`:
   `measure` ({low, high}), `left_out` ({brighter 9.0, odd_under 12.0} to
   begin with), `breath_noise` ([20.0, 20000.0]).
2. Runs `kauai/tools/make_data.cul --sounds kauai/sounds/<name> --sources
   work/sources work/measured/*.json`, which writes `data/` beside it.
3. Copies the template to the package as `main.cul`, with the voice's name.
4. With targets, runs `kauai/tools/fit_style.cul --sounds
   kauai/sounds/<name> --voice <name in the songs> --tune SONG=TARGET ...
   --log work/fit.json`, which reads a first `play` off the recordings and
   moves the numbers they cannot say until the `tune` songs are as close to
   their targets as they get. **Reads its warnings**: "the songs ask for
   nothing above N" and "X is heard by none of the songs" mean the songs
   cannot decide that number; says so in the journal, and asks the composer
   for a song that goes there, rather than leaving the guess. Without
   targets, `fit_style.cul` has nothing to fit to: writes `play` by hand
   (the alto's came from the soprano's `play`, with the numbers that depend on
   the recordings read off them) and says in the package's README that it is
   not fitted.
5. Changes **one** thing the judge named (`left_out`, a pitch range, a
   style number), and does 2 and 4 again as it needs.

**Gives**: the package, and `work/journal.md` lines: what was changed, the
cost after it.

**Tools**: the tools above, `kauai/tools/compare.cul` to look.

**Stops** when the judge says the distance has stopped falling, or after the
rounds it was given.
