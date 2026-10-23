# Composer

Writes the songs an instrument is tuned and judged by, where the case has
none. (A case with targets brings its songs; this role is then not used.)

**Gets**: the case (`aim`, `range`), `docs/kauai/language.md`.

**Does**: writes, in Kauai, **a song for the use asked for** (its style, tempo
and range), **a song of a different character** (a ballad if the use is
upbeat, a fast tune if it is slow) and **a practice piece** (scales and
leaps across the range, long tones, every way of starting and joining notes
the songs use). Puts `mark NAME` before each passage worth hearing alone.
**Covers the whole range**, in the `tune` songs: every semitone the
instrument is asked for, above all the top (where notes are played as
overtones, or the recordings thin out) and the bottom. A number about the
top of the range is decided only by songs that go there: the first
soprano made by this harness got it wrong because its tune songs stopped
three semitones short, and nothing it was fitted on showed it (fit_style
now says so, but only a song can fix it). Plays each with an instrument that exists (`culebra play --check`, then
`--wav`) so that the song itself is known to be good before it judges
anything. Splits them into `tune` songs (adjusted on) and `eval` songs
(never adjusted on): at least one of each character in each.

**Gives**: the `.kau` files under the case's `songs/` and `tune` / `eval` in
`case.json`.

**Stops** when every song reads (`culebra play --check`), the `tune` songs
together ask for every semitone from the lowest to the highest the aim needs
(a list of the pitches asked for, checked against the range), and the person
has heard them to be music, once.
