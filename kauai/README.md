# Kauai

Kauai is Culebra's music language: a song is a lead sheet and the grooves a
band plays it with, and `culebra play song.kau` plays it. This folder holds
what goes beyond the built-in instruments. Find what you want to do below.

## I want to write a song

Read the language specification, which is the reference for the whole
language (draft, with a Japanese version):

- [`docs/kauai/language.md`](../docs/kauai/language.md), or in Japanese
  [`docs/kauai/language.ja.md`](../docs/kauai/language.ja.md)
- [`docs/guides/kauai-songs.md`](../docs/guides/kauai-songs.md): a walkthrough
  of writing a song, with the English and Japanese versions beside it

Its section 2 is a first song. Play it with `culebra play song.kau`, or write
it to a WAV file with `culebra play --wav out.wav song.kau`.

## I want to play a song with a sound

- **Built-in sounds**: the sounds the language has (`pulse2`,
  `triangle`, `saw` and the rest) need nothing else. See the language
  specification's section on voices.
- **External sounds**: an instrument made from recordings, given to a song as
  `external NAME`. Today there are two:

  | Instrument | `external` | What it is |
  |---|---|---|
  | [`sounds/soprano_sax`](sounds/soprano_sax) | `SopranoSax` | a soprano saxophone, from royalty-free recordings |
  | [`sounds/alto_sax`](sounds/alto_sax) | `AltoSax` | an alto saxophone, from a jazz player's improvisations (CC0 and CC BY) |

  Play a song with one: `culebra play --sounds kauai/sounds/soprano_sax song.kau`.
- **Recipes**: synthesized instruments written as data, a synth brass, a synth
  string section and an electric piano, with a `bright` knob, and a drum kit
  (`Kick`, `Snare`, `CrossStick`, three hi-hats, two cymbals, toms). Play a
  song with them: `culebra play --sounds kauai/recipes song.kau`.
  [`recipe.md`](recipe.md) lists them and says how to write one.
- [`docs/guides/kauai-sounds.md`](../docs/guides/kauai-sounds.md): the sounds a
  song plays, shaping them, supplying an `external` instrument, a model, and a
  recorded instrument made with the harness.
  [`engine.md`](engine.md) lists the voice parameters (`breath`, `wander`, `vibrato` and five more).

## I want to make a new instrument

Making an instrument from recordings is a long piece of work: finding and
checking the recordings, measuring them, setting the style, and judging the
result by ear. Start with the route:

- [`making.md`](making.md): the route, in order, with the rules that matter.
- [`skills/make-instrument`](skills/make-instrument): the step-by-step work for
  Claude to follow with you.
- [`harness`](harness): the same work done by agents on their own, judged by a
  distance, with you deciding the last word.

## I want to know how an instrument plays, or to measure one

- [`engine.md`](engine.md): how a phrase is played from recorded notes, what
  each style key does, and the limits.
- [`tools/README.md`](tools/README.md): each measuring, checking, comparing and
  listening tool, with its options.

## I want a command line that needs no Culebra

`just kauai-build` compiles [`cli/main.cul`](cli/main.cul) into one binary,
`build/kauai`, and `just kauai-install [PREFIX]` puts it in `PREFIX/bin`.
Running it needs nothing else: it reads no module at run time.

```bash
kauai song.kau                      # play it
kauai song.kau --check              # read and check it, play nothing
kauai song.kau --wav out.wav        # write it to a WAV file, no device needed
kauai song.kau --wav out.wav --from Chorus --to 1:30
```

It takes the options `culebra play` takes, except `--sounds` and `--watch`.
The instruments in `sounds/` (`SopranoSax`, `AltoSax`), the recipes in
`recipes/` (`FmBrass`, `FmStrings` and the rest) and the drums in
`recipes/drums/` (`Kick`, `Snare` and the rest) are compiled in, and a song
names them as `external SopranoSax` or `drum Kick external Kick`;
`kauai instruments` lists them. An
instrument of your own is added to `cli/main.cul` and the binary built again,
because a compiled program loads no module of its own at run time.

## I want a score to print or edit

```bash
kauai convert song.kau score.musicxml   # or culebra kauai/cli/main.cul convert ...
```

[`convert/musicxml.cul`](convert/musicxml.cul) writes the song as a MusicXML
4.0 score, which MuseScore, Dorico, Sibelius and the browser libraries
(OpenSheetMusicDisplay, Verovio) open. The melody is the first staff and
each part the song writes out another; the chords are chord symbols over the
melody, with the quality as written for their text. The notes keep their
ties, tuplets, grace notes, slurs, rolled chords and marks (the jazz
articulations too), and the dynamics, hairpins, tempo marks, swing, meters
and keys are where the song writes them. A `%` is drawn as a repeat sign.

The band follows: a staff for each voice that plays its grooves, echoes and
harmonies, with the notes it plays, spelled from the chord they are in (else
from the key), and a drum set staff for the drums it hits. A note is cut
where each beat starts unless it starts on one, and a beat whose notes are
off a 128th's grid is a tuplet. The drums Kauai's songs name (Kick, Snare,
Rim, Hat, Open or OpenHat, Pedal, Ride, Crash, Tom, Floor) stand where drum
set notation puts them, with their General MIDI sounds; any other drum takes
a free line or space.

The score is the song in the order it plays: each time it plays a section,
named by a rehearsal mark, with `in` spelled in the new key. Times through a
section that are all the same, band and all, are written once in repeat
signs. Times that differ only toward their end have first and second endings
from the bar where they start to differ (or from the section's own endings,
where those come first), as long as at least half of the section's body
stays in the repeat signs; any other time is written out. A pickup that
starts the song is a short first bar, and a later one goes into the rests at
the end of the bar before it.

The band's staves have the dynamics and hairpins of the bar lines, which the
whole band plays at, each drawn where it falls and placed before the first
note it reaches. A section played `with echo` or `with harmony` says so in
small italics where it starts, and a song that loops says `loop` after its
last note. A `\rit N` or `\accel N` is a rit. or accel.

What a score has no place for (the band, its grooves and voicings, a line's
parameters such as `\bright=`, the hits, the sections and the order the song
plays them in, `mark` and `room`, the tempo a rit. reaches) goes with it as the
song's Kauai text, in a `<miscellaneous-field>` named `kauai-song`, which
MuseScore keeps when it saves the score again. Writing a compressed `.mxl` is
to come.
[`convert/musicxml-4.0/`](convert/musicxml-4.0/) holds the schema the tests
check every score against.

```bash
kauai convert score.musicxml song.kau   # .musicxml, .xml or .mxl
```

[`convert/musicxml_read.cul`](convert/musicxml_read.cul) reads a score back as
a song. Each voice of each part is a line: the first part's first voice is the
melody, every other a part, and the song gets a band that plays them (single
notes on the chip channels while they last, chords on `external Ep`). The
staves a Kauai band played are left out. Rehearsal marks, repeat signs,
endings, D.S., D.C., To Coda and Fine make the sections and the order the
song plays them in. A pickup the song starts with plays again where the
score goes back to it (a D.C., a repeat from the start) or writes it again
at the end of the bar before (a first ending, the last bar of a repeat),
and not where it does neither. What the score holds that Kauai does not play
is an error (a 64th, a trill, a 7:3 tuplet); what Kauai leaves out that
plays the same (lyrics, words, a chord it spells another way) is said, and
so is a slur Kauai has to end where a section ends, and a pickup a repeat
plays again where the bar before it does not rest. The tests read the
scores in [`convert/scores/`](convert/scores/): a selection of the MusicXML
test suite, and the OpenScore *Je te veux*, which plays as
`tests/kauai/jetveux.expect` says.

A score Kauai wrote reads back as the song it carries, with its band,
grooves, voicings, sections and lines. Each measure is set beside the score
Kauai writes for that song: where the score shows the same, the measure is
the song's own bar; where it shows otherwise (a note or a chord changed in
MuseScore), it is the score's, written for the time through its section it
is in (`2nd:`, or `1st:` where the other times play as the song does), in
the section's key where `in` moved it, with the song's parameters and the
tempo its rit.s reach. A time through a section that no longer fits it
(another rehearsal mark, measure count, meter, key or swing, or a note in
the measure before it held over where its pickup starts) is a section of its
own as the score shows it, one for the times it shows the same, named by its
rehearsal mark where that changed; the song keeps its key. The band's staves
are not read: the band plays from the chords and the grooves, and the
measures where its staves show otherwise are said. What no longer fits is
said and left out: a band that does not play every part of the score, or
without which the song reads where it does not with it, gives way to one
made for its lines, as for a score Kauai did not write; an order of sections
the score changes, to the sections and the order the score shows; and a
song text that does not read, to the score alone.

## I want to play an instrument from a MIDI controller

```bash
kauai live FmBrass                  # from the one MIDI input there is
kauai live FmBrass --midi EWI       # from the input whose name has EWI
kauai live FmBrass --breath pressure  # the breath from the channel's pressure
```

[`cli/live.cul`](cli/live.cul) plays an instrument as a wind controller plays
it, one note at a time. The breath (CC 2 unless `--breath` says CC 7, CC 11
or the channel's pressure) is the volume, in proportion: half the breath
is half the amplitude, 6 dB under full; how hard to blow for full is the
controller's own breath gain. It plays 6 dB over the instrument's own
level (`--gain` says how many), so that it stands over a song played
alongside, and a limit at its output (`Audio.Graph`'s `limit`) keeps the
loudest breath from clipping. A note played while another is held moves to the new
pitch without starting over, and a note blown harder is brighter: a
recipe's breath turns its `bright` knob. A saxophone plays each note as a
song's phrase would, slurred from the note before
([`engine/borrow_live.cul`](engine/borrow_live.cul)): the breath a note
starts with is the loudness of the recording it borrows, and from then on
dims or brightens it. Its first 0.4 s are worked out as the note is played
(about 6 ms), the rest by a worker thread. Live, a saxophone has no vibrato,
scoop or recorded attack of its own, only the player's. A recipe whose
sound fades by itself, as `FmElectricPiano`, strikes every note anew.
Through a laptop's built-in speakers a note sounds more than 10 ms later
than through a headphone jack.

## Folders

| Folder | What it holds |
|---|---|
| `engine/` | the playing code instruments from recordings share, and the interpreter of recipes |
| `recipes/` | the instruments written as recipes, one JSON file each, and the drums in `recipes/drums/` |
| `tools/` | the tools for measuring, checking and comparing |
| `cli/` | the command line `just kauai-build` compiles |
| `convert/` | a song as a score (MusicXML) and back to `.kau` |
| `sounds/` | the finished external instruments, one folder each |
| `skills/make-instrument/` | the skill for making an instrument with Claude |
| `harness/` | the agents' roles and the `main.cul` template, for building with one job to an agent |
