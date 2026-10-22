---
name: make-instrument
description: Make or improve an external instrument for Kauai, Culebra's music language, as a package under kauai/sounds/ that plays songs with a real instrument's sound (an alto or soprano saxophone, a flute, a bowed string, an electric piano), using Culebra alone, from royalty-free recordings. Use this skill whenever someone asks for a new instrument sound for Kauai or culebra play, wants an existing external instrument to sound more natural or more like a player or a style ("mellow like Kenny G", "the lead alto of a fusion band"), reports a noise or an artificial tone in one (a hiss, a click, "it sounds electronic"), or wants to compare two versions of an instrument by ear, even if they do not say "skill" or "external".
---

# Making a Kauai instrument

An external instrument is a Culebra module that Kauai hands each phrase (or
pitch, or hit) of a song and that answers with an `Audio.Sound`. A finished
one is a package: a folder under `kauai/sounds/` whose `main.cul` exports
`voices` (and `drums`), with the data it plays from in `data/` beside it,
a README that credits its sources, and a test. The soprano saxophone in
`kauai/sounds/soprano_sax/` is the first one made from recordings (its
recordings were measured with Python then; this skill does it in Culebra).
Read its README and the top of its `main.cul` before starting, as the model
of what a package holds.

The work is a loop between measuring, making, and listening, and the
listener is the person you work with. Your job is to make each round of
that loop cheap for them: hear only what measuring cannot find, hear one
change at a time, and hear it in a passage short enough to judge.

## Ground rules

- **Culebra only.** Measure recordings, train any model (Culebra's
  `Tensor`), and render with Culebra. No Python, no PyTorch. The one
  outside tool is `ffmpeg`, to keep made sounds as Ogg Vorbis. This keeps
  the instrument something anyone with the `culebra` binary can rebuild.
- **Royalty-free sources.** Use recordings released as CC0 or CC BY (never
  NC or ND). Note each source's URL, author, and license when you take it,
  and credit them in the package README as the soprano's does.
- **One place for the work.** Everything that is not the package goes in
  the instrument's `work/` folder (`kauai/sounds/<name>/work/`, which git
  ignores): the recordings, what was measured, the test songs, the
  listening files, and `notes.md`. Scattered files were the main mess of
  the first instrument; keep them out of `/tmp` and the repository root.
- **Talk in the listener's words.** Speak the person's language. Ask them
  to describe what they hear in their own words, onomatopoeia included
  (a hiss, a "wah-wah" wobble, a "too-too" stutter, a "bwo" thump), and
  write those words down in `notes.md` next to what they turned out to be.
- **Decide with them, not for them.** Confirm the sound you are aiming for
  before building it, and ask before any choice they would want to make
  (what a parameter is called, a trade-off between two qualities).

## The workspace

```
kauai/sounds/<name>/
  main.cul            the instrument (committed)
  data/               what it plays from (committed)
  README.md           how to use it, and its sources (committed)
  test_<name>.cul     its test (committed)
  work/               not committed
    sources/          the recordings, and a manifest of where each came from
    measured/         what was measured from them
    songs/            the test songs, with marks on their passages
    listen/           what listen.cul writes
    notes.md          the aim, what was decided, what the listener heard
```

Start `notes.md` on the first day with the aim as confirmed, and add to it
every round: what changed, what the listener said, what it turned out to
be. It is what a later session, or a later instrument, learns from.

## The flow

1. **Turn the request into a sound to aim for.** Requests come in loosely
   ("an alto sax for jazz fusion, like the lead alto of T-SQUARE"). Restate
   it as: the instrument and its range, the timbre in words (bright, breathy,
   round, edgy), and how the music plays it (long tones, fast runs, bends,
   vibrato, how notes start and join). Confirm this with the person before
   anything is built. A player named as a reference is a guide for those
   words only; their recordings are not material.
2. **Write the test songs before the instrument.** Write, in Kauai, a song
   for the use asked for, one of a different character (a ballad if the use
   is upbeat, or the reverse), and a short practice piece that exercises the
   range and the joins. Put a `mark` before each passage worth listening to
   on its own. Render them first with an instrument that already exists (a
   built-in voice, or the soprano saxophone) and have the person hear them:
   a weak song makes every later judgment about the instrument unreliable.
   Songs the person brings are welcome too.
3. **Loop: measure, make, check, listen.**
   - Measure the sources (pitch, loudness, harmonics, breath, attacks) and
     make the instrument from what was measured.
   - Render the test songs and run `check.cul` on them. Fix what it finds
     before anyone listens; what measuring can find is a waste of the
     listener's ear.
   - Improve one thing at a time by comparing two renderings of a passage,
     1 against 2, with `listen.cul`, keeping the winner and trying the next
     change against it, as an optician does with lenses.
   - Have the whole song heard now and then: a passage can be right and the
     song still feel wrong.

`references/workflow.md` has each step in detail, with what to ask and
what to watch for. Read it at the start of a new instrument.

## Listening

Every comparison goes through `listen.cul`: the two versions are named only
1 and 2, assigned at random, at the same loudness, as separate files. Two
things about blind comparison are easy to break: do not read the record
(`work/listen/NAME.json`) before the person answers, and do not describe
the versions in a way that tells which is new. Give the person paths they
can open from their own machine. `references/listening.md` covers choosing
the passage, what to change between rounds, and how to read what the
person says. Read it before the first comparison.

## Tools

| Tool | For |
|---|---|
| `culebra play --sounds <pkg> --wav F --from A --to B song.kau` | render a song, or a passage of it, to a WAV |
| `culebra kauai/tools/check.cul --sounds <pkg> song.kau` | what measuring finds: pitch off the score, dropouts, dips, abrupt edges, clicks, clipping, silence |
| `culebra kauai/tools/listen.cul start/pick/next` | two versions of a passage heard blind, round after round |
| `culebra kauai/tools/make_sounds.cul` | every sound songs need, made beforehand, for a program or the browser |
| `culebra kauai/tools/measure.cul --low HZ --high HZ --out work/measured work/sources/*` | each recording's pitch and loudness every 10 ms, and its notes with how each starts and ends |
| `Audio.decode`, `Audio.spectrum`, `Audio.pitch`, `Audio.resample`, `Audio.loudness`, `Audio.integrated_loudness` | reading and measuring recordings in Culebra |
| `song.marks()`, `song.seconds(at)`, `song.external_sounds()`, `song.render(from:, to:)` | where passages are, and what an instrument is asked for |

`references/tools.md` has how each is used in this work, with the options
that matter. Read it when you first use one.

## Making the sound

`references/making.md` is where what works for building an instrument's
sound is written down: how the soprano saxophone borrows recorded notes,
joins them, adds the breath, and plays its top notes as overtones. It is a
skeleton for now; fill it in as the measuring tools move into Culebra and
as each instrument teaches something that carries to the next.
