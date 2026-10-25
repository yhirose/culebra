# The harness: an instrument made from nothing, judged by a distance

The harness is how a new Kauai instrument is made with as little of a
person's time as it takes: agents with one job each use the tools, a
distance says how far the result is from what it should sound like, and the
person is asked only what no number can answer. It is the same for every
instrument; what is particular to one is its **case**.

```
kauai/harness/
  README.md        this file
  roles/           what each agent is for: inputs, outputs, tools, when to stop
  template/        main.cul, the one file a package adds to its data/
```

and, in `kauai/tools/`, what the roles use: `measure.cul`, `make_data.cul`,
`fit_style.cul`, `compare.cul`, `check.cul`, `listen.cul`;
and in `kauai/engine/`, the playing code every instrument made from
recordings shares.

## A case

What is being made is written down as a case, `case.json`, in the instrument's
`work/` folder:

| Key | What it is |
|---|---|
| `name`, `aim` | the instrument's name, and what it should sound like, in words |
| `voice`, `range` | its name in songs (lower case), and the pitch range (`low`, `high`, Hz) |
| `sources` | the recordings (file, title, author, licence, link) |
| `tune`, `eval` | the songs the instrument is adjusted on, and the songs it is judged by and never adjusted on |

A case may have targets: renderings of the same songs by whatever the
instrument should sound like, one for each `tune` and `eval` song
(`{song, target}`, `target` a WAV). `fit_style.cul` and `compare.cul` need
them. A request for an alto saxophone has none: the composer writes the
`tune` and `eval` songs, and the person's ear is the target. The flow below
says what each step does in either case.

## The flow

0. **design** writes what the recordings must cover (`design.md`: the range,
   notes per semitone, long notes per octave, loud, middle and quiet notes,
   the playing), before any search. `kauai/tools/coverage.cul` checks it.
1. **scout** finds recordings that meet the design (free licences, or a
   collection that says so, quoted in the manifest) and writes `sources`.
   The recordings set the ceiling on how natural the instrument can be, so
   this step is where the effort goes: real performances over single notes,
   and a new search for any articulation the songs ask for that no recording
   has.
2. **composer** writes the songs the instrument is tuned and judged by.
3. **measurer** measures the recordings (`measure.cul`), choosing the range.
4. **builder** makes `data/` (`make_data.cul`), writes `data/instrument.json`
   (the `make` it is made with and a first `play`), adds `main.cul` from the
   template, and sets `play`. With targets, `fit_style.cul` finds it against
   the `tune` targets. Without, the builder writes `play` by hand from the
   recordings and from the style of an instrument already made (the alto's was
   set from the soprano's relations), and the ear tunes it from there.
5. **judge** plays the `tune` songs and says what to change; the builder
   changes one thing. With targets it reads `compare.cul`'s numbers and its
   worst seconds, and steps 4 and 5 repeat until the distance stops falling.
   Without, it reads `check.cul`'s findings and `sweep.cul`, and has the
   person hear two renderings of a passage (`listen.cul`); the round ends
   when the person keeps the version they hear.
6. **skeptic** plays the `eval` songs (`culebra play --wav`), which nothing was
   adjusted on, looks for ways the result is not what it seems (a number met
   by a trick, a song it only just passes), and reports pass or fail. Without
   targets, nothing is a number to pass: it runs `check.cul` and `sweep.cul`
   on them and asks the person to hear them.
7. **the person** listens to what the numbers cannot judge (`listen.cul`) and
   decides.

## Who runs on what

| Role | Model | When it is stuck |
|---|---|---|
| orchestrator (the session) | the session's | hands over (below) |
| scout, composer | sonnet | opus |
| measurer, builder | sonnet | opus |
| judge, skeptic | opus | opus with a deeper effort, then a larger model |

A role is *stuck* when its number has not fallen in two rounds in a row, or
it has used the rounds it was given. What solved it is written in the
journal, so that the table moves toward what is cheapest.

## Context

Nothing is carried by an agent's memory. A run keeps a journal
(`work/journal.md` in the instrument's folder): what was decided and why, the
distances after each round, what was tried and put aside, what is next. An
agent is given one job, the journal, and the files it needs; it ends by
writing what it tried, what it found and what to do next, and a new agent
takes up from there. The orchestrator reads only summaries.

## What the person is asked

The aim in words, a source with a licence that is not plain, the end result
by ear, and anything the numbers pull apart on (a distance that falls
while the sound is heard as worse). Nothing else.
