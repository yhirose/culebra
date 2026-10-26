# Writing a song in Kauai

A walkthrough of writing a short song in Kauai, the music language that
ships with culebra: from a first chord to a song with a repeat, a dynamic and
a tempo change, played from the command line or from a culebra program. The
complete reference is [the language specification](../kauai/language.md).

A song is a plain text file with the extension `.kau`. You write it in four
parts, and the parts can come in any order in the file.

| Part | What it holds |
|---|---|
| `band` | the voices that play it |
| `groove` | the accompaniment, written against the chords |
| `section` | the lead sheet: one line per bar, its chords and its melody |
| `song` | the tempo, the meter, the key, and the sections in the order they play |

## Your first song

Save this as `first.kau`:

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

Play it with the command line:

```sh
culebra play first.kau
```

Read it this way:

- **Each line of a section is one bar.** The chords come first, then the notes.
  The second line names no chord, so it keeps `C`. `F G` puts two chords in one
  bar, one after the other.
- **Notes are letters.** `c` is a C, and `e4` is an E as a quarter note. The
  lengths are `1` (whole), `2` (half), `4` (quarter), `8` (eighth) and so on.
  A note with no length takes the length of the note before it in its bar.
- **An apostrophe moves a note up an octave**, so `c'` is the C above the
  range `C5..B5`. A comma moves it down.
- **The groove is the accompaniment.** `1` is the chord's root, `5` its fifth,
  and `-` holds the last note. The groove plays under every bar of the song.
  A row names a drum or a voice of the band, and a name the band does not
  have is an error.
- **`Verse x2`** plays the section twice.

## Marks, dynamics and tempo

Add marks by putting them before a note, and directions by starting a word
with a backslash. A direction takes effect from the note after it.

```culebra
let song = Audio.Kauai.new(`
band Duo {
  voice Tune  pulse     plays melody            vol 20
  voice Bass  triangle  plays roots in E2..D#3  vol 30
}

song Second {
  tempo 100
  band Duo
  groove Walk
  melody in C5..B5

  Verse
  Chorus
}

groove Walk {
  Bass  1 - 5 -
}

section Verse {
  C     .e4 .g c'2
        \cresc d'4 c' g2 \f
  F G   a4 f g d
  C     c1 \rit 80
}

section Chorus {
  C     \tempo 120 c2 !c'4 g
  F G   a4 f g d
  C     c1
}
`)
println(song.length())
```

- `.` before a note makes it staccato (half its length). `!` makes it an accent.
- `\cresc` gets louder up to the next dynamic, here `\f`. The dynamics are
  `\pp`, `\p`, `\mp`, `\mf`, `\f` and `\ff`.
- `\rit 80` slows the tempo down evenly, up to the next tempo direction.
- `\tempo 120` sets the tempo from that note on.

## Playing from a program

A program plays a song with `Audio.Kauai`. `Audio.Kauai.new` takes the text of
a song, and `Audio.Kauai.load` takes a file. Before it plays, a song can tell
you its length and the notes it will play:

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
println(song.length())             # => 19.2
println(song.events()[0])          # => {at: 0.0, len: 0.6, by: 'Tune', pitch: 76, vol: 20.0}
```

`song.render()` gives the song as samples, with no audio device, so a program
can save it to a file. `culebra play --wav first.wav first.kau` does the same
from the command line.

## When something is wrong

Kauai checks the whole song before it plays, and an error names the line it
is on, so you can go straight to it. A section line whose first word is not a
chord, such as `Q c1` where `Q` is not a chord or a key, stops the song before
any sound, with a message like `not a chord or key: Q`.

## Next steps

- The [language specification](../kauai/language.md) lists every word:
  voices and their options, marks, directions, repeats, parts and drums.
- [Writing a sound in Kauai](kauai-sounds.md) covers the voices that make the
  sound, and external instruments.
