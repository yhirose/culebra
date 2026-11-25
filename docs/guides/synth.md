# Writing a sound with Synth

A guide to `Synth`, the namespace that writes a sound as an expression. It
starts from one sine wave and adds one idea at a time: a shape in time, a
tone, a string, an echo, a stereo room, notes, and a sound changed while it
plays. Each example is a program that runs as it is written. The complete
reference is [`stdlib.md` §46](../stdlib.md#46-synth), and the music language
that can use these sounds is in
[Writing a sound in Kauai](kauai-sounds.md).

## A sound is an expression

`Synth.sine(440.0)` is a sine wave of 440 Hz. It is a *part*: a value that
holds no audio. Parts combine with `+`, `-` and `*`, and `render` works the
expression out and returns the samples.

```culebra
let {sine, render} = Synth
let tone = sine(440.0) * 0.2
let samples = render(tone, 0.5)
println(samples.size())  # => 22050
println(samples.max() <= 0.2)  # => true
```

`let {sine, render} = Synth` brings the names into scope, so they are
written without `Synth.`. `let {*} = Synth` brings in all of them, and the
examples below use it.

Parts never change once they are made, so an expression can be kept and
used again. A part used twice is one part: with `let a = sine(440.0)`, the
sum `a + a` is one wave added to itself, and `sine(440.0) + sine(440.0)` is
two waves.

## Hearing it

`play` plays an expression on the device. A wave never ends on its own, so
give it `secs`:

```culebra
# doctest: skip (it plays on the device)
let {*} = Synth
play(sine(440.0) * 0.2, secs: 1.0)
```

`play` waits until the sound has ended. With `wait: false` it returns at
once, and the sound plays on while the program does other things.

To look at a sound instead of hearing it, `render` gives the samples, and
`Audio.Sound.new(samples).data()` is a WAV that `FS.write` saves.

The [Synth Studio](https://yhirose.github.io/culebra/playground/studio.html)
page plays the last expression of a program and draws its wave, its spectrum
and its harmonics, so a change to a sound can be seen as well as heard. It
runs in the browser, and
[`examples/synth-studio/`](../../examples/synth-studio/README.md) is the same
page in a native window.

## A shape in time: envelopes

A sine at a steady level is a test tone. What makes a sound a bell or a
pluck is how its level moves, and an envelope writes that.
`perc(attack, release)` rises to a peak and falls back to 0, like a struck
sound:

```culebra
let {*} = Synth
let blip = sine(880.0) * perc(0.005, 0.3, peak: 0.3)
println(render(blip).size())  # => 13450
```

A sound ends on its own where its output is 0 for good, and the product with
an envelope ends where the envelope does, so `render` with no `seconds` stops
there. The wave alone never ends, which is why it needed `secs` above.

`env(levels, times)` is the general form: levels joined by segments of the
given lengths, holding the last level after. `curve` bends the segments, and
`"exp"` moves at a steady ratio, as a pitch is heard:

```culebra
let {*} = Synth
let swell = env([0.0, 0.3, 0.1, 0.0], [0.2, 0.3, 0.5])
let drop = xline(2000.0, 100.0, 0.5)  # a pitch falling 4.3 octaves
println(swell.at(0.2))  # => 0.3
println(drop.duration())  # => 0.5
let kick = sine(drop) * perc(0.001, 0.4, peak: 0.8)
println(render(kick).size() > 17000)  # => true
```

Where a part takes a number, an envelope can go instead, and it is that
value's schedule from the start. The kick above is a sine whose pitch falls.

`adsr(attack, decay, sustain, release)` is for a note that is held: up to
the peak, down to the sustain level, and there until it is let go.
`e.hold(secs)` lets it go `secs` from its start:

```culebra
let {*} = Synth
let held = sine(440.0) * adsr(0.02, 0.1, 0.6, 0.3).hold(0.5) * 0.3
println(render(held).size())  # => 35280
```

## A signal is a value

A part given where a number goes is the signal itself. So a part can move
another part's value, and that is how the common effects are written.

A **tremolo** moves the level. A sine from -1 to 1 becomes a level from 0 to
1 with `0.5 + 0.5 * sine(5.0)`, or by `range`:

```culebra
let {*} = Synth
let tremolo = sine(330.0) * sine(5.0).range(0.3, 1.0) * 0.3
println(render(tremolo, 1.0).size())  # => 44100
```

A **vibrato** moves the pitch. `hz` turns a note number into Hz (69 is 440 Hz,
a semitone is a twelfth of an octave), and given a signal it answers a
signal, so `sine(5.0) * 0.3` becomes a wobble of 0.3 semitones:

```culebra
let {*} = Synth
let vibrato = sine(hz(69.0 + sine(5.0) * 0.3)) * 0.3
println(render(vibrato, 1.0).size())  # => 44100
```

**FM** moves a wave's phase with another wave. The ratio of the two
frequencies decides whether the result is harmonic. 616 / 440 is 1.4, which
puts the partials between the harmonics, and a depth that dies away faster
than the sound makes a bell that turns pure as it fades:

```culebra
let {*} = Synth
let depth = perc(0.001, 1.2, peak: 3.0)
let bell = sine(440.0, phase: sine(616.0) * depth) * perc(0.002, 2.5, peak: 0.3)
println(render(bell).size() > 100000)  # => true
```

## Tone: waves and filters

`sine`, `saw`, `square` and `triangle` are the four waves, `pulse(freq,
width)` is a square with a width, and `noise()` is white noise. A saw has
every harmonic and a sine has one, so a filter has more to work on in a saw.

`lowpass`, `highpass` and `bandpass` are methods of a part, taking `cutoff`
and `q`. A high `q` rings at the cutoff. The cutoff is a value like any
other, so an envelope sweeps it:

```culebra
let {*} = Synth
let cutoff = env([100.0, 5000.0, 100.0], [2.0, 2.0], "exp")
let sweep = saw(55.0).lowpass(cutoff, q: 6.0) * 0.2
println(render(sweep, 4.0).size())  # => 176400
```

Two saws a few cents apart beat against each other and sound wider than
one. An Array where a part goes is a channel for each item, and `mix` adds
the channels back into one part:

```culebra
let {*} = Synth
let pad = saw([110.0, 110.6]).mix().lowpass(700.0 + sine(0.4) * 400.0, q: 2.0) * 0.2
println(render(pad, 1.0).size())  # => 44100
```

## A string

`pluck` is a string struck by its input: a short burst of noise rings in a
delay as long as one period. `ring` is how many seconds it takes to die
away. Written with `|>`, the burst is the receiver:

```culebra
let {*} = Synth
let string = noise() * perc(0.001, 0.01) |> pluck(hz(57.0), ring: 1.5)
println(render(string).size() > 66000)  # => true
```

Three strings struck one after another make a chord. `delay(time)` is the
part as it was `time` seconds before, so it places each string in time:

```culebra
let {*} = Synth
let string = fn (note, at) {
  noise() * perc(0.001, 0.01) |> pluck(hz(note), ring: 2.0).delay(at)
}
let chord = [string(57.0, 0.0), string(60.0, 0.15), string(64.0, 0.3)].mix() * 0.5
println(render(chord).size() > 88000)  # => true
```

## An echo is a loop

`feedback(|x| ...)` writes a loop. `x` stands for what the expression
answers, heard through a delay, so `x.delay(0.33) * 0.5` is the sound a third
of a second ago at half the level:

```culebra
let {*} = Synth
let blip = sine(880.0) * perc(0.001, 0.08, peak: 0.4)
let echo = feedback(|x| blip + x.delay(0.33) * 0.5)
println(render(echo, 2.0).size())  # => 88200
```

A way from `x` to the answer with no delay on it is a `ValueError`, since
the sound would depend on itself at the same instant. A loop is worked out a
sample at a time, so it costs several times what the same parts cost with no
loop.

A loop is also how a filter that `Synth` has no name for is written.
`y.delay(0.0)` is the answer one sample back, so a one-pole low-pass
`y = 0.05 x + 0.95 y[-1]` is:

```culebra
let {*} = Synth
let x = saw(220.0) * 0.8
let smoothed = feedback(|y| x * 0.05 + y.delay(0.0) * 0.95)
println(render(smoothed, 0.1).size())  # => 4410
```

## Stereo and a room

An Array of two where a part goes is stereo: the first is the left and the
second the right. `p.pan(pos)` places one channel from -1 (left) to 1
(right) at equal power. `render` with `channels: 2` answers
`[left, right]`:

```culebra
let {*} = Synth
let wide = [saw(110.0).pan(-0.5), saw(110.77).pan(0.5)].mix() * 0.1
let sides = render(wide, 0.5, channels: 2)
println((sides.size(), sides[0].size()))  # => (2, 22050)
```

`reverb(mix, room, damp)` puts a part in a room, and makes stereo from it.
`mix` is how much of the sound is the room's, `room` how long it rings and
`damp` how soon its highs go. This is a chord of four notes, each two saws
a little apart on either side, through a slowly moving filter and a large
room:

```culebra
let {*} = Synth
let voice = fn (note) {
  let f = hz(note)
  [saw(f).pan(-0.7), saw(f * 1.004).pan(0.7)].mix()
}
let chord = [48.0, 55.0, 60.0, 63.0].map(voice).mix()
let cutoff = 400.0 + (sine(0.25) + 1.0) * 1200.0
let swell = adsr(0.8, 0.5, 0.7, 1.5).hold(4.0)
let wide = chord.lowpass(cutoff, q: 1.5) * swell * 0.08 |> reverb(mix: 0.35, room: 0.8)
println(render(wide, channels: 2)[0].size() > 200000)  # => true
```

## Notes

`Voices(count, builder)` plays notes. `builder(freq)` returns the
expression of one voice, with `freq` a signal in Hz that each note sets, and
it is called once for each of the `count` voices. A gated envelope (`adsr`)
starts when a note starts and is let go when the note ends:

```culebra
let {*} = Synth
let lead = Voices(4, |f| sine(f) * adsr(0.01, 0.1, 0.6, 0.2) * 0.2)
lead.sequence([60, 64, 67, nil, 72], step: 0.25, len: 0.2)
println(lead.ends())  # => 1.4
println(render(lead, 1.4).size())  # => 61740
```

`sequence` puts the notes `step` seconds apart, each `len` long, and a `nil`
is a rest. `note_on(note, at)` and `note_off(note, at)` place one note at a
time. A note goes to a voice that is free, and when none is, to the one that
has sounded longest, which fades out over 5 ms first.

A voice's graph is made once, not for each note, so what keeps a state
carries over from one note to the next. `freq.lag(0.1)` glides from the
last note to this one, a portamento.

## Changing a sound while it plays

`control(name, init)` is a value that can be set from outside. `play` with
`wait: false` returns a handle, and `h.set(name, v)` moves the control:

```culebra
let {*} = Synth
let h = sine(control("freq", 440.0)) * perc(0.01, 1.0) * 0.3 |> play(wait: false)
println(h.ends())  # => 1.01
h.set("freq", 660.0, at: 0.5)  # from half a second in
h.set("freq", 330.0, secs: 0.2)  # now, over a fifth of a second
h.stop()
```

`h.set` takes `secs` for a glide in a straight line and `at` for a time on
the sound's own clock. A control given `lo` and `hi` is kept between them.

`Proxy` is a box whose sound is replaced while it plays, and which other
expressions read as a signal. `box.set(expr)` puts a new sound in, and the
old one fades out over `fade` seconds as the new one fades in. A sound that
reads a box hears the replacement at once, with no graph built again, so a
box is how a sound is edited live:

```culebra
let {*} = Synth
let lfo = Proxy()
lfo.set(sine(5.0))
let box = Proxy(fade: 0.05)
box.set(saw(110.0 + lfo * 3.0).lowpass(800.0) * 0.2)
box.play()
lfo.set(sine(7.0))  # the vibrato quickens where box reads it
box.set(saw([110.0, 110.77]).mix().lowpass(1200.0) * 0.1)  # crossfades
box.stop()
lfo.stop()
```

A sound that reads a box has no end of its own, and is heard only as it
plays. `render` of it is a `ValueError`.

## Playing from a MIDI controller

A sound played from a keyboard or a wind controller is moved by the
messages its input reads ([`MIDI`](../stdlib.md#41-midi)): a note on and
off go to a `Voices`, and a bend, a controller (CC) or aftertouch to a
`control`, set on the handle `play` gave. A control of one name is one
value in a whole expression, so one in each voice's builder is set for
every voice at once, as is a part made outside the builder:

```culebra
let {*} = Synth
let bend = control("bend", 1.0)  # a ratio to each note's freq
let vibrato = sine(5.5) * control("wheel", 0.0).lag(0.05) * 0.01
let keys = Voices(8, |f| saw(f * bend * (1.0 + vibrato)).lowpass(2000.0) * adsr(0.01, 0.2, 0.7, 0.3) * velocity() * 0.1)
let h = play(keys, wait: false)
println(h.ends())  # => nil
h.stop()
```

```culebra
# doctest: skip (it needs a MIDI device)
let input = MIDI.Input.new(MIDI.inputs()[0].id)
for m in input.messages() {
  cond {
    m.kind == "note_on" => keys.note_on(m.note, velocity: m.velocity),
    m.kind == "note_off" => keys.note_off(m.note),
    m.kind == "bend" => h.set("bend", 2.0 ** (m.bend * 2.0 / 12.0)),  # 2 semitones at full
    m.kind == "cc" && m.cc == 1 => h.set("wheel", m.value),
    _ => {},
  }
}
```

A controller moves in steps, a 128th of its range at a time, each a jump
heard as a click or a zipper on a gain or a cutoff. `.lag(t)` after the
control glides each step over about `t` seconds: 0.02 to 0.05 is short
enough to follow a player and long enough to hide the steps.

A wind controller plays one note at a time, and its breath (CC 2 on most) is
how loud it is. One voice is enough: the note is a control that glides, and
the breath is the envelope:

```culebra
let {*} = Synth
let breath = control("breath", 0.0).lag(0.02)
let freq = hz(control("note", 60.0)).lag(0.05)
let h = saw(freq).lowpass(300.0 + breath * 4000.0) * breath * 0.3 |> play(wait: false)
println(h.ends())  # => nil
h.stop()
```

```culebra
# doctest: skip (it needs a MIDI device)
for m in input.messages() {
  cond {
    m.kind == "note_on" => h.set("note", m.note),
    m.kind == "cc" && m.cc == 2 => h.set("breath", input.control(2)),
    _ => {},
  }
}
```

A wind controller playing legato sends the new note's note on before the
old note's note off, so a note on that only moves the pitch, with the note
off left alone, plays legato, and the breath alone ends the sound.
`input.control(2)` is the breath at 14 bits when the controller sends them
(CC 2 with CC 34), where the message's `value` holds only its own 7.

## A sound for a Kauai song

A Kauai song names the sound each voice plays, and a program can supply
one. `Synth` is how such a sound is written: an external instrument is a
function that answers an `Audio.Sound` for a pitch, and a `render` with no
`seconds` makes the sound as long as it takes to die away, so the song, not
the instrument, says how long a note is held:

```culebra
let {*} = Synth
let plucked = {
  note: |pitch, params| Audio.Sound(render(noise() * perc(0.001, 0.01) |> pluck(hz(pitch), ring: params.ring))),
  knobs: {ring: {default: 0.5, min: 0.1, max: 2.0}},
}
println(render(noise() * perc(0.001, 0.01) |> pluck(220.0, ring: 0.5)).size())  # => 22535
```

[Writing a sound in Kauai](kauai-sounds.md) shows how a song uses it.

## Where to go next

- [`stdlib.md` §46](../stdlib.md#46-synth) lists every function and method
  with its range and its errors, how a sound's end is worked out, and the
  rules for a `Voices` and a `Proxy`.
- `culebra docs -g 'Synth'` prints the same sections from the binary.
- The Studio's sample programs (`site/playground/studio-samples/`) are
  short sounds to read and change: an FM bell, plucked strings, an echo, a
  wide chord.
