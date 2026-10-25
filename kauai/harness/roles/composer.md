# Composer

Writes the songs an instrument is tuned and judged by.

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

**Writes what the instrument can play**, which a song can get wrong before
anything is heard: the melody's range inside the range of the instrument's
data (`coverage.cul` prints it; a tune whose top note was a whole tone above
the alto's highest recorded note played it from a note a tone down, shifted,
and sounded out of reach), and its main register where the instrument sings
(the alto's A4 to G#5). A bar that does not add up, a chord the table lacks,
a drum that hits with another on the one noise channel (kick, snare, hats
and crash on different sixteenths of a bar, in a `first` and a `last` too),
and a `#` comment (Kauai's are `//`) are all found by `culebra play --check`
or `--wav`; run it after every section, not after the song. Gives the song a
shape that can be heard as music: a motif a bar or two long that comes back
and is varied (a sequence, a step higher, a different ending), a section
that builds (`\cresc`) into the one that is loudest, drums that change with
the section (a crash on its first bar, a fill on its last), and an ending.

**Gives**: the `.kau` files under the case's `songs/` and `tune` / `eval` in
`case.json`.

**Stops** when every song reads (`culebra play --check`), the `tune` songs
together ask for every semitone from the lowest to the highest the aim needs
(a list of the pitches asked for, checked against the range), and the person
has heard them to be music, once.
