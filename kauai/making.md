# Making an external instrument

An external instrument is a folder under `sounds/`: `main.cul` plus `data/`.
The data is measured from recordings; the playing is the shared engine's
(see [engine.md](engine.md)). A new instrument is therefore data and a style,
not a copy of the playing code.

## The route

1. **Say what the sound should be.** The instrument, its range, its timbre in
   words, and how the music will play it (tongued or slurred, long tones,
   vibrato). Write the aim down before anything else.
2. **Design the recordings.** Decide what they must cover: the range, notes
   per semitone, long notes per octave, loud, middle and quiet notes, and the
   slurs the songs use by interval and direction. Pieces played as slurs need
   recordings that play those slurs, since a slur borrowed one note at a time
   from different takes breaks in two. `coverage` counts this.
3. **Find the recordings the design asks for.** Check each source's licence
   first. Recordings under CC0 or CC BY are simple. A collection that asks for
   a credit and to be told how it is used (the University of Iowa's is one)
   is usable with the credit in the README; it is a request, not a licence
   text. Keep the raw files in the instrument's `work/` (git ignores it).
4. **Measure and make the data.** `measure` reads each recording's notes;
   `make_data` writes `data/`. A group of recordings at different dynamics
   shares one loudness reference (`make.reference_groups`), or their dynamics
   come out equal.
5. **Set the style.** `data/instrument.json` holds the style (`play`) and what
   `make_data` is made with (`make`). `fit_style` finds the numbers the
   recordings cannot say, from target renderings of tune songs.
6. **Judge.** `compare` gives the distance from the targets; `evaluate` runs
   the eval songs, which were not used to tune. The numbers do not always
   match the ear: a listener's pick is the last word, and the ear found the
   click at a join that the numbers had not.
7. **Listen.** `listen` renders two versions of a passage, hidden as 1 and 2,
   at the same loudness. Keep the one the person picks.
8. **Finish the package.** `main.cul` (from `harness/template/main.cul`),
   `data/instrument.json`, the README with the sources and their licences, and
   a test.

## The work, step by step

The rules and the steps for working with Claude are in
[skills/make-instrument](skills/make-instrument): `SKILL.md` for the rules,
`references/workflow.md` for the steps, `references/tools.md` for the commands.

A fully automatic route (agents with one job each, judged by a distance, the
person deciding the last word) is in [harness/README.md](harness/README.md).

## What is still open

- **Slurs in a recording are only borrowed whole if the engine supports it.**
  Today a written slur whose notes come from different takes may break.
- **Long notes**: the engine stretches a note's own recording only up to a
  point, and fills the rest from other notes.
- **Judging the joins.** The numbers see a jump; they do not say whether a
  join sounds natural.
