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

**The recordings decide how good the instrument can be.** The engine borrows
a recorded note with its own moves of pitch, loudness and timbre, so it can
only be as natural as what it borrows. The alto's first recordings were a
university's single notes in an anechoic chamber (32 notes, no vibrato, no
join between notes), and it sounded like a wind controller; with seven solo
improvisations in its place (2520 notes with real slurs and attacks) it
sounded like a player. The soprano went the same way. Spend the effort here
before spending it on style numbers, engine changes or listening rounds, and
go on searching while the design still has a shortfall.

Look for the recordings the design (step 1b) asks for: the ones that add
notes, long notes, dynamics and the playing it lacks. Only recordings that
are free to reuse:

- released as **CC0 or CC BY** (never NC or ND: CC BY-NC items looked ideal
  and had to be thrown out), or a collection that says it is free to use with
  a credit (quote its statement in the manifest, and ask the person first);
- with the licence read **from the item's own page**, not from a search
  snippet or an aggregator's summary.

**Real performances beat sample libraries.** What the aim needs is the
instrument played as music, with slurs, tongued attacks, scoops and falls,
vibrato that comes and goes, and breath. A library of single notes, however
clean, has none of it. Prefer, in this order:

1. solo improvisations or tunes by one player, dry, with no accompaniment;
2. multitrack or stem datasets with the instrument isolated (read their
   licences: many are research-only);
3. single-note libraries, only to fill a range the performances lack.

Where to look, with what worked:

- **Freesound** search with the licence filter in the URL
  (`/search/?q=alto+sax+solo&f=license:"Creative Commons 0"`, and
  `license:"Attribution"`); then the uploader's other sounds and packs, since
  one player often posted many takes. The page source carries the HQ preview
  (`cdn.freesound.org/previews/...-hq.mp3`) that can be fetched without a
  login; the original WAV needs one, so ask the person to download it when
  quality is the limit.
- **Academic datasets** (Zenodo, ISMIR papers): isolated wind and string
  parts under CC BY exist. Check the instrument, the register (a chorale
  dataset has no high notes) and the licence text.
- Public-domain archives, with care: a recording's date and country decide
  whether it is free, and old transfers are poor.

Check each candidate before relying on it: its range (a "soprano" sax that
plays below the soprano's lowest note is not one), whether it clips, how much
room it has, and whether it is alone: the level of the gaps between notes
against the notes says it (a backing track under the instrument leaves the
gaps nearly as loud; alone, they fall 14 dB or more). Measure it
(`measure.cul`) and read `coverage.cul` before taking it: the count of notes,
slurs and tongued attacks is what the recording is worth, not its length.

**Look for what is missing, by name.** After the first measure, list the
articulations the songs will ask for (a fall at a phrase's end, a scoop up
into a note, a slur up a fourth) and count how many the recordings have. An
articulation with no recording is played by the engine's rule, which bends
the pitch and nothing else, and it is the sound heard as electronic. Search
again for a recording that has it, rather than tuning the rule.

Keep every recording in `work/sources/` with a manifest of its URL, author,
license, and what you found in it.

## 3. Write the test songs

The songs are how every later judgment is made, so make them first:

- **A song for the use asked for**, in its style, tempo, and range.
- **A song of a different character**, so the instrument is not tuned to one
  kind of music: a ballad with long tones if the use is upbeat, a fast tune
  if it is slow.
- **A practice piece**: scales and leaps across the range, long tones,
  every way of starting and joining notes the songs use. It finds the
  weak spots quickly.

Write them in the keys the recordings hold. Recordings of a few takes hold
their own keys' pitches many times and the others a few: the alto's
improvisations, in G minor and F minor, hold 144 Bb4s and 9 E5s, and no E5,
F#5 or B4 of 0.4 s or more. Its first tunes, in A major and E minor, asked
for E5 73 times, 33 of them held, and each was borrowed from a semitone away
or filled from another pitch's long notes: an artificial tone that no change
to the engine could reach. Run `coverage.cul --song` on each song before
rendering it, and move the tune (or the passage, with `in`) until nothing is
short; a last chorus a tone up must keep to its new key's covered pitches
too.

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

When a word of the person's does not say where the fault is, or a change
made it worse, work from the word to a number and back to the ear
(`listening.md`, "From a word to a number"): the time, the note alone, the
real thing and ours measured with one tool, one change at the strength the
person last called good.

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
