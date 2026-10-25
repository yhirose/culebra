# The engine

`engine/` is the playing code external instruments made from recordings
share. An instrument is its recordings (`data/`, laid out as
[`engine/data_format.cul`](engine/data_format.cul) says) and a style. The
engine's entry point is `Instrument.new(dir, style)`, where `dir` is the
`data/` folder, and `phrase_samples(phrase, params)` voices a phrase.

## How a phrase is played

1. **Each note borrows a recorded note.** The one chosen is the nearest in
   pitch, length, loudness, how it is entered and left, and how much vibrato
   it has. [`engine/choose.cul`](engine/choose.cul) holds that choice. If no
   recording is within reach, the nearest one is borrowed.
2. **The borrowed note keeps its own moves.** Its pitch and loudness curves
   and its timbre, frame by frame, are played as harmonics with the breath
   between them. Its attack is kept for a note that starts a phrase or is
   tongued.
3. **A note longer than its borrowed note** is filled out with steady stretches
   of long notes near it. With `slow_kept` under 1 (the alto's 0.35), most of
   each long note's slow loudness and timbre is taken out of what it borrowed,
   so the pieces do not swell and sag; what the score writes on the note (its
   level, a crescendo, a bend) is put over it afterwards, whole.
4. **A slur joins two takes** with a 20 ms crossfade of timbre, so the join
   does not click. The step between the two (in loudness, timbre and breath)
   is taken up half by each side, read where each side stands at its level
   (`join_reads`): a dip right at the join, a recorded onset or release, is
   the borrowed note's own move and stays. Read across a dip, the step was
   the dip's depth, and half of it went on the frames beside it, a note 14 dB
   over what was asked.
5. **A bend keeps the resonances.** `\scoop`, `\plop`, `\doit` and `\fall` bend
   the pitch of the borrowed note. The instrument's resonances do not move with
   it, so each harmonic takes the level the borrowed tone had at its own
   frequency (`reshaped` in `engine/borrow.cul`); moved by harmonic number, a
   fall of nine semitones dragged the resonances down with it and went dull.
   A bend that changes the tone's level is the breath at work: the harmonics
   brighten with a push and dim with a loss (`dimmed`), the breath is louder
   against the tone either way (`BEND.breath_db`), and the pitch shakes the
   further it has been bent (`bend_shake`).
6. **A fall has a course of its own** (`engine/bend.cul`): the pitch gathers speed (slow at first, nine semitones
   down in 0.3 s), while the loudness is greatest at the start, falls at once
   and thins out softly to nothing. The two are not on one curve; when they
   slid down together the fall sounded like a wind controller's glide. A fall
   shaped as a jazz alto's measured falls are (a hard push on landing, then a
   collapse to breath) was heard as a blunt "buh" and given up: the
   measure says what changes (the tone dims and goes to breath), not how hard.

## The style

The style is an object, the `play` in `data/instrument.json`. Its keys:

| Key | What it sets |
|---|---|
| `mf_db` | a note's loudness in dB at mf (A4) |
| `db_per_octave` | how much louder a note is an octave up |
| `lib_level_mf` | the level (dB, against each recording's loudest) a note at mf asks to borrow |
| `vibrato` | `{depth, straight}`: vibrato of a long note in cents, and the share of its length held straight first |
| `onset_from_t0` | `{rest, tongue}`: where a recorded note's onset is, in seconds from its first frame |
| `overtone_from` | the lowest note played as an overtone of the fingering an octave down (0 for none) |

These six are required. The rest have defaults (`DEFAULTS` in
[`engine/borrow.cul`](engine/borrow.cul)): `scoop`, `own_kept`, `drift_kept`,
`gain_range`, `onset_at`, `min_lead`, `wander_style`, `slow_kept` and `choice`.

`choice` is an object of weights for `choose.cul`. The defaults are in
`CHOICE` there. The ones to know:

- `pitch`, `reach`: the cost of a semitone off, and the furthest a recording may be
- `squeeze`, `stretch`: the cost of a recording much longer or shorter than the note
- `level_db`: the cost of loudness that differs from the one asked
- `slur_other`, `slur_none`, `tongue_slur`, `rest_tongue`: the cost of a join that does not match the note's entry and exit
- `same_take`: a bonus for the take that followed the previous borrowed note in the recording, so one player sounds through a slur
- `crest_from`, `crest_db`: the cost of a note whose loudest frame stands far over its level (a puff of breath at its start), since the loudness asked is set by its middle
- `edge_from`, `edge_db`: for a slur in or out, the cost of a recording that dips far at its start or its end (one that began after a break, or faded); the dip stays in the note and is heard as a gap in the slur, while a slur's own small dip (up to 10 dB) costs nothing
- `crisp` (0, off): the cost of a start that is not crisp (frames to come within 3 dB of its level, and how round its attack is), for a note not asked to begin softly. The soprano's soft start, its "poa", is its own and stays off; the alto's tongued notes begin crisply (`1`)

The weights were fitted on the soprano's targets. Changing them moves every
song.

## Voice parameters

These are set on the band's line (`external Sax breath=0.5`) or in the score
(`\vibrato=12`):

| Parameter | Meaning |
|---|---|
| `breath` | the breath heard with the tone: 0 none, 1 as recorded |
| `wander` | a slow wander of pitch and loudness: 0 none |
| `vibrato` | the vibrato a long note asks for, in cents at its deepest |

## What the engine does not do yet

- **A slur whose notes come from different takes can break at the join.** The
  crossfade hides a click, not a change of player. A written slur is not kept
  as a phrase: Kauai folds the melody into the instrument's range, so the
  written intervals and the played ones differ.
- **A long note is stretched only so far.** Past that, the middle is filled from
  other notes, and the seam can show.
- **The numbers do not hear a join.** `compare` sees a jump in level or
  pitch; whether the join sounds natural is for the ear.
