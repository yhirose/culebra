# Scores

MusicXML scores for testing the MusicXML importer. Every file is as
fetched, byte for byte; only `jetveux.mxl` is renamed.

## jetveux.mxl

*Je te veux* (Erik Satie, 1897; words by Henry Pacory, English text by
Robert Hess) for voice and piano, transcribed by markpearse for
[OpenScore Lieder](https://musescore.com/openscore-lieder-corpus). It is
`scores/Satie,_Erik/_/Je_te_veux/lc6986302.mxl` of
[OpenScore/Lieder](https://github.com/OpenScore/Lieder) at commit
`38c5db510224d9facdc4b08d741fc788cfb58ea8`, a MusicXML 4.0 export from
MuseScore 4.5.2 (score page: <https://musescore.com/score/6986302>).

It is the score `tests/kauai/jetveux.expect` was written from. Its voice
part (`P1`, voice 1; voices 2 and 3 are the octave alternatives in bars
34-35 and 40-41), with tied notes joined and the repeats played as bars
1-37, 47-78, 6-35, 38-39, 79-110, 6-35, 40-46 (170 bars), gives the
expectation's 273 notes, each with the same pitch, start and length.

The OpenScore Lieder scores are released under
[CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/)
(the repository's `LICENSE.txt`; the score's `<rights>` reads "OpenScore
(CC0)"). CC0 asks for no notice; OpenScore asks that public uses credit
OpenScore Lieder and link to the corpus, as above.

## kainhofer/

31 files of Reinhold Kainhofer's *Unofficial MusicXML test suite*. Each
checks one part of MusicXML and describes itself in
`<miscellaneous-field name="description">`. They are from
`input/regression/musicxml/` of [LilyPond](https://gitlab.com/lilypond/lilypond)
at tag `v2.26.0` (commit `3596756be0fac1f81066a99d2054329212172b2d`).
LilyPond is under the GPL, but that folder's `LICENSE` puts the suite
under the MIT license, whose notice follows:

```text
An (unofficial) MusicXML Test Suite

This test suite of MusicXML unit tests is licenced under the MIT license:

Copyright (c) 2008--2026, Reinhold Kainhofer

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

What each file tests:

- `01a-Pitches-Pitches.xml`: every pitch from G to c'''', plain, sharp and flat, then naturals, double sharps and flats, cautionary and editorial accidentals
- `02a-Rests-Durations.xml`: rests of every length, plain and dotted, a two-bar multi-measure rest, a whole-bar rest with `measure="no"`
- `03a-Rhythm-Durations.xml`: note values from long to 1024th, plain, dotted and double-dotted
- `03b-Rhythm-Backup.xml`: a `<backup>` to beat 2 rather than to the bar's start, for a second voice
- `03c-Rhythm-DivisionChange.xml`: `<divisions>` changing mid-bar, from 1 to 8 to 38
- `11a-TimeSignatures.xml`: a new time signature in each bar, from 2/2 to 12/8
- `13a-KeySignatures.xml`: key signatures from 11 flats to 11 sharps, major and minor, a new one in each bar
- `21a-Chord-Basic.xml`: one two-note chord (`<chord/>`) and a rest
- `21f-Chord-ElementInBetween.xml`: `<direction>` elements between the notes of a chord
- `23a-Tuplets.xml`: tuplets 3:2, 4:2, 4:1, 7:3 and 6:2, one without a `number` attribute
- `23d-Tuplets-Nested.xml`: tuplets inside tuplets, with a `<divisions>` change
- `24a-GraceNotes.xml`: grace notes single and beamed, slashed and not, with accidentals, one right before a bar line
- `31b-Directions-Order.xml`: a tempo word, a metronome mark, a dynamic and a diminuendo wedge in successive `<direction>` elements with `<offset>`
- `31c-MetronomeMarks.xml`: metronome marks with dotted beat units, note-equals-note marks, and parentheses
- `32c-MultipleNotationChildren.xml`: a staccato and an accent written three ways in `<notations>` and `<articulations>`
- `32d-Arpeggio.xml`: arpeggiated chords (`<arpeggiate>` plain, up and down, and `<non-arpeggiate>`), labelled with lyrics
- `33b-Spanners-Tie.xml`: two whole notes tied across a bar line
- `33c-Spanners-Slurs.xml`: a note that ends one slur and starts another, nested slurs, a slur from one voice to another
- `41a-MultiParts-Partorder.xml`: four parts, `P0` to `P3`
- `42a-MultiVoice-TwoVoicesOnStaff-Lyrics.xml`: two voices on one staff, each with lyrics
- `43a-PianoStaff.xml`: one part on two staves, a voice on each
- `45a-SimpleRepeat.xml`: one bar repeated with `times="5"`, the repeat starting implicitly at the beginning
- `45b-RepeatWithAlternatives.xml`: a repeat with first and second endings
- `45d-Repeats-MultipleEndings.xml`: eight endings, some for several passes (`number="3, 5, 7"`), the last discontinued
- `45e-Repeats-Combination.xml`: repeats back to back, endings with `number=""`, a second ending that starts a repeat
- `46d-PickupMeasure-ImplicitMeasures.xml`: a 3/8 pickup bar 0 and bars split by `implicit="yes"` measures
- `46g-PickupMeasure-Chordnames-FiguredBass.xml`: a pickup bar with chord symbols (`<harmony>`) and figured bass
- `61a-Lyrics.xml`: lyrics with each `<syllabic>` value and notes without lyrics
- `71a-Chordnames.xml`: chord symbols with altered roots, added and altered degrees
- `71f-AllChordTypes.xml`: 38 chord symbols covering every `<kind>` but `none`, each over one note, labelled with lyrics
- `71g-MultipleChordnames.xml`: several `<harmony>` elements during one note
