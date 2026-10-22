# The flow, step by step

## Contents

1. Turn the request into a sound to aim for
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

## 2. Find the sources

Look for recordings released as CC0 or CC BY (Freesound, university sample
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
