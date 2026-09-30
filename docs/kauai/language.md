# Kauai Language Specification

> **Status: Draft.** Kauai is implemented in Culebra as `Audio.Kauai`; the
> language and its API may still change before a release. The songs in
> [Appendix C](#appendix-c-complete-songs) were checked note for note
> against the game they came from and against the score.

Kauai is a language for writing music as a band plays it: a lead sheet of
chords and melody, and grooves that say how the band accompanies them. A
Kauai file is plain text with the extension `.kau`. A program written in
Culebra plays it through `Audio.Kauai` ([section 10](#10-playing-from-culebra)).

The language is named after Kauaʻi, an island of Hawaiʻi, as Culebra is named
after an island of Puerto Rico. In code and file names it is written `kauai`
and `.kau`.

## Table of contents

1. [Overview](#1-overview)
2. [A first song](#2-a-first-song)
3. [Names and words](#3-names-and-words)
4. [Files](#4-files)
5. [Bands](#5-bands)
6. [Grooves](#6-grooves)
7. [Sections](#7-sections)
8. [Songs](#8-songs)
9. [Errors](#9-errors)
10. [Playing from Culebra](#10-playing-from-culebra)
11. [Appendix A: Chord qualities](#appendix-a-chord-qualities)
12. [Appendix B: Grammar](#appendix-b-grammar)
13. [Appendix C: Complete songs](#appendix-c-complete-songs)
14. [Appendix D: Habits that do not carry over](#appendix-d-habits-that-do-not-carry-over)

## 1. Overview

A song is built from four kinds of definition:

| Definition | What it holds |
|---|---|
| `band` | the instruments (voices) and the drum kit |
| `groove` | one or more bars of accompaniment, written against the chord rather than as notes |
| `section` | a lead sheet: one line per bar, its chords and its melody, and any parts written out |
| `song` | the tempo, the meter and the key, and the sections in the order they play |

The design follows these rules.

1. **A line is a bar.** In a section, each line is one bar: its chords, then
   its notes. The one line of two bars is `%%`, as a chart's two-bar repeat
   sign spans two.
2. **Write what a musician writes.** Chord symbols, note lengths as note
   values (`c4.` is a dotted quarter), marks on notes where a score puts them
   (`.c8` is staccato), and dynamics and tempo where they change (`\cresc`,
   `\rit 104`).
3. **Write a thing once.** A groove is shared by every bar that plays it, a
   chord carries on until the next one, and a repeat in another key or with
   another ending is one line of the song.
4. **An edit stays where it is made.** Changing a note changes that note:
   octaves come from a fixed range, not from the note before, and a length
   left out carries on only within its own bar.
5. **Stop a mistake where it is written.** Every error in
   [section 9](#9-errors) is found before the song plays, and names its line.
6. **Expansion is deterministic.** A song expands to the same notes every
   time; nothing is chosen at random.

## 2. A first song

```kauai
band Duo {
  voice Tune  pulse     plays melody            vol 20
  voice Bass  triangle  plays roots in E2..D#3  vol 30
}

song First {
  tempo 100
  band Duo
  groove Walk
  melody in C5..B5

  Verse x2
}

groove Walk {
  Bass  1 - 5 -
}

section Verse {
  C     e4 g c'2
        d'4 c' g2
  F G   a4 f g d
  C     c1
}
```

Each line of the section is a bar: chords first, then notes. The second line
names no chord, so it keeps `C`, and `F G` shares its bar between two chords.
`c'` and `d'` are an octave above the range `C5..B5`. The groove plays under
every bar: the bass takes each chord's root (`1`), holds it (`-`), and then
its fifth (`5`).

## 3. Names and words

**Names and keywords.** A word that starts with a capital letter is a name
the song defines: `Duo`, `Tune`, `Verse`, `Walk`. A word in lower case is a
keyword of the language: `band`, `voice`, `plays`, `groove`. What musicians
write in capitals is written so here too: chord symbols (`Fmaj7`), pitches
and keys (`C4`, `Eb`) and intervals (`M3`, `P5`). Each appears only where
that kind of thing is expected.

**Lines and blocks.** Kauai is read a line at a time. A line that ends with
`{` opens a block, and a line that is only `}` closes it. Words are separated
by spaces; indentation has no meaning.

**Comments.** A `//` at the start of a line, or after a space, starts a
comment that runs to the end of the line. A `//` inside a word is not one
(`https://`), and a `#` is always a sharp.

**Pitches in ranges.** Where a definition names a pitch (`E2..D#3`,
`from C4`), it is written with a capital letter, an optional `#` or `b`,
and an octave number. `C4` is middle C (MIDI 60). In a note line the same
letters in lower case are notes and the number after them a length: `e4` is
a quarter-note E, and `E4` the pitch.

**Chord symbols.** A root (a capital letter and an optional `#` or `b`), a
quality from the chord table ([Appendix A](#appendix-a-chord-qualities)), and
an optional bass note after a `/`: `Fmaj7`, `Bm7b5`, `C/D`, `Gmaj7/A`. A
quality may be spelt as charts spell it (`CM7`, `C△7`, `C-7`, `Cø`, `C°7`,
`C+`), and tensions follow in parentheses, comma separated, as chord charts
write them: `C7(9,13)`, `Cm7(11)`, `C7(b9,#11)`
([Tensions](#tensions)). `♭` and `♯` may stand for `b` and `#`. A `/`
before a capital letter names the bass; one before a digit belongs to the
quality (`C6/9`). The root takes a `#` or `b` that follows its letter, so
`Cb9` is C flat's ninth. `N.C.` means no chord.

## 4. Files

A file that plays holds exactly one `song`. A file without one is a library:
its bands, grooves and sections are there to be used by other files.

**`use 'file'`** brings in another file's definitions, as if they were
written at that point. The path is quoted and relative to the file that says
`use`. A used file may not hold a `song`.

**`voicings { ... }`** adds chord qualities to the table in
[Appendix A](#appendix-a-chord-qualities), one a line: the quality's name and
its intervals in semitones from the root. A name may not start with `#` or
`b`, which would read as the root's sharp or flat, and may not be a quality
or a spelling the table already has.

```kauai
voicings {
  7b5     0 4 6 10
  9sus4   0 5 7 10 14
}
```

**`about { ... }`** says what the song is: who wrote it, when, and where
it comes from. Each line is an item and its text, which runs to the end of
the line without quotes. Each item is written at most once, and none is
required. Only a file with a `song` may have one, and at most one.

```kauai
about {
  title     Je te veux
  composer  Erik Satie
  year      1897
  source    the OpenScore Lieder score (CC0)
  note      the voice as written; the chords are a reading of the piano part
}
```

| Item | What it says |
|---|---|
| `title` | the song's title |
| `composer` | who wrote the music |
| `lyricist` | who wrote the words |
| `arranger` | who arranged it or wrote it down in Kauai |
| `year` | the year it was written, a number |
| `source` | where it was taken from: a score, a recording |
| `license` | the terms it may be used under |
| `note` | anything else, on one line |

Nothing in `about` changes how the song plays; a program reads it with
`about()` ([section 10](#10-playing-from-culebra)).

Two definitions of the same kind with the same name (two grooves, two
sections), in one file or across files, are an error. A groove and a section
may share a name.

## 5. Bands

```kauai
band Run {
  voice Lead    pulse     plays melody            duty 1/4  vol 15  env 1 2 4  gap 2
  voice Second  pulse2                            duty 1/8  vol 9   env 1 2 6  gap 2
  voice Bass    saw       plays roots in E2..D#3            vol 18  env 0 3 3  gap 3
  voice Arp     triangle  plays chords from C4              vol 7   env 1 4 6  gap 4

  Second echoes Lead      late 3/16  level 70%
  Second harmonizes Lead  under m3

  drum Kick   noise 120       len 1  vol 20  env 0 3 2
  drum Tom    noise 900->500  len 3  vol 12  env 0 0 4
}
```

### Voices

`voice NAME SOURCE [plays ...] OPTIONS...`

The source is one of [`Audio.tone`](../stdlib.md#tones)'s channels, `pulse`,
`pulse2`, `triangle` or `saw`, or `host NAME` for an instrument the program
supplies ([section 10](#10-playing-from-culebra)). A channel plays one note at
a time, and two voices of a band may not share one; the drums take the
`noise` channel.

**What a voice plays** decides how its lines are read:

| Words | The voice plays |
|---|---|
| `plays melody` | the sections' melody; exactly one voice of a band plays it |
| `plays roots in LO..HI` | a bass line in chord degrees ([Bass rows](#bass-rows)); `LO..HI` is one octave, where each chord's bass note is placed |
| `plays chords from NOTE` | places in the chord ([Chord rows](#chord-rows)), voiced from `NOTE` up; `plays chords rootless from NOTE` leaves out the root |
| `plays part NAME...` | the parts named (one or more: `plays part Upper Lower`) where a section writes them ([Parts](#parts)), instead of its groove row in those bars; it may come with another `plays` |
| (no `plays`) | only what it echoes or harmonizes ([Derived parts](#derived-parts)) |

A chord is voiced from its quality's intervals: the first tone is the lowest
of its pitch at or above `NOTE`, and each tone after it the lowest of its
pitch above the one before. With `plays chords from C4`, `Fmaj7` is F4 A4 C5
E5; with `plays chords rootless from E3`, `Abmaj9` is C4 Eb4 G4 Bb4.

| Option | Meaning | Default |
|---|---|---|
| `duty 1/8` `1/4` `1/2` `3/4` | the pulse wave's duty cycle | `1/2` |
| `vol N` | the level at `mf`: `0` to `100` as in `Audio.tone`; for a host voice, a percentage of the instrument's own level (`vol 46%`) | (required) |
| `env A D R` | attack, decay and release, in ticks of 1/60 second | `0 0 0` |
| `gap N` | ticks cut from the end of every note, so that notes are heard apart | `0` |
| `poly` | a host voice that sounds several notes at once | one note at a time |

A host voice plays each note's sound through to its end; `env`, `gap` and
the note's length do not apply to it. It may carry parameters of its own as
`name=value` words after its name (`host Ep bright=0.8`); Kauai hands them to
the host with every note.

### Derived parts

A band line that starts with a voice's name and goes on with `echoes` or
`harmonizes` gives that voice a part made from another voice's notes. A
song chooses which one plays, section by section ([section 8](#8-songs)).

| Line | The part voice `V` plays |
|---|---|
| `V echoes SOURCE late 3/16 level 70%` | SOURCE's notes, a length later (here three sixteenths), at a percentage of V's `vol` |
| `V harmonizes SOURCE under m3` | for each SOURCE note, the highest tone of the chord under it (in any octave) at least the interval below it, for as long |

Intervals are written `m2 M2 m3 M3 P4 TT P5 m6 M6 m7 M7 P8`. An echo reads
its source across section boundaries, and in a looping song the song's first
notes echo its last.

### Drums

`drum NAME noise FREQ[->END] len N vol V [env A D R] [plays hits]`
`drum NAME host SOUND [name=value...] vol P% [plays hits]`

A noise drum is a hit on the noise channel: `FREQ` in Hz, sliding to `END`
if given, held for `len` ticks. The channel sounds one drum at a time, so two
noise drums may not hit at the same moment.

A host drum plays a sound the program supplies
([section 10](#10-playing-from-culebra)) through to its end, at a percentage
of the sound's own level. Host drums hit together, with each other and with
a noise drum, as the drums of a kit do:

```kauai
band Combo {
  voice Keys  host Ep bright=0.8  plays chords rootless from E3  poly  vol 40%
  voice Bass  triangle            plays roots in E2..D#3            vol 30

  drum Kick   host Kick   vol 80%
  drum Ride   host Ride   vol 50%
  drum Hat    host Hat    vol 40%
}
```

A drum that `plays hits` strikes on a section's hits; the others rest
through them ([Hits and breaks](#hits-and-breaks)).

## 6. Grooves

```kauai
groove Hook {
  Bass   1 - 8 -  1 8 - 1  - 8 1 -  5 - > -
  Arp    4 3  2 1  2 3  4 3
  Kick   x... ...x x... ....
  Hat    ..x. ..x. ..x. ..x.
  Snare  .... x... .... x...
  first {
    Kick   .... ...x x... ....
    Crash  x... .... .... ....
  }
  last {
    Snare  .... x... ...x xx..
    Floor  .... .... .... ..xx
  }
}
```

A groove is a row for each voice or drum that accompanies, named by it. A
row is a grid: a bar of `n` tokens gives each token an `n`-th of the bar, so
16 tokens in 4/4 are sixteenths and 8 are eighths, and a groove fits a bar of
any meter. Each token has to come to a note value, plain, dotted or a
triplet's: 12 in 4/4 are eighth triplets, and 15 is an error. `|` starts the row's next bar; a row of two bars repeats every
two. A groove's bars are counted from the first bar of the section, or from
the `groove` line that names it, and each row goes through its own bars, so
a row of one bar plays in every bar under a row of two. Every row reads `-` as
holding the note before and `.` as silence. A voice with no row in a groove
is silent while it plays.

### Bass rows

The voice that `plays roots` reads chord degrees:

| Token | The note |
|---|---|
| `1` | the chord's bass (the note after `/`, else the root), placed in `LO..HI` |
| `3` `5` `7` `9` | that degree as the chord has it (the `5` of `Bm7b5` is F), at the lowest pitch at or above the `1` |
| `8` | the `1` an octave up |
| `>` / `<` | a semitone below / above the next chord's `1`: `>` leads up into it, `<` down |

Each token reads the chord sounding at its time, so in a bar of `C F` the
first half's tokens read C and the second half's F. The next chord is the
first chord of the next bar the song plays; in the last bar of a song that
does not loop there is none, and `>` and `<` play the chord's own `1`. A
chord the chord table does not give a degree takes a default: 4, 7, 10 and
14 semitones.

### Chord rows

The voice that `plays chords` reads places in its voicing, counted from the
bottom: `1 2 3 ...` (a number past the top is the top tone), or `x` for the
whole chord at once (a `poly` voice).

### Drum rows

A drum's row is `x` for a hit and `.` for none, one character a step; spaces
between them are ignored.

### First and last bars

`first { ... }` and `last { ... }` hold rows that replace the same rows in the
first and the last bar of each section the groove plays under (every repeat
of `xN` counts). They are how a section opens with a crash or closes with a
fill.

## 7. Sections

```kauai
section Refrain {
  pickup  \pp (e g
  Cadd9         \tempo d'2.
  C/G           c2 e'4
  Cmaj7         b2.
  C6/G          a2.)
  G7 G7 C/E     \cresc (a b c \!
  ending 1 {
    C           c,2.~
    D7 D7 C/E   c,) (b c)
  }
  ending 2 {
    C           c,2.~
    D7 D7 C/E   c,) (b c)
  }
}
```

`section NAME [key KEY] { ... }` holds bar lines, and these lines:

| Line | Meaning |
|---|---|
| `pickup NOTES` | notes that lead into the section, sounding over the last beats of the bar played before it; that bar has to rest there ([Pickups](#pickups)) |
| `groove NAME` | the groove for the bars after it |
| `meter N/D` | the meter for the bars after it ([Meter changes](#meter-changes)) |
| `melody in LO..HI` | the range for this section's notes ([Octaves](#octaves)) |
| `NAME: NOTES` | under a bar, the notes part `NAME` plays in it ([Parts](#parts)) |
| `%` / `%%` | the bar before again, or the two bars before ([Repeat signs](#repeat-signs)) |
| `hits: x8 r ...` | under a bar, the rhythm the band plays in it in place of its groove ([Hits and breaks](#hits-and-breaks)) |
| `2nd: NOTES` | under a bar, the bar as it plays the second time ([Later times](#later-times)) |
| `part NAME { ... }` | part `NAME` written out, a line a bar ([Parts](#parts)) |
| `part NAME in LO..HI` | the range for part `NAME`'s notes |
| `swing 8` / `straight` | the swing for the bars after it ([Swing](#swing)); `straight` plays them as written |
| `ending N { ... }` | bars that close the section, a different set for each time it is played; `ending N as written { ... }` is not moved by the song's `in` |

`key KEY` says what key the section is written in, which the song's `in`
moves it from; it defaults to the song's key. A section played without `in`
sounds as written, whatever the song's key.

### Bar lines

A bar line is the bar's chords, then its notes. The chords are the words at
the start that begin with a capital letter, or `N.C.`; they share the bar
equally, so their number divides the bar's beats. A `/` among them is a beat
slash, as on a chart: a beat more of the chord before it, so in 4/4
`C / / G7` is three beats of C and one of G7. With slashes, the bar has a
chord or a `/` for each of its beats, and a bar that starts with a `/` goes
on with the chord sounding. A bar that names no chord keeps the chords of the
bar before it; the first bar of a section and of an ending names its chords.
`N.C.` silences the voices that play roots and chords, and the drums play on.

A bar of chords alone, with no notes, is a bar the melody rests in, as a
chart's bars of slashes are:

```kauai
section Vamp {
  Dm7 / G7 /
  Dm7 / G7 /
  Cmaj7         e4 d c2
}
```

### Repeat signs

A line of `%` alone is the bar before it again, as a chart's repeat sign
is: its chords, its notes, and what is written under it (its parts, its hits
and its `2nd:` lines). `%%` is the two bars before it again, and is two
bars. A repeat takes its own place in the groove, the meter and the swing;
a bar that differs is written out, so nothing goes under a `%`.

```kauai
section Riff {
  Dm7           d8 f a c' r2
  G7            b4 g r2
  %%
  Cmaj7         e1
}
```

### Notes

| Written | Meaning |
|---|---|
| `c` `d` `e` `f` `g` `a` `b` | a note, always in lower case |
| `#` / `b` after it | sharp / flat: `f#`, `eb`, and `bb` is B flat (`##` / `bb` double: `f##`, `ebb`, and B double flat `bbb`) |
| `'` / `,` after that | an octave above / below its range: `e'`, `c,` |
| `1 2 4 8 16 32` | whole, half, quarter, eighth, sixteenth, thirty-second |
| `.` after the value | dotted (`..` double-dotted): `g4.` |
| (no value) | the value of the note before it in the same bar, dots and all; a bar's first note is one beat |
| `~` at the end | tied to the next note, which is the same pitch |
| `r` | a rest, with a value like a note: `r4`, `r2.` |
| `3[c8 d e]` | a tuplet: the number is how many it holds |
| `2:3[c8 d]` | a tuplet with its ratio: two in the time of three |
| `<c e g>4` | a chord: its notes sound together, as one note does |
| `{d}c4` | a grace note, before the note it leads into |

A note is exactly what its name says. The key adds no sharps or flats, so in
`key D` the F sharp is written `f#` and `f` is F natural, and an accidental
belongs to its own note, not to the rest of the bar: `f# g f` is F sharp, G,
F natural.

A tuplet fits into the longest plain value (whole, half, quarter, ...) no
longer than what is written in it: `3[c8 d e]` is three eighths in a
quarter, `3[c4 d e]` three quarters in a half, and `5[c16 d e f g]` five
sixteenths in a quarter. The number is written against the `[`, so
`\tempo 3[c8 d e]` goes back to the song's tempo and then plays a tuplet.

A tuplet the rule does not fit is written with its ratio, `p:q`, as a score
prints it: its notes play in `q/p` of their written length, `p` of them in
the time of `q`. In 6/8, `2:3[c8 d]` is two eighths in the time of three,
and `4:6[c8 d e f]` four in the time of six. The ratio counts note values,
not notes, so a tuplet of mixed values takes one too: `3:2[c4 d8]` is a
quarter and an eighth in the time of a quarter. `3[c8 d e]` is `3:2[c8 d e]`
written short.

A chord is one note with several pitches. Its length goes after the `>`, a
mark before the `<` (`.<c e g>8`), and each of its notes takes its own
accidental and octave marks (`<c e' g>`). A `~` after the `>` ties the whole
chord; a `~` after a note inside ties that note alone, so part of a chord can
be held while the rest changes: `<g,~ b~ e g~>2.` then `<g, b d g>4` holds G,
B and G and plays D new. Notes that sound together need a `poly` voice. When
the melody holds chords, the derived parts follow its top note.

Grace notes go in braces before the note they lead into, `{d}c4` or
`{e d}c4`. Each takes a sixty-fourth from the start of that note, which
sounds after them for the rest of its length. They take accidentals and
octave marks, and no length or marks of their own; the note after them may
be a chord, and may not be a rest or a note held by a tie.

### Octaves

A note without `'` or `,` takes its letter's place inside the range: the
section's `melody in`, else the song's. The range is one octave from a
natural note to the note a semitone below its octave (`melody in F5..E6`,
`melody in D4..C#5`), so each letter's natural falls in it once, and a sharp
or flat then moves the note from there, as on a staff: in `C4..B4`, `cb` is
just below `c` (B3) and `b#` just above `b` (C5). Each `'` moves a note an
octave up and each `,` an octave down. A note does not depend on the note
before it, so changing one changes nothing else. A range that starts near the
tune's lowest note keeps the marks few.

### Marks on notes

A mark goes before the note's name; a note's length goes after it.

| Written | Mark | How it sounds |
|---|---|---|
| `.c8` | staccato | half its length |
| `!c4` | accent | a level louder |
| `-c4` | tenuto | its full length |
| `^c4` | marcato | a level louder and three quarters of its length |
| `~<c e g>2` | arpeggio | the chord rolled: its notes start a sixty-fourth apart, from the bottom up, and end together |
| `(c8 d e f)` | slur | each note joined to the next; the last one is not |
| `@c2` | fermata | held: its time plays twice as long, and the whole band holds with it |

Marks may be combined: `.!c8` is a staccato accent. A rest takes no marks but
a fermata (`@r2`, a pause for the whole band), and a note that a tie holds on,
which is not struck again, takes none. A fermata counts its written length
in the bar.

A slur runs from its `(` to its `)` in the order the song plays, across bars
and sections, so one slur may start in a pickup and end bars later. It is one
line at a time: a `(` while a slur is open, a `)` with none open, and a slur
still open when the song ends are errors. An ending goes on from its
section's body, so a slur the body leaves open is closed in every ending,
though a score draws it closing only in the first:

```kauai
section Tag {
  C             (e4 d c2~
  ending 1 {
    G7          c1)
  }
  ending 2 {
    G7          c2) r
  }
}
```
The `~` of an arpeggio goes right before the `<`, where a score draws its
wavy line; a `~` after a note is a tie, so `~<g, g>2.~` is a rolled chord
tied to the next. Notes held by a tie into a rolled chord are not struck
again, and the roll starts from the lowest note struck. Grace notes come
before the marks: `{d}.c8`.

### Directions

A word that starts with `\` is a direction, and takes effect from the note
after it.

| Written | Meaning |
|---|---|
| `\pp` `\p` `\mp` `\mf` `\f` `\ff` | the dynamic level: 0.4, 0.55, 0.7, 0.85, 1.0 and 1.15 times a voice's `vol`; a song starts at `mf` |
| `\cresc` / `\dim` | louder / softer, evenly in time, up to the next dynamic |
| `\!` | the `cresc` or `dim` ends here, a level louder or softer than where it began |
| `\tempo 114` | this tempo from here; `\tempo 4.=76` counts another value, here dotted quarters |
| `\rit 104` / `\accel 140` | slower / faster, evenly, reaching the tempo at the next tempo direction |
| `\tempo` | back to the song's tempo |

A dynamic in a bar line is the whole band's, as a chart's `mf` or `cresc.`
is read by everyone playing from it: the melody, the parts, the grooves, the
drums and the derived parts all play at that level, times their own `vol`, so
the band can drop for a verse and build into a chorus together. A dynamic in a
part's line sets that part alone, from where it stands until the next dynamic
in a bar line, which the part then follows again. A `cresc` or `dim` may run
on into the next section the song plays.

A `cresc` that ends at a dynamic ends with it, and needs no `\!`: from `p`,
`\cresc a b c \f d` rises through the `a`, `b` and `c` to `f` at the `d`. A
`\!` ends it one level from where it began, so from `p`,
`\cresc a b c \! \f d` rises only to `mp`, and the `d` jumps to `f`.

Tempo directions apply to the whole band. `\rit N` and `\accel N` change the
tempo evenly from where they are written to the next tempo direction the song
plays, which may be in a later section, and reach `N` there; that direction
then sets the tempo from its note on. With no tempo direction after it, the
song reaches `N` at its end.

### Parts

A part is a line of notes written out for an accompaniment: a fill, a lead-in,
a counter-melody, or each voice of a duet or a trio. A section names the part,
never an instrument; the band says who plays it with `plays part NAME`, as it
says who plays the melody with `plays melody`. In a bar where a part is
written, the voices that play it play its notes instead of their groove
rows; in any other bar they play their grooves.

A part is written in either of two ways. Under a bar, a line that starts with
the part's name and a colon holds its notes for that bar:

```kauai
section Intro {
  N.C.          r2.
    Fill:       r4 e g
  G9            r2.
}
```

Or a `part NAME { ... }` block in the section (or in an ending) holds a line
for each of its bars, in order, with `.` for a bar the part has nothing in:

```kauai
section Theme {
  C             e4 g c'2
    Alto:       c2 e
  G7            d'4 c' g2
    Alto:       b2 d
  C             e2 c

  part Cello {
    c1
    g,1
    .
  }
}
```

The two ways mean the same thing. The first keeps each bar's parts together,
as a score shows them one above another, and suits a part that comes and
goes; the second keeps a part together from start to end, as a player's part
does, and suits a voice that runs throughout. A part is written one way in
each section or ending, and a block has exactly as many lines as its bars.

A part's notes are written as a melody's are. Those without `'` or `,` are
in the part's range, `part NAME in LO..HI` in the section or the song, else
the melody's.

### Hits and breaks

A band leaves its groove to strike a rhythm together (hits), or to stop and
leave a bar to one player (a break). A `hits:` line under a bar gives that
rhythm: `x` where the band strikes and `r` where it rests, with values, dots,
ties and tuplets as notes have, and the marks `!` and `.`.

In that bar the band plays the hits in place of its groove:

| Who | On each hit |
|---|---|
| the voice that `plays roots` | the chord's bass note, the `1` of a bass row |
| the voice that `plays chords` | the chord, voiced as a chord row's `x`; its top note on a voice that is not `poly` |
| a drum that `plays hits` | a hit |
| the other drums | nothing: they rest through the bar |

A hit sounds for its value (`x4` a quarter), half of it under `.`, and a tie
holds it on (`x8~ x8`). Each hit takes the chord sounding at its moment, and
the band's dynamic; `!` makes it a level louder. The melody plays its notes,
and a part voice its part where the section writes one, as it does over a
groove.

A `hits:` line of rests alone is a break: the band is silent through the bar,
and the melody or a part plays on alone.

```kauai
section Kime {
  Dm7 G7          d'8 f' a' g' f'4 d'
  EbM7 F          !g'8. f'16 r8 !g' r !bb'4.
    hits:         !x8. x16 r8 !x r !x4.
  G7sus4          r4 d'8 e' f' g' a' b'
    hits:         !x4 r r2
  C               c'1
    hits:         r1
}
```

The drums that play hits are named in the band:

```kauai
band Fusion {
  voice Lead  saw                 plays melody                     vol 12
  voice Keys  host Ep  poly       plays chords from C4             vol 40%
  voice Bass  triangle            plays roots in E2..D#3           vol 30

  drum Kick   host Kick   vol 80%  plays hits
  drum Crash  host Crash  vol 60%  plays hits
  drum Hat    host Hat    vol 40%
}
```

`2nd hits:` gives the hits the second time, and `2nd hits: .` plays the
groove that time ([Later times](#later-times)). A drum fill in a break is a
groove of its own for that bar, rather than hits.

### Later times

A section the song plays more than once may differ in a bar or two from
one time to the next. A line under a bar that starts with `2nd:` is that bar
as it plays the second time the song plays the section; `3rd:` is the third
time, and so on. The times are counted over the whole song: each line of the
song that plays the section counts one, and `xN` counts `N`. A song that
loops counts its first time through only.

```kauai
section Verse {
  C             e4 g c'2
  G7            d'4 c' g2
  2nd:          d'4 b g2
  F G           a4 f g d
  C             c1
  2nd:          C7  c2 r
  F             f1
  2nd:          Fm
}
```

The line gives the bar's chords, its notes, or both. Without chords it keeps
the bar's, and without notes, as `2nd: Fm` above, it keeps the bar's notes
under new chords. It changes that bar alone: the bar after it keeps its own
chords, even when it names none. For a part, `2nd NAME:` under a bar gives part `NAME`'s
notes that time, and in a part block, a `2nd:` line under a line of the
block is that line the second time; it is not a bar of its own. The part
needs no notes of its own in the bar, and `.` leaves it out that time.
In the same way, `2nd hits:` gives the bar's hits that time, and
`2nd hits: .` none ([Hits and breaks](#hits-and-breaks)).

```kauai
section Theme {
  C             e4 g c'2
    Alto:       c2 e
    2nd Alto:   c2 g
  G7            d'4 c' g2

  part Cello {
    c1
    g,1
    2nd: b,1
  }
}
```

The time comes first, `2nd Alto:` rather than `Alto 2nd:`, so that the first
word of a line says what the line is.

### Pickups

A `pickup` line holds the notes that lead into a section's first bar. Its
first note without a value is a beat of that bar. The notes sound over the
last beats of the bar the song plays before the section, which has to rest
there; a section played `x2` takes its pickup the second time over its own
last bar.

When the section starts the song, the song starts with the pickup: a bar as
long as the pickup, before the first bar, which the band does not play in.
In a song that loops, the pickup plays again over the last beats of the
song's last bar, which has to rest there.

```kauai
section Opening {
  pickup  g8 a
  C             c2 e4 d
  G7            b,1
}
```

### Meter changes

A `meter N/D` line in a section (or an ending) changes the meter for the bars
after it, to the end of the section; the other sections keep the song's
meter. An ending starts in the meter its section's body ends in. The tempo
still counts quarter notes, a bar's first note without a value is a beat of
its own meter, and a groove's rows fit each bar as it comes.

```kauai
section Turn {
  C             e4 g c'2
  meter 3/4
  F             a2.
  G             g2 d4
  meter 4/4
  C             c1
}
```

## 8. Songs

```kauai
song JeTeVeux {
  tempo 124
  meter 3/4
  key C
  band Salon
  groove Valse
  melody in D4..C#5

  Intro
  Refrain  ending 1
  Couplet
  Refrain  ending 2
  Couplet
  Refrain  ending 3
  Coda
}
```

| Line | Meaning | Default |
|---|---|---|
| `tempo N` | quarter notes a minute, in any meter (a decimal is allowed); `tempo 4.=60` counts another value, here dotted quarters, as a 6/8 chart does | (required) |
| `meter N/D` | beats a bar, and the note value of a beat; a section can change it ([Meter changes](#meter-changes)) | `4/4` |
| `key K` | the key: a pitch name, with `m` for minor (`Eb`, `F#m`) | `C` |
| `band NAME` | the band that plays | (required) |
| `groove NAME` | the groove a section plays when it names none and has none of its own name | none |
| `melody in LO..HI` | the range for notes without `'` or `,` | (required if any section lacks one) |
| `part NAME in LO..HI` | the range for part `NAME`'s notes | the melody's |
| `swing 8` | pairs of eighths (`swing 16`: sixteenths) played long-short, 2:1 unless a ratio follows (`swing 8 3:2`) ([Swing](#swing)) | as written |
| `loop` | at the end, start again from the top | play once |
| `mark NAME` | a point the program can ask about ([section 10](#10-playing-from-culebra)) | |

Any other line plays a section, with these options, in any order and each
at most once (`Chorus with echo ending 2 in E`):

| Option | Meaning |
|---|---|
| `with echo` / `with harmony` | the derived part to play under it |
| `ending N` | the ending to play after its body |
| `in K` | play it in key `K`: moved from its key by the smaller interval, up to a tritone up |
| `xN` | play it `N` times |

A bar plays the groove its section names with `groove`; else the groove with
the section's own name; else the song's `groove`; else nothing.

### Swing

Notes are written straight, and swing is how they are played, as a chart
writes *Swing* over straight eighths. With `swing 8`, each pair of eighths,
counted from the start of the bar, plays long-short: the first in two thirds
of the pair's time and the second in the third that is left, as the eighths
of a triplet. `swing 8 3:2` gives another ratio, the long to the short, and
`swing 16` pairs sixteenths instead.

A time inside either half of a pair moves in proportion, so sixteenths under
`swing 8` swing with it and no note passes another. A tuplet keeps its notes
evenly spaced between where its start and its end are heard, so a triplet
on a beat is played as written. Swing moves everything the band plays: the
melody, the parts, and the grooves' rows and drums.

The song's `swing` holds for every section. A `swing` or `straight` line in
a section (or an ending) changes it for the bars after it:

```kauai
section Bridge {
  straight
  Dm7           d8 f a c' b a g f
  G7            e4 g2 r4
  swing 8 3:2
  Cmaj7         e8 g b d' c' b a g
}
```

## 9. Errors

A song is checked as a whole before it plays. Each error names the file and
the line; an error in a groove names the groove's row and the bar it plays
under.

- a bar whose notes are not as long as its meter says, or whose chords do not share its beats evenly (`C F G` in 4/4); a bar with slashes that has other than a chord or a `/` for each beat (`C / G7` in 4/4)
- a note, a length, a chord or a direction that does not parse; a note in capital letters reads as a chord, and is an error there; a chord quality the table does not have (`Cdim9`); a tension not in the table of [Tensions](#tensions) (`C7(10)`, `C7(b11)`), or one written twice
- a note both sharp and flat (`c#b`), or with more than two sharps or flats
- a mark other than a fermata on a rest, a mark on a note a tie holds on, a tie to a different note or into a rest, or a tie still open when the song ends
- a `(` while a slur is open, a `)` with none open, or a slur still open when the song ends
- a tuplet without a ratio that holds other than its number of notes, or a ratio that is not two counts above zero (`2:3`)
- a chord of one note, or notes sounding together on a voice that is not `poly`
- a part written both under its bars and in a block, a part block with other than its bars' number of lines, or a part line before any bar
- an arpeggio on a single note, grace notes before a rest or a tied note, or a note too short for its grace notes or its roll
- a `2nd:` line before any bar or line it could change, one with neither chords nor notes, a time the song never plays the section (`3rd:` under a section it plays twice), or a time spelt otherwise than `1st` `2nd` `3rd` `4th` ...
- a `hits:` line of other than `x` and `r`, not as long as its bar, before any bar, or written twice under one; `plays hits` on a voice
- the first bar of a section or an ending without chords
- a `%` or `%%` without the bars before it in its section or ending, or a line under a `%`
- a pickup over notes of the bar before it, or longer than that bar; at the start of a song that loops, a pickup over notes of the song's last bar
- a `\!` with no `cresc` or `dim` going, a `cresc` or `dim` still going when the song ends, or a `\rit` or `\accel` without its tempo (`\rit 104`)
- a tempo counted in what is no note value (`tempo 3=60`)
- a groove, voice, drum, section, band, ending or derived part that is not defined, or a name defined twice
- a groove row with a bar of no tokens, or of tokens that come to no note value (15 in 4/4)
- two noise drums hitting at the same moment, or two voices of a band on one channel
- a host drum without a sound, with a `len` or an `env`, or with a `vol` that is not a percentage
- no voice that plays melody, or more than one
- a range that is not one octave, or that starts on a sharp or a flat
- a `meter` whose beat is not a note value (`3/5`)
- `>` or `<` before `N.C.`
- an option said twice on a line of the song (`in D in E`)
- a `song` in a file that is used, or more than one in a file
- an `about` in a file that is used or twice in a file; an item it does not have, one written twice or without its text, or a `year` that is not a number
- a `voicings` name that starts with `#` or `b`, or that the chord table already has
- a `swing` on other than `8` or `16`, a ratio that is not long to short (`3:2`), or a swing whose pairs do not fill the bar (`swing 8` in 3/8)

## 10. Playing from Culebra

```culebra
let song = Audio.Kauai.new(`
band Duo {
  voice Tune  pulse     plays melody            vol 20
  voice Bass  triangle  plays roots in E2..D#3  vol 30
}

song First {
  tempo 100
  band Duo
  groove Walk
  melody in C5..B5

  Verse x2
}

groove Walk {
  Bass  1 - 5 -
}

section Verse {
  C     e4 g c'2
        d'4 c' g2
  F G   a4 f g d
  C     c1
}
`)
println(song.length())     # => 19.2
println(song.events()[0])  # => {at: 0.0, len: 0.6, by: 'Tune', pitch: 76, vol: 20.0}
song.play()
```

`Audio.Kauai.new(text)` takes a song as a string, which cannot `use` other
files. `Audio.Kauai.load(path)` reads a song file, and the files it names
with `use`, relative to it; with `dir:` it reads them from a directory
handle instead, an `Embed.dir` or any object with `exists(name)` and
`read(name)`, so a program built into one binary carries its songs. Both
check the song and raise the first error of [section 9](#9-errors) as a
`KauaiError` whose message names the file and the line
(`ballad.kau:12: ...`, or `line 12: ...` for a string).

| Method | What it does |
|---|---|
| `play()` | plays from the top (again, if it is playing), after building the host sounds `prepare()` has not |
| `stop()` | stops, silencing what it was playing |
| `volume(v)` | the level of the whole song, `0.0` to `1.0`, from its next note on |
| `playing()` | whether it is playing: from `play()` until `stop()`, or until its end if it does not `loop` |
| `reached(NAME)` | whether it has played as far as `mark NAME` |
| `prepare()` | builds one host sound, and answers how many remain |
| `length()` | seconds once through |
| `about()` | what its `about` says, as an object of the items written: `{title: 'Je te veux', composer: 'Erik Satie', year: 1897}`; `{}` without one |
| `events()` | what it plays, in order: `{at, len, by, pitch, vol}`, with `at` and `len` in seconds, `by` the voice or drum, `pitch` a MIDI note (`nil` for a drum), and `vol` the voice's `vol` at the note's dynamic (a host sound's as a fraction of its own level) |

The runtime schedules every note on the audio stream's own clock, so the
music keeps time through a long frame, and plays in a program with no frame
loop at all. A song keeps its own time as well: `playing()` and `reached()`
follow the clock from `play()`, whether or not a device plays it.

```culebra
# doctest: skip (reads ballad.kau, and electric_piano is the program's own)
let song = Audio.Kauai.load("ballad.kau", voices: {
  ep: |pitch, params| Audio.Sound(electric_piano(pitch, params.bright)),
})
song.prepare()          # build one host sound; answers how many remain
song.play()
song.reached("Drive")   # has the song passed `mark Drive`?
```

A host voice is built one pitch at a time: for each pitch the song needs, the
function given under `voices:` for the voice's host name is called with the
pitch, a MIDI note number (`60` is middle C), and the voice's `name=value`
parameters as an object, and returns the `Audio.Sound` to play. A host drum
is built once: the function given for its sound under `drums:`
(`drums: {kick: fn (params) { ... }}`) is called with the drum's
`name=value` parameters, and returns the `Audio.Sound` to play. The keys of
`voices:` and `drums:` are the host names in lower case: `host Ep` is `ep:`,
`host HiHat` is `hihat:`; a song whose host sound has no function is a
`ValueError` when it is read. `prepare()` builds one sound and answers how
many remain, so a game can spread the work over frames; `play()` builds
whatever is left first.

## Appendix A: Chord qualities

The table below is what `plays chords` voices and what degrees are read from.
A `voicings` block adds to it ([section 4](#4-files)). A quality may be
written in any of its spellings. Parentheses that hold numbers are tensions
on the quality before them ([Tensions](#tensions)), which give the table's
own chord where it has one (`C7(b9)` is `C7b9`); other parentheses are left
out before the table is looked up (`Cm(maj7)` is `Cmmaj7`).

| Quality | Written | Other spellings | Intervals |
|---|---|---|---|
| major | `C` | | 0 4 7 12 |
| minor | `Cm` | `C-` `Cmi` `Cmin` | 0 3 7 12 |
| diminished | `Cdim` | `C°` `Co` | 0 3 6 12 |
| augmented | `Caug` | `C+` | 0 4 8 12 |
| suspended second | `Csus2` | | 0 2 7 12 |
| suspended fourth | `Csus4` | `Csus` | 0 5 7 12 |
| sixth | `C6` | | 0 4 7 9 |
| sixth, added ninth | `C6/9` | `C69` | 0 4 7 9 14 |
| minor sixth | `Cm6` | `C-6` | 0 3 7 9 |
| added ninth | `Cadd9` | | 0 4 7 14 |
| minor, added ninth | `Cmadd9` | | 0 3 7 14 |
| dominant seventh | `C7` | | 0 4 7 10 |
| major seventh | `Cmaj7` | `CM7` `Cma7` `CΔ` `CΔ7` `C△` `C△7` | 0 4 7 11 |
| minor seventh | `Cm7` | `C-7` `Cmi7` `Cmin7` | 0 3 7 10 |
| minor, major seventh | `Cmmaj7` | `Cm(maj7)` `CmM7` `CmΔ7` `C-Δ7` | 0 3 7 11 |
| half-diminished | `Cm7b5` | `Cø` `Cø7` `C-7b5` `Cm7-5` | 0 3 6 10 |
| diminished seventh | `Cdim7` | `C°7` `Co7` | 0 3 6 9 |
| seventh, sharp fifth | `C7#5` | `C+7` `Caug7` | 0 4 8 10 |
| seventh, suspended fourth | `C7sus4` | `C7sus` | 0 5 7 10 |
| seventh, flat ninth | `C7b9` | `C7(b9)` | 0 4 7 10 13 |
| seventh, sharp ninth | `C7#9` | `C7(#9)` | 0 4 7 10 15 |
| seventh, sharp eleventh | `C7#11` | `C7(#11)` | 0 4 7 10 18 |
| altered | `C7alt` | | 0 4 10 15 20 |
| major seventh, sharp eleventh | `Cmaj7#11` | `Cmaj7(#11)` | 0 4 7 11 18 |
| ninth | `C9` | | 0 4 7 10 14 |
| major ninth | `Cmaj9` | `CM9` `Cma9` `CΔ9` `C△9` | 0 4 7 11 14 |
| minor ninth | `Cm9` | `C-9` | 0 3 7 10 14 |
| minor eleventh | `Cm11` | `C-11` | 0 3 7 10 14 17 |
| thirteenth | `C13` | | 0 4 7 10 14 21 |

### Tensions

After a quality, tensions in parentheses, comma separated, add notes to it, as
a chord chart writes them:

| Written | Adds | Semitones |
|---|---|---|
| `9` `b9` `#9` | the ninth, flat or sharp | 14, 13, 15 |
| `11` `#11` | the eleventh, sharp | 17, 18 |
| `13` `b13` | the thirteenth, flat | 21, 20 |
| `b5` `#5` | the fifth flat or sharp, in place of the fifth | 6, 8 |

Only what is written is added: `C7(13)` is C7 and the thirteenth, without
the ninth (`C7(9,13)` has both, as does `C13`). On a triad, whose row in the
table doubles the root an octave up, a tension takes the place of that
octave: `C(9)` is `Cadd9`, and `Cm(9)` is `Cmadd9`. Inside the parentheses `-`
and `+` may stand for flat and sharp (`C7(-9)`, `C7(+11)`); outside them a
`-` is still minor (`C-7`). A tension that is none of these (`C7(10)`, `C7(b11)`), one
written twice, or a plain `5` is an error.

```kauai
section Drive {
  CM7(9)            e4 g b d'
  Am7(11)           c'2. b4
  Dm7(9) G7(9,13)   a4 f e d
  C6(9) C7(b9,#11)  e2 g4 bb
}
```

The degrees a bass row reads are the chord's own: `3` is the interval of 3 to
5 semitones, `5` of 6 to 8, `7` of 9 to 11 and `9` of 13 to 15.

## Appendix B: Grammar

The grammar in PEG form. A file is read a line at a time; `EOL` ends a line,
and the pieces of a line are separated by spaces. Where the grammar allows
more than the language does (the number of beats in a bar, which names are
defined), [section 9](#9-errors) says what is checked.

```
File       <- (Use / Voicings / About / Band / Groove / Section / Song / EOL)*
Use        <- 'use' Quoted EOL
About      <- 'about' '{' EOL (AboutItem Text EOL / EOL)* '}' EOL
AboutItem  <- 'title' / 'composer' / 'lyricist' / 'arranger' / 'year'
            / 'source' / 'license' / 'note'

Voicings   <- 'voicings' '{' EOL (!('#' / 'b') Quality Int+ EOL)* '}' EOL
Band       <- 'band' Name '{' EOL (Voice / Drum / Derived / EOL)* '}' EOL
Voice      <- 'voice' Name Source VoiceWord* EOL
Source     <- 'pulse' / 'pulse2' / 'triangle' / 'saw' / 'host' Name Param*
Param      <- Word '=' Number
VoiceWord  <- 'plays' Plays / 'duty' Ratio / 'vol' Level / 'env' Int Int Int
            / 'gap' Int / 'poly'
Plays      <- 'melody' / 'roots' 'in' Range / 'chords' 'rootless'? 'from' Pitch
            / 'part' Name+
Derived    <- Name 'echoes' Name 'late' Ratio 'level' Percent EOL
            / Name 'harmonizes' Name 'under' Interval EOL
Drum       <- 'drum' Name ('noise' Int ('->' Int)? 'len' Int 'vol' Int
              ('env' Int Int Int)? / 'host' Name Param* 'vol' Percent)
              ('plays' 'hits')? EOL

Groove     <- 'groove' Name '{' EOL (Row / Edge / EOL)* '}' EOL
Edge       <- ('first' / 'last') '{' EOL (Row / EOL)* '}' EOL
Row        <- Name Cells ('|' Cells)* EOL
Cells      <- (Degree / Place / 'x' / '-' / '.' / DrumCells)+

Section    <- 'section' Name ('key' Key)? '{' EOL SectionLine* '}' EOL
SectionLine <- 'pickup' Notes EOL / 'groove' Name EOL / 'melody' 'in' Range EOL
            / 'part' Name 'in' Range EOL / PartBlock / Ending / Again / PartLine
            / Hits / Repeat / Feel / Meter / Bar / EOL
Ending     <- 'ending' Int ('as' 'written')? '{' EOL
              (Bar / Repeat / Again / PartLine / Hits / PartBlock / 'groove' Name EOL / Feel / Meter / EOL)* '}' EOL
Feel       <- ('swing' Swing / 'straight') EOL
Meter      <- 'meter' Int '/' Int EOL
Bar        <- (Chord / '/')+ Notes? EOL / Notes EOL     # '/': a beat more of the chord
PartLine   <- Name ':' Notes EOL
PartBlock  <- 'part' Name '{' EOL
              ((Notes / '.') EOL / Ordinal ':' (Notes / '.') EOL / EOL)* '}' EOL
Again      <- Ordinal ':' (Chord+ Notes? / Notes) EOL / Ordinal Name ':' (Notes / '.') EOL
            / Ordinal 'hits' ':' (HitNotes / '.') EOL
Hits       <- 'hits' ':' HitNotes EOL
Repeat     <- ('%%' / '%') EOL                    # the bar, or the two bars, before it again
HitNotes   <- (Int (':' Int)? '[' Hit+ ']' / Hit)+
Hit        <- ('.' / '!')* ('x' / 'r') Value? '~'?
Ordinal    <- Int ('st' / 'nd' / 'rd' / 'th')

Notes      <- (Direction / Tuplet / Slurred)+
Tuplet     <- Int (':' Int)? '[' (Direction / Slurred)+ ']'
Slurred    <- '('? Note ')'?
Note       <- Grace? Mark* ('~'? '<' ChordNote ChordNote+ '>' / Pitchname Octave* / 'r')
              Value? '~'?
Grace      <- '{' (Pitchname Octave*)+ '}'
ChordNote  <- Pitchname Octave* '~'?
Mark       <- '.' / '!' / '-' / '^' / '@'
Pitchname  <- [a-g] ('##' / '#' / 'bb' / 'b')?
Octave     <- "'" / ','
Value      <- ('1' / '2' / '4' / '8' / '16' / '32') '.'*
Direction  <- '\\' ('pp' / 'p' / 'mp' / 'mf' / 'f' / 'ff' / 'cresc' / 'dim'
              / 'rit' Tempo / 'accel' Tempo / 'tempo' (Tempo !'[')? / '!')
Tempo      <- (Value '=')? Number                 # quarter notes a minute, or Value's

Song       <- 'song' Name '{' EOL SongLine* '}' EOL
SongLine   <- 'tempo' Tempo / 'meter' Int '/' Int / 'key' Key / 'band' Name
            / 'groove' Name / 'melody' 'in' Range / 'part' Name 'in' Range
            / 'loop' / 'mark' Name / 'swing' Swing
            / Play / EOL
Swing      <- ('8' / '16') (Int ':' Int)?
Play       <- Name PlayOption* EOL          # each option at most once
PlayOption <- 'with' ('echo' / 'harmony') / 'ending' Int / 'in' Key / 'x' Int

Chord      <- 'N.C.' / ChordRoot Quality ('/' ChordRoot)?
ChordRoot  <- [A-G] ('#' / 'b' / '♯' / '♭')?
Root       <- [A-G] ('#' / 'b')?
Quality    <- ([a-zA-Z0-9#+°øΔ△♭♯()-] / ',' / '/' [0-9])*  # a quality of the chord table, then tensions
Pitch      <- Root Int
Range      <- Pitch '..' Pitch             # from a natural note, one octave
Key        <- Root 'm'?

Degree     <- [13579] / '8' / '>' / '<'
Place      <- Int
DrumCells  <- ('x' / '.')+

Name       <- [A-Z] [A-Za-z0-9]*
Word       <- [a-z] [a-z0-9]*
Int        <- [0-9]+
Number     <- [0-9]+ ('.' [0-9]+)?
Ratio      <- Int '/' Int
Percent    <- Int '%'
Level      <- Int / Percent
Interval   <- 'm2' / 'M2' / 'm3' / 'M3' / 'P4' / 'TT' / 'P5' / 'm6' / 'M6'
            / 'm7' / 'M7' / 'P8'
Quoted     <- "'" [^']* "'"
Text       <- (!EOL .)+                     # to the end of the line, no quotes
```

## Appendix C: Complete songs

Three songs for one game, two laps of a race and the ending theme, and a
song of Erik Satie's. The laps share a band through `use`.

### run.kau

The band both laps use.

```kauai
band Run {
  voice Lead    pulse     plays melody            duty 1/4  vol 15  env 1 2 4  gap 2
  voice Second  pulse2                            duty 1/8  vol 9   env 1 2 6  gap 2
  voice Bass    saw       plays roots in E2..D#3            vol 18  env 0 3 3  gap 3
  voice Arp     triangle  plays chords from C4              vol 7   env 1 4 6  gap 4

  Second echoes Lead      late 3/16  level 70%
  Second harmonizes Lead  under m3

  drum Kick   noise 120       len 1  vol 20  env 0 3 2
  drum Snare  noise 800       len 3  vol 13  env 0 2 2
  drum Hat    noise 3000      len 1  vol 5   env 0 0 1
  drum Open   noise 3000      len 3  vol 6   env 0 0 3
  drum Crash  noise 5000      len 4  vol 11  env 0 0 30
  drum Tom    noise 900->500  len 3  vol 12  env 0 0 4
  drum Floor  noise 500->250  len 3  vol 13  env 0 0 5
}
```

### cruise.kau

Lap one.

```kauai
about {
  title  Cruise
  note   lap one of a racing game: ~129 bpm in C
}

use 'run.kau'

song Cruise {
  tempo 129
  key C
  band Run
  melody in F5..E6
  loop

  Intro    with harmony
  Verse
  Climb    with echo
  Hook     with echo ending 1
  Hook     with harmony ending 2
}

groove Intro {
  Bass   1 - . 1  8 - 1 -  1 - . 1  8 - > -
  Arp    1 2  3 4  3 4  2 3
  Kick   x... .... x..x ....
  Hat    ..x. ..x. ..x. ..x.
  Snare  .... x... .... x...
  last {
    Kick   x... .... x... ....
    Hat    ..x. ..x. ..x. ....
    Snare  .... x... ...x x...
    Tom    .... .... .... .x..
    Floor  .... .... .... ..xx
  }
}

groove Verse {
  Bass   1 - . 1  8 - 1 -  . 8 - 1  5 - > -
  Arp    1 2  3 4  3 4  2 3
  Kick   x... ...x x... ....
  Hat    ..x. ..x. ..x. ....
  Snare  .... x... .... x...
  Open   .... .... .... ..x.
}

groove Climb {
  Bass   1 - 1 -  1 - 1 -  1 - 1 -  1 - > -
  Arp    1 2  3 4  1 2  3 4
  Kick   x... .... x... ....
  Hat    ..xx ..xx ..xx ..xx
  Snare  .... x... .... x...
  last {
    Kick   x... .... .... ....
    Hat    .... .... .... ....
    Snare  ..x. x.xx .... ....
    Tom    .... .... xxxx ....
    Floor  .... .... .... xxxx
  }
}

groove Hook {
  Bass   1 - 8 -  1 8 - 1  - 8 1 -  5 - > -
  Arp    4 3  2 1  2 3  4 3
  Kick   x... ...x x... ....
  Hat    ..x. ..x. ..x. ..x.
  Snare  .... x... .... x...
  Open   .... .... .... ...x
  first {
    Kick   .... ...x x... ....
    Crash  x... .... .... ....
  }
  last {
    Hat    ..x. ..x. ..x. ....
    Snare  .... x... ...x xx..
    Open   .... .... .... ....
    Floor  .... .... .... ..xx
  }
}

section Intro {
  Fmaj7       r8 a16 c e8. d c8 a4
  G6          r8 b16 d g'8. e d8 b d
  Em7         e2 d8 b g a
  Am7         b8. c4~ c16 e8 d c b
}

section Verse {
  Cmaj7       r4. e,16 g b8. a g8~
  Bm7b5 E7    g f8 e, d,4 g#8 b
  Am7         c4. b8 a4 e,8 g~
  Gm7 C7      g8 f bb8. a g8 e,4
  Fmaj7       f r8 a16 c e8. d c8~
  Fm6         c r8 ab g f d, f
  Em7 A7      e, g8 b c#8. e d8~
  Dm7 G7      d c8 a b4 g8 f
}

section Climb {
  Fmaj7       e,8 f a c4 e8 c a
  Em7         b g8 b4 d8 b g
  Dm7         a8 c d f'4 e8 d c
  G7sus4 G7   d4. c8 b4 r16 g a b
}

section Hook {
  Fmaj7       c8. a c8 e4 d8 c
  G7          d4. b4 g8 a b
  Em7         b8. g b8 d4 e8 d
  Am7         c2 r8 e d c
  Dm7         a8. f a8 c4 e8 f'
  G7          g' f'8 d b4 d8 f'
  ending 1 {
    Cmaj7       e2 d8 b g a~
    Gm7 C7      a r8 bb a g e, g
  }
  ending 2 {
    Cmaj7       e2 g'8 e d c~
    Gm7 C7      c r8 d c bb g e,
  }
}
```

### sprint.kau

Lap two. The second chorus is the first one a step up (`in E`), and its last two bars fall back to D as written.

```kauai
about {
  title  Sprint
  note   lap two of the racing game: 150 bpm in D, after the Japanese fusion bands of the 80s
}

use 'run.kau'

song Sprint {
  tempo 150
  key D
  band Run
  melody in A5..G#6
  loop

  Riff     with harmony
  Verse
  Lift     with echo
  Chorus   with echo ending 1
  Chorus   with harmony ending 2 in E
}

groove Riff {
  Bass   1 - . 1  - . 1 -  . 1 - .  1 - 8 -
  Arp    1 2  3 4  4 3  2 1
  Kick   x... ...x x... ....
  Hat    ..xx ..x. ..xx ..x.
  Snare  .... x... .... x...
  Open   .... .... .... ...x
}

groove Verse {
  Bass   1 8 1 8  1 8 1 8  1 8 1 8  5 8 > 8
  Arp    1 3  2 4  1 3  2 4
  Kick   x... ...x ..x. ....
  Hat    .xxx .xx. xx.x .xxx
  Snare  .... x... .... x...
}

groove Lift {
  Bass   1 - 1 -  1 - 1 -  1 - 1 -  1 - > -
  Arp    1 2  3 4  1 2  3 4
  Kick   x... .... x... ....
  Hat    .xxx .xxx .xxx .xxx
  Snare  .... x... .... x...
  last {
    Kick   x... .... .... ....
    Hat    .xxx .xxx .... ....
    Snare  .... x... x.xx ....
    Tom    .... .... .... xx..
    Floor  .... .... .... ..xx
  }
}

groove Chorus {
  Bass   1 . 8 1  . 8 1 .  1 8 . 1  8 . > 8
  Arp    4 3  2 1  4 3  2 1
  Kick   x... ...x x... ....
  Hat    .x.x .x.. .x.x .x.x
  Open   ..x. ..x. ..x. ..x.
  Snare  .... x... .... x...
  first {
    Kick   .... ...x x... ....
    Hat    .xxx .xx. .xxx .x.x
    Open   .... .... .... ..x.
    Crash  x... .... .... ....
  }
  last {
    Hat    .x.x .x.. .x.. ....
    Open   ..x. ..x. .... ....
    Snare  .... x... ..xx ....
    Tom    .... .... .... xx..
    Floor  .... .... .... ..xx
  }
}

section Riff {
  Dmaj7       d8 r16 a8 r16 d8 e f# r16 a'8.
  C/D         g r8 e r16 d8 c8. a8
  Bbmaj7      bb8 r16 f,8 r16 bb8 c d r16 f8.
  A7sus4 A7   e8. d a8 c# e d16 c# b a
}

section Verse {
  Em9         f#,2 r8 g,16 a b8 d~
  F#m7        d8 e4 c# b8 a4~
  Gmaj7       a r8 f#,16 a c#8. b a8~
  A7sus4 A7   a8 e, g, a d8. c# r8
  Bm9         r8 f#,16 a c#4 b8 a4 f#,8~
  Gmaj7       f#, e,8 f#, a8. b d8
  C9          e d8 bb4 g,8 e, g,
  F#m7 B7     a8. c# e8 d#4 a8 f#,
}

section Lift {
  G/A         e,8 a b d4 e8 d b
  A/B         f#,8 b c# e4 f#8 e c#
  Bb/C        g,8 c d f4 g8 f d
  C/D D7      e4. d8 c a f#,16 g, a8
}

section Chorus {
  Gmaj7       f#2 e8 f# d f#~
  F#m7        f#8 e c#4 b8 c# e4
  Fmaj7       e2 d8 e c e~
  Em7 A7      e8 d b4 c#8 e g f#~
  Bm7         f#4. e8 d8. c# b8~
  E9          b g#,8 b d8. e f#8~
  ending 1 {
    Gmaj7/A     f#4. e8 d8. b a8~
    Cmaj7 D/E   a g,8 b d8. e f#8
  }
  ending 2 as written {
    Amaj7/B     g# a'8 g# f# e c# a~
    Bbmaj7 C    a d8 f a' g e c
  }
}
```

### ballad.kau

The ending theme. The electric piano is a host voice (`Ep`) that plays chords, and the melody is played on it too, brighter.

```kauai
about {
  title  Theme
  note   the racing game's ending theme: a slow ballad in Eb, after the late-night coast-road ballads of the 16-bit arcades
}

band Trio {
  voice Piano  host Ep bright=0.8  plays chords rootless from E3  poly  vol 26%
  voice Sing   host Ep bright=1.3  plays melody                    poly  vol 46%
  voice Bass   triangle            plays roots in E2..D#3          vol 34  env 3 0 14  gap 3

  drum Brush  noise 4000  len 2  vol 4  env 0 0 5
  drum Ride   noise 6000  len 1  vol 3  env 0 0 10
}

song Theme {
  tempo 69
  key Eb
  band Trio
  melody in D5..C#6

  Intro
  mark Drive
  Verse
  Peak
}

groove Alone {
  Piano  x - - -  - - - -  - - 4 -  - - - - | x - - -  - - - -  x - - -  - - - -
  Bass   1 - - -  - - - -  - - - -  5 - - - | 1 - - -  - - - -  1 - - -  - - > -
  Ride   .... x... .... x...
}

groove Verse {
  Piano  x - - -  - - - -  - - x -  - - 4 -
  Bass   1 - - -  - - - -  - - 5 -  - - > -
  Brush  .... x... .... x...
}

groove Peak {
  Piano  x - - 4  - - 3 -  x - - 4  - - 3 -
  Bass   1 - - -  - - 5 -  1 - - -  8 - > -
  Brush  .... x... .... x...
  Ride   .... .... x... ..x.
}

groove Settle {
  Piano  x - - -  - - 4 -  - - 3 -  - - 2 -
  Bass   1 - - -  - - - -  - - - -  - - - -
}

groove Ring {
  Piano  x - - -  - - - -  - - - -  - - - -
  Bass   1 - - -  - - - -  - - - -  - - - -
}

section Intro {
  groove Alone
  Abmaj9      r4. bb16 g bb2~
  Gm7 C7      bb g8 f e2
  Fm9         f4. bb16 g eb2~
  Bb7sus4 Bb7 eb r8 eb16 f d4 f8 g
}

section Verse {
  Ebmaj9      bb2~ bb8 g4 f8
  Gm7 Cm9     g2 eb4 d8 eb
  Abmaj9      c2 bb4 g
  Fm9 Bb7sus4 ab4. g8 f4 eb
  Ebmaj9      d4. eb8 f4 g8 bb
  Dm7b5 G7    c4. ab8 b4. g8
  Cm9         g2~ g8 eb d c,
  Abm6 Db9    eb2 f4 eb
}

section Peak {
  Abmaj9      eb'4. c bb4
  Bb/Ab       d'2 c4 bb
  Gm7         bb4. d' c8 bb
  Cm9         g2~ g8 bb g eb
  Fm9         f2 ab4 g
  Bb7sus4 Bb7 eb4. f8 d4. r8
  groove Settle
  Abmaj9      eb2 g4 bb
  groove Ring
  Ebmaj9      g1
}
```

### jetveux.kau

*Je te veux* (Erik Satie, 1897). The voice part is as written in the OpenScore Lieder score (CC0), with its slurs, dynamics, crescendos and diminuendos, and the piano's right hand in the intro is the part `Fill`, its chords and ties as written; the chord symbols are a reading of the piano part, and the `\rit 104` and the `\tempo` that answers it stand for the score's *retenir* and *au refrain*.

```kauai
about {
  title     Je te veux
  composer  Erik Satie
  year      1897
  source    the OpenScore Lieder score (CC0)
  note      the voice as written; the chords are a reading of the piano part
}

voicings {
  7b5     0 4 6 10
  9sus4   0 5 7 10 14
}

band Salon {
  voice Singer  pulse               plays melody                           duty 1/2  vol 22  env 2 2 8
  voice Bass    triangle            plays roots in E1..D#2                           vol 32  env 0 2 6
  voice Piano   host Ep bright=0.9  plays chords from E3  plays part Fill  poly      vol 30%
}

song JeTeVeux {
  tempo 124
  meter 3/4
  key C
  band Salon
  groove Valse
  melody in D4..C#5
  part Fill in A3..G#4

  Intro
  Refrain  ending 1
  Couplet
  Refrain  ending 2
  Couplet
  Refrain  ending 3
  Coda
}

groove Valse {
  Bass   1 - -
  Piano  . x x
}

section Intro {
  N.C.          r2.
    Fill:       r4 e g
  G9            r2.
    Fill:       <a d>2.
  C6/G          r2.
    Fill:       <a c e a'>2 <e, e>4
  G7            r2.
    Fill:       <g,~ b~ e g~>2.
                r r2
    Fill:       <g, b d g>4 e g
}

section Refrain {
  pickup  \pp (e g
  Cadd9         \tempo d'2.
  C/G           c2 e'4
  Cmaj7         b2.
  C6/G          a2.)
  Cmaj7         (b2.
  C6/G          a2 e4
  G7/D          b2.~
  G6            b) (d e
  G7/D          g2.~
  G6            g) (d e
  Dm6           a2.~
  G6            a) (d e
  G7/D          g2 f4~
  G7            f g2
  C             e2.
  Em7/B         d) (e g
  A7sus4        d'2.
  C/G           c2 e'4
  Cmaj7         b2.
  C6/G          a2) r4
  Cmaj7         (b2.
  C6/G          a2 g4
  Dm7           a2.
  Dm/F          d) (f g
  G7 G7 C/E     \cresc a b c \!
  Dm/F Em/G Dm7 d' e' f'
  C/E C/G Am    g' e' c
  F#m7b5        a c e'
  C/G C/G F/A   \dim g2 f4) \!
  G7/F          (e d2
  ending 1 {
    C             c,2.~
    D7 D7 C/E     c,) (b c)
  }
  ending 2 {
    C             c,2.~
    D7 D7 C/E     c,) (b c)
  }
  ending 3 {
    C             c,2.~
                  c,) r r
  }
}

section Couplet key G {
  melody in E4..D#5
  Am6           \p (e2.~
  D7            e) (e f#
  G             d,2.~
  D7            d,) \cresc (f# g \!
  D7/A          d2 d4~
  D7            d d2
  G6            \dim e'2. \!
  D7            d) (b c
  Am6           e2.~
  D7            e) (e f#
  G             d,2.~
  G/D           d,) (b g
  F#7           \cresc f#2 f#4~ \!
  F#7/C#        f# f#2
  Bm/D          \dim b2. \!
  Cmaj7         d) (b c
  Am6           e2.~
  D7            e) (e f#
  G             d,2.~
  D7            d,) \cresc (f# g \!
  D7/A          d2 d4~
  D7            d d2
  Gmaj7         \dim f#'2. \!
  D7            e') (d b
  Am6           e2.~
  D7            e) (e f#
  G             d,2.~
  D7            d,) (g f#
  Am7           e \cresc g c
  Ab7b5         \rit 104 d2 \! d4
  G G9sus4 G6   \dim g'2. \!
  G7            g) r2
}

section Coda {
  Em            \tempo 114 r2.
  Dm7/F         r2.
  G7/D          r2.
  C             r2.
                r2.
}
```

## Appendix D: Habits that do not carry over

Some things other music notations write one way are written, or read,
another way here. Each row is a habit and what Kauai takes instead; most of
them would otherwise play a wrong note without an error.

| Habit | In Kauai |
|---|---|
| A key signature sharpens or flattens the notes | the key adds no accidentals: in `key D`, F sharp is `f#` |
| `c'` is one fixed octave | `'` and `,` move a note an octave from its range (`melody in LO..HI`) |
| `C4` in a line of notes is middle C | notes are in lower case and the number after them is a length: `c4` is a quarter note; `C4` is a pitch only in a range or after `from` |
| `bb` might be two B's | `bb` is B flat, and B double flat is `bbb` |
| A length carries over into the next bar | a bar's first note without a value is one beat |
| A mark after the note (`c4-.`) | marks go before the note: `.c4` |
| Dynamics as `p`, `\<` or `/p` | directions start with `\`: `\p`, `\cresc`, and `\!` to end a `cresc` |
| `C9` for a chord with an added ninth | `C9` has the minor seventh; `Cadd9` and `C(9)` do not |
| `Cdim` for a diminished seventh | `Cdim` is the triad; the seventh chord is `Cdim7` |
| `tempo 60` in 6/8 for dotted quarters | `tempo` counts quarter notes; dotted quarters are `tempo 4.=60` |
| A drum row of 15 or 17 steps for 16 | a bar's steps come to a note value, and a miscount is an error |
| `#` or `%` starts a comment | comments start with `//`; `#` is a sharp and `%` repeats a bar |
| The title and composer in quotes (`title = "..."`) | an `about` block, each item's text to the end of its line without quotes |
