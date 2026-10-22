# Listening with the person

The person's ear is the judge, and the scarcest thing in the work. Spend it
only on what measuring cannot settle, and make each listen as easy to judge
as you can.

## Comparing two versions

Use `listen.cul` for every comparison of two versions:

```sh
culebra kauai/tools/listen.cul start joins --out kauai/sounds/alto_sax/work/listen \
  --song kauai/sounds/alto_sax/work/songs/practice.kau --from Legato --to Leaps \
  --sounds kauai/sounds/alto_sax/main.cul --sounds kauai/sounds/alto_sax/work/try_joins.cul
culebra kauai/tools/listen.cul pick joins 2 --out kauai/sounds/alto_sax/work/listen
culebra kauai/tools/listen.cul next joins --out kauai/sounds/alto_sax/work/listen --set breath=0.4
```

What it does for you, and why each matters:

- **Separate files, the same passage at the same time in each.** Two
  versions played one after the other in one file are hard to compare;
  the person wants to switch between two files at the same moment.
- **Named 1 and 2, assigned at random.** Knowing which is new makes it sound
  better. Do not read `NAME.json` before the person answers, and do not
  say anything that hints at which is which ("the second has less breath").
- **The same loudness.** A louder version sounds better, whatever else
  differs. `listen.cul` evens out the integrated loudness; when the change
  *is* a change of loudness (one register quieter), start with
  `--same-gain`, or the change is evened away.
- **The winner is kept.** `next` hears the one kept, the very bytes heard
  before, against it with one more change.

A version made by a change to the code is a second module: copy `main.cul`
to `work/` under a name for the change and edit the copy, or write a small
module there that imports `main.cul` and wraps its voices. A change of a
parameter needs no copy: `--set NAME=VALUE`.

When the person answers, `pick` says which was which; tell them, and write
the answer and their words in `notes.md`. `same` is a fine answer: it keeps
the one before, and says the change is not worth its complexity.

## A passage or the whole song

Cut out the passage the change is about (`--from`, `--to` at its marks),
a few seconds to twenty: short enough to replay and compare. Have a whole
song heard after a few rounds, and whenever the person has not heard the
instrument in its music for a while. A passage can be right while the song
feels mechanical; only the whole tells.

Use songs of different character in turn. An instrument tuned on one tune
learns that tune.

## Giving the person the files

Give the path in a form the person can open on their machine: the folder
and the two file names. On WSL, give the Windows path
(`\\wsl.localhost\<distro>\home\...`) as well, since a player in Windows
opens it from there and a player in WSL may not play smoothly.

## Reading what the person says

People describe sound in their own words, onomatopoeia included. Ask for
it, keep their word, and find what it is before changing anything. What
the words turned out to be, making the soprano saxophone:

| The person said | It was |
|---|---|
| "bwo" (a dull thump in a note) | the note's pitch measured an octave low at its start or end |
| "bari-bari", a crackle at attacks | the first value of a harmonic curve played before the curve starts |
| "buda", a thud as a high note starts | a key or tongue thump, under 450 Hz, in the recorded attack it was given |
| "za-za", "da-da", repeated noise at attacks | sharp air striking the microphone in the recorded attacks (the person's guess; taking out what was not harmonic from the attacks took most of it away) |
| "tan", a short click as a note starts | a note with no recorded attack rising from silence in 3 ms |
| "wah-wah-wah", a wobble in long notes | a long note pieced from parts of several recorded notes, each with its own timbre, so the timbre waved at every join, every 0.3 to 0.5 s |
| "uwa-uwa", "the same note twice" | first, a repeated short note a few cents off the one before, beating against its reverb; what remained after, most likely the reverb returning a short echo of a sharp attack |
| "ton", a knock between notes | dips inside borrowed notes and steps in level at their joins; the smaller ones, a limit on the fine gain that stopped notes fading naturally before a rest |
| "electronic", "an EWI" | long notes too regular, as far as it was found: little wander in pitch, the same vibrato every cycle, harmonics that never flicker, too little breath, and the score's exact timing and dynamics |
| "the high notes are hysterical" | most likely a high note made quieter without being made softer in timbre: in a real player, loudness and brightness move together |
| the soprano lacks its "poa" | the soft-then-full start of a soprano's note, which, once named, was the biggest single step forward |

Words for what the instrument *is* help as much as words for what is
wrong: naming the soprano's "poa" did more than any measurement.
