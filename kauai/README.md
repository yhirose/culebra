# Kauai

Kauai is Culebra's music language: a song is a lead sheet and the grooves a
band plays it with, and `culebra play song.kau` plays it. This folder holds
what goes beyond the built-in instruments. Find what you want to do below.

## I want to write a song

Read the language specification, which is the reference for the whole
language (draft, with a Japanese version):

- [`docs/kauai/language.md`](../docs/kauai/language.md), or in Japanese
  [`docs/kauai/language.ja.md`](../docs/kauai/language.ja.md)
- [`docs/guides/kauai-songs.md`](../docs/guides/kauai-songs.md): a walkthrough
  of writing a song, with the English and Japanese versions beside it

Its section 2 is a first song. Play it with `culebra play song.kau`, or write
it to a WAV file with `culebra play --wav out.wav song.kau`.

## I want to play a song with a sound

- **Built-in sounds**: the sounds the language has (`pulse2`,
  `triangle`, `saw` and the rest) need nothing else. See the language
  specification's section on voices.
- **External sounds**: an instrument made from recordings, given to a song as
  `external NAME`. Today there are two:

  | Instrument | `external` | What it is |
  |---|---|---|
  | [`sounds/soprano_sax`](sounds/soprano_sax) | `Sax` | a soprano saxophone, from royalty-free recordings |
  | [`sounds/alto_sax`](sounds/alto_sax) | `Alto` | an alto saxophone, from a jazz player's improvisations (CC0 and CC BY) |

  Play a song with one: `culebra play --sounds kauai/sounds/soprano_sax song.kau`.
- [`docs/guides/kauai-sounds.md`](../docs/guides/kauai-sounds.md): the sounds a
  song plays, shaping them, supplying an `external` instrument, a model, and a
  recorded instrument made with the harness.
  [`engine.md`](engine.md) lists the voice parameters (`breath`, `wander`, `vibrato`).

## I want to make a new instrument

Making an instrument from recordings is a long piece of work: finding and
checking the recordings, measuring them, setting the style, and judging the
result by ear. Start with the route:

- [`making.md`](making.md): the route, in order, with the rules that matter.
- [`skills/make-instrument`](skills/make-instrument): the step-by-step work for
  Claude to follow with you.
- [`harness`](harness): the same work done by agents on their own, judged by a
  distance, with you deciding the last word.

## I want to know how an instrument plays, or to measure one

- [`engine.md`](engine.md): how a phrase is played from recorded notes, what
  each style key does, and the limits.
- [`tools/README.md`](tools/README.md): each measuring, checking, comparing and
  listening tool, with its options.

## Folders

| Folder | What it holds |
|---|---|
| `engine/` | the playing code instruments from recordings share |
| `tools/` | the tools for measuring, checking and comparing |
| `sounds/` | the finished external instruments, one folder each |
| `skills/make-instrument/` | the skill for making an instrument with Claude |
| `harness/` | the agents' roles and the `main.cul` template, for building with one job to an agent |
