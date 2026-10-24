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
| "buuun", a siren at every interval of a slur (the alto) | the pitch glided between slurred notes with a 25 ms time constant; a change of fingering moves it in about 4 ms, with a brief dip of the air |
| "the EWI's glide", a downbend (the alto) | the pitch and the loudness slid down on one curve; a fall's loudness is greatest at the start and thins out softly while the pitch gathers speed |
| "bu..." (a blunt knock), a fall (the alto) | the measured landing of eight falls, a push of 8 dB and a collapse in 0.1 s, put in at full strength; the measure said what changes, not how hard |
| "flat", "nopperi", after the rest is fixed | what is still the same everywhere: the slur's join, the breath, small moves of level and pitch |
| the soprano lacks its "poa" | the soft-then-full start of a soprano's note, which, once named, was the biggest single step forward |

Words for what the instrument *is* help as much as words for what is
wrong: naming the soprano's "poa" did more than any measurement.

## From a word to a number, and back to the ear

When the person's word does not say what is wrong, this found it, and kept
the next change from being worse than the last (the alto's downbend took five
tries to make right; the method is what shortened them):

1. **Ask where.** The time in the file ("0:30 and 1:00"), not the feeling
   alone. A passage is found by counting bars; `check.cul --from --to --all`
   lists what measuring finds in a stretch.
2. **Play that note alone.** One note with the articulation (a C5 held 1 s
   with `\fall`) in a throwaway file, so that a change can be seen without
   the song around it.
3. **Measure the real thing and ours with the same tool.** `measure.cul` on
   the recordings and on what the instrument plays (a scale, every semitone
   held, the one note). Compare what a word can mean: the spectrum in Hz
   (band by band), the breath against the tone in each band, the attack
   (how many ms to within 3 dB), the level over time at chosen points, the
   centroid. Where a recorded fall lands, how loud, how bright, how long its
   tail: read it off the recording rather than guess.
4. **Change one thing, at the strength the person last called good, not
   the measured one.** A tendency measured on one player is a hint of what
   changes, not of how hard: the alto's fall shaped on eight measured falls
   was a blunt "buh" and had to be given up. Keep the version the person
   called good in a commit, so that going back is one step, and write the
   change's strength in a constant that can be turned.
5. **Hear it, and say what was heard** ("better, the timbre is still too
   flat"), then go back to 1 with the next word.

A number that moved is not a sound that improved: the ear decides, and
the measuring is for finding where to look and for telling that a change did
what it was meant to.
