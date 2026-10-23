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

It also reads each frame's 40 harmonics along the pitch
(`Audio.harmonics`) and the breath, what is left without them, in five
octave bands from 500 Hz, against the harmonics' power: the soprano's notes
hold the same numbers, measured the same way. The harmonics are read with
the pitch moving across each 20 ms window as it moves between frames;
held, a vibrato of 30 cents leaves its own harmonics in the rest 30 dB
under them, about as loud as a saxophone's breath (in the soprano's
recordings, whose breath is louder, it read the vibrato notes' lowest band
about 1 dB high). The soprano keeps each harmonic's level as its log averaged over 150
ms, scaled to unit power: a weak harmonic's level wavers from frame to
frame, and that average sits a dB or so under the frames' median.

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

`kauai/tools/make_data.cul` keeps each tongued or rested note's attack, 150
ms of the recording from 30 ms before its onset, cleaned of the air that
strikes the microphone: the recording's steady noise taken out, then each
frame's bins kept only about the harmonics of its own pitch (an attack can
start far from its note's pitch, so the pitch is read frame by frame). The
air left in was heard as a rushing "zah-zah" at each tongued note of the
soprano. It also reads how the breath pulses with the tone's wave in each
band (the noise between the harmonics swells and fades once a period, more
in the high bands), which the soprano makes its breath with.

To be written: grafting the attack onto the borrowed note; how much breath
to hear with the tone.

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
