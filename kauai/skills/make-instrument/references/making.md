# Making the sound

This is a skeleton. Each section names what the soprano saxophone
(`kauai/sounds/soprano_sax/`) does, where to read it in its `main.cul`, and
is to be written out as the measuring tools move into Culebra and as each
new instrument shows what carries over.

## Measuring the recordings

`kauai/tools/measure.cul` reads each note's pitch every 10 ms
(`Audio.pitch`: candidate periods scored by how the spectrum matches their
harmonics and chosen along each run, so a strong harmonic is not taken for
the note), its loudness, where it starts and how it is entered and left
(after a rest, tongued, or slurred). A reed's top notes can have their
fundamental 15 dB under their 2nd harmonic, or few harmonics at all, which
a pitch reading easily takes for a note at a fraction of the pitch: check
the highest notes of a new instrument against the spectrum
(`Audio.spectrum`), the pitch read the lowest of its strong peaks.

To be written: its harmonics' shape and the breath between them.

## Playing from recorded notes

To be written. The soprano borrows, for each note of a phrase, the recorded
note nearest it in pitch, length, loudness, how it is entered and left, and
vibrato, and keeps that note's own moves of pitch and loudness and its
timbre frame by frame. A note longer than the one it borrows is filled out
with steady stretches of long notes near it.

## Joins

To be written: where to cut and how to ease from one borrowed note or part
to the next. The soprano takes up the step between the frames on either
side of a join half on each side, eased in over up to 140 ms, so every move
the borrowed notes make is kept and only a slow slope is added (as joins of
recorded speech are smoothed). Its slow wander of pitch and loudness (the
`wander` parameter) is Perlin noise, which never repeats and has no corners.

## Attacks and breath

To be written: starting a note with the recording's own attack, cleaned of
the air that strikes the microphone; how much breath to hear with the tone,
and how it pulses with the wave.

## The top of the range

To be written: the soprano plays its top notes (from A#5) as overtones of
the fingering an octave down, as a player does to keep them round: borrowed
from a note an octave down, its even harmonics sounding as the note's, so
it keeps the low note's resonances.

## Ideas that carried

These worked for the soprano and are worth trying on the next instrument:

- Perlin noise where a smooth move is wanted that must not sound regular
  (a join, a slow wander).
- Overtones for the highest notes.
- Do not loop one stretch of a long tone: the repeat is heard.
- Move loudness and brightness together; a note made quieter but as
  bright sounds strained.
- Irregularity in long notes (pitch wander, vibrato that differs cycle to
  cycle) keeps them from sounding electronic.
