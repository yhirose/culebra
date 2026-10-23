# The flow, step by step

## Contents

1. Turn the request into a sound to aim for
1b. Design the recordings before looking for them
2. Find the sources
3. Write the test songs
4. The loop: measure, make, check, listen
5. Finishing the package

## 1. Turn the request into a sound to aim for

A request names an instrument and a feeling, rarely more: "an alto
saxophone for jazz fusion, like the lead alto of T-SQUARE", "a mellow
soprano like Kenny G". Before building anything, restate it in three
parts and ask the person to confirm or correct each:

- **The instrument and its range**: which instrument, the lowest and highest
  notes the songs will ask of it, and how it should behave at the edges
  (the soprano saxophone plays its top notes as overtones, as a player does).
- **The timbre in words**: bright or dark, breathy or clean, round or edgy,
  how much vibrato and where. Offer words for the person to pick from rather
  than asking an open question.
- **How the music plays it**: long tones or fast runs, how notes start
  (tongued, breathed in) and join (slurred, bent, scooped), the dynamics.

A player or a record named as a reference is a guide for these words. It is
not material: do not use those recordings as sources.

Write the confirmed aim at the top of `work/notes.md`.

## 1b. Design the recordings before looking for them

Decide what the recordings must cover, write it down, and only then search.
Searching first finds whatever is free; designing first finds what the
instrument needs, and a search that cannot meet the design says so early.

The design (`kauai/sounds/<name>/work/design.md`, the same numbers as the
case's `needs` when there is a case) states:

- **Range**: the lowest and highest notes, in Hz (`make.measure`).
- **Notes per semitone**: at least 5 across the range (`coverage.cul`).
- **Long notes**: at least 10 per octave of 1 s or more. The long notes are
  what the instrument plays best and what a recording of short tones lacks.
- **Dynamics**: loud, middle and quiet notes, at least 30 each. Recordings
  made at pp, mf and ff are one reference group in `make.reference_groups`
  (`make_data.cul` gives them one loudness reference), or the dynamics vanish.
- **Playing**: slurred runs, tongued notes, long tones, and vibrato or none.
  Slurs are a need of their own: a slur in the song is heard only if a
  recording has the same kind of join (interval, direction, length), since
  borrowing one note at a time from different takes breaks a slur into two
  halves. Count the slurs the songs use by interval and direction, and find
  recordings that have them. A design without slurs is a design for a
  different instrument's music, and the pieces will sound tongued.
- **Quality**: no clipping, no accompaniment, a dry or known room.

Then search, and for each candidate recording say which of the design's
needs it meets (`culebra kauai/tools/measure.cul` and `coverage.cul` say how
many notes and long notes it adds). A recording that adds nothing the design
lacks is left out. When the design cannot be met, the person hears what is
missing and decides: a different range, a recording to make, or no instrument.

## 2. Find the sources

Look for the recordings the design (step 1b) asks for: the ones that add
notes, long notes and dynamics it lacks. Look for recordings released as CC0 or CC BY (Freesound, university sample
libraries); leave out anything NC or ND. Prefer:

- a dry room, or at least one you can tell apart from the tone
- the instrument alone, with no accompaniment bleeding in
- the range the aim needs, played as the aim plays it (long tones and
  phrases, not only single notes)
- no clipping, and a quiet noise floor

Check each candidate before relying on it: its range (a "soprano" sax that
plays below the soprano's lowest note is not one), whether it clips, how
much room it has. Keep every recording in `work/sources/` with a manifest
of its URL, author, license, and what you found in it.

## 3. Write the test songs

The songs are how every later judgment is made, so make them first:

- **A song for the use asked for**, in its style, tempo, and range.
- **A song of a different character**, so the instrument is not tuned to one
  kind of music: a ballad with long tones if the use is upbeat, a fast tune
  if it is slow.
- **A practice piece**: scales and leaps across the range, long tones,
  every way of starting and joining notes the songs use. It finds the
  weak spots quickly.

Put `mark NAME` before each passage worth hearing alone (a long tone, a
fast run, the top of the range), so `listen.cul --from NAME` can cut it
out. Render the songs with an instrument that exists already and have the
person hear them before you build anything. If a song is weak, the person
will hear the song and blame the instrument.

## 4. The loop: measure, make, check, listen

Each round:

1. **Measure** what the change needs from the sources.
2. **Make** one change to the instrument. One: two changes heard together
   cannot be told apart, and a change that helps can hide one that hurts.
3. **Check** with `check.cul` on the passage and on the whole song. Fix what
   it finds before listening. If it finds nothing new, go on.
4. **Compare** the passage the change is about with `listen.cul`, the
   version before against the version after. Keep the winner.
5. **Write down** in `notes.md` what changed, what was heard, and what it
   turned out to be.

Now and then, have a whole song heard (`culebra play --wav` without
`--from`): some things are only heard in context, as a phrase sounding
mechanical when it repeats, or a level that is right alone and wrong in
the band.

When the person says something sounds wrong, find where and why before
changing anything: cut out the passage, measure it, compare it with the
source. A thump the person heard at 0:22 may be a note read an octave
low in the measurements, not a fault in how the note is played.

## 5. Finishing the package

- `main.cul` documents the instrument's parameters at its top, as the
  soprano's does, with their defaults.
- `data/` holds only what the instrument plays from, packed small (16-bit
  numbers, as the soprano's are), read with `Dir.embedded`.
- `README.md` says what the instrument is, how to play a song with it, and
  credits every source with its license.
- `test_<name>.cul` loads the data, voices a phrase, and plays a song
  through Kauai, quickly.
- A program that cannot wait for the sounds to be made uses
  `make_sounds.cul` to make them beforehand.
