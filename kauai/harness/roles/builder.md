# Builder

Makes the instrument's package and finds its style.

**Gets**: `work/measured/`, `work/sources/`, the case (`name`, `voice`,
`range`, `tune`), `kauai/harness/template/main.cul`, and the judge's notes
from earlier rounds.

**Does**:
1. Makes `kauai/sounds/<name>/data/instrument.json` with a `make`:
   `measure` ({low, high}), `left_out` ({brighter 9.0, odd_under 12.0} to
   begin with), `breath_noise` ([20.0, 20000.0]).
2. Runs `kauai/tools/make_data.cul --sounds kauai/sounds/<name> --sources
   work/sources work/measured/*.json`, which writes `data/` beside it.
3. Copies the template to the package as `main.cul`, with the voice's name.
4. Runs `kauai/tools/fit_style.cul --sounds kauai/sounds/<name> --measured
   work/measured --tune SONG=TARGET ... --log work/fit.json`, which reads a
   first `play` off the recordings and moves the numbers they cannot say
   until the `tune` songs are as close to their targets as they get.
5. Changes **one** thing the judge named (`left_out`, a pitch range, a
   style number), and does 2 and 4 again as it needs.

**Gives**: the package, and `work/journal.md` lines: what was changed, the
cost after it.

**Tools**: the tools above, `kauai/tools/compare.cul` to look.

**Stops** when the judge says the distance has stopped falling, or after the
rounds it was given.
