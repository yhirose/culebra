# Kauai

Kauai is Culebra's music language. This folder holds what goes beyond its
built-in instruments: external instruments made from recordings, the playing
code they share, the tools that measure and check them, and the skill and
harness that make them. Read the part for what you want to do.

## To play a song with an instrument

- [`sounds/soprano_sax`](sounds/soprano_sax): a soprano saxophone, made from
  royalty-free recordings. Its README says how to play a song with it.
- `culebra play --sounds kauai/sounds/soprano_sax song.kau`

## To make a new instrument

- **[making.md](making.md)**: the whole route, in the order to do it, and the
  rules that matter (the sources' licences, designing the recordings before
  searching for them, judging by ear).
- **[skills/make-instrument](skills/make-instrument)**: the step-by-step work,
  for Claude to follow with the person (`SKILL.md` for the rules,
  `references/workflow.md` for the steps).
- **[harness](harness)**: the same work done by agents on their own, judged
  by a distance, with a person deciding the last word.

## To understand how an instrument plays

- **[engine.md](engine.md)**: how a phrase is played from recorded notes, what
  each number of a style does, and the limits (slurs, the choice of takes).

## To measure, check, compare or fit

- **[tools/README.md](tools/README.md)**: each tool, with its options and what
  its numbers mean. `measure` and `make_data` make an instrument's data;
  `coverage` checks the recordings; `fit_style` and `evaluate` set and judge
  the style; `compare` gives the distance between two renderings; `check` and
  `listen` are for the ear.

## Folders

| Folder | What it holds |
|---|---|
| `engine/` | the playing code: `borrow.cul` (the instrument), `choose.cul` (which recorded note is borrowed), `data_format.cul` (how `data/` is laid out) |
| `tools/` | the tools above, each a program with a `_lib.cul` and a test |
| `sounds/` | the finished instruments, one folder each |
| `skills/make-instrument/` | the skill for making an instrument with Claude |
| `harness/` | the agents' roles, the case files and the clean room for building without the answer |
