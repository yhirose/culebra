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
  cleanroom.cul    a copy of the repo to work in without seeing the answer
  cases/<name>/    a case: case.json, and the songs it is tuned and judged by
```

and, in `kauai/tools/`, what the roles use: `measure.cul`, `make_data.cul`,
`fit_style.cul`, `compare.cul`, `evaluate.cul`, `check.cul`, `listen.cul`;
and in `kauai/engine/`, the playing code every instrument made from
recordings shares.

## A case

`cases/<name>/case.json` says what is being made and how it is judged:

| Key | What it is |
|---|---|
| `name`, `aim` | the instrument's name, and what it should sound like, in words |
| `voice`, `range` | its name in songs (lower case), and the pitch range (`low`, `high`, Hz) |
| `sources` | the recordings (file, title, author, licence, link) |
| `tune` | songs the instrument is adjusted on: `{song, target}`, `target` a WAV in the targets folder |
| `eval` | songs it is judged by and never adjusted on, the same way |
| `hide` | files that hold the answer in another shape, left out of a clean room |

A case with targets is a test: the instrument made must sound like them.
A case with none (a request for an alto saxophone) is a job: the `tune` and
`eval` songs are written by the composer, and the person's ear is the target.

## The flow

0. **design** writes what the recordings must cover (`design.md`: the range,
   notes per semitone, long notes per octave, loud, middle and quiet notes,
   the playing), before any search. `kauai/tools/coverage.cul` checks it.
1. **scout** finds recordings that meet the design (free licences, or a
   collection that says so, quoted in the manifest) and writes `sources`.
2. **composer** writes the songs the instrument is tuned and judged by.
3. **measurer** measures the recordings (`measure.cul`), choosing the range.
4. **builder** makes `data/` (`make_data.cul`), writes `data/instrument.json`
   (the `make` it is made with and a first `play`), adds `main.cul` from the
   template, and finds `play` by `fit_style.cul` against the `tune` targets.
5. **judge** plays the `tune` songs, reads `compare.cul`'s numbers and its
   worst seconds, and says what to change; the builder changes one thing.
   Steps 4 and 5 repeat until the distance stops falling.
6. **skeptic** plays the `eval` songs (`evaluate.cul`), which nothing was
   adjusted on, looks for ways the result is not what it seems (a number met
   by a trick, a song it only just passes), and reports pass or fail.
7. **the person** listens to what the numbers cannot judge (`listen.cul`) and
   decides.

## The test: the soprano saxophone again

`cases/soprano_sax` is the instrument made once by hand. The test is that
the harness makes it again from the recordings alone, in a clean room that
has not got the package, and that the result is within `compare.cul`'s
limits of the hand-made one on the `eval` songs.

```sh
culebra kauai/harness/cleanroom.cul --case soprano_sax --dest ../runs/soprano-1
```

makes the copy (outside the repo, without the package, the eval songs and the
files `hide` names). The agents work only in it. The result is judged back
in the repo:

```sh
culebra kauai/tools/evaluate.cul --sounds ../runs/soprano-1/kauai/sounds/soprano_sax \
  --case kauai/harness/cases/soprano_sax/case.json --targets <targets>
```

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
(`work/journal.md` in the clean room): what was decided and why, the
distances after each round, what was tried and put aside, what is next. An
agent is given one job, the journal, and the files it needs; it ends by
writing what it tried, what it found and what to do next, and a new agent
takes up from there. The orchestrator reads only summaries.

## What the person is asked

The aim in words, a source with a licence that is not plain, the end result
by ear, and anything the numbers pull apart on (a distance that falls
while the sound is heard as worse). Nothing else.
