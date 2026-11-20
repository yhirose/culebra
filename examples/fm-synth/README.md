# FM Synth

A three-operator FM synthesizer in a native window, as one binary. Play it
from a MIDI keyboard, a wind controller (an EWI), the mouse or the computer's
keys. Every knob changes the sound while you play. Sounds are kept as
presets.

```sh
# From source (the page is read from the checkout)
culebra examples/fm-synth/main.cul

# One binary (the page baked in)
culebra build examples/fm-synth/main.cul -o fm-synth
./fm-synth
```

## Playing it

- **Presets**: pick one at the top, or step through them with ◀ ▶. *edited*
  shows when the sound differs from the preset. **Save** keeps your changes
  (a preset that came with the app is saved under a new name), **Save as…**
  keeps a copy, **Delete** removes one of yours.
- **MIDI devices** appear at the top as they are plugged in, each with a
  **Keys / Wind** switch. A device whose name says it is a wind controller
  (EWI, WX, Aerophone…) starts as Wind.
  - *Keys* plays chords (8 voices), as hard as each key is played, with the
    sustain pedal (CC64) and the mod wheel's vibrato (CC1).
  - *Wind* plays one voice: the last finger held is the note, gliding there,
    and the breath is the volume and opens the brightness. Choose where the
    breath comes from (CC2, CC11, CC7 or aftertouch) next to the devices.
  - Both bend ±2 semitones. **All notes off** stops everything.
- **The keyboard** at the bottom plays with the mouse, or with the keys
  A W S E D F T G Y H U J K (Z and X change the octave), at a velocity of 0.8.

## Making a sound

- **Wiring** chooses how the three operators connect. An operator pointing at
  another *changes its tone* (a modulator); one pointing at the speaker is
  *heard* (a carrier).
- Each operator has:
  - **Volume** (when heard) or **Brightness** (when it changes another): how
    much of it there is.
  - **Pitch**: its frequency as a multiple of the note's. Whole numbers sound
    harmonic, others bell-like or metallic.
  - **Fine tune**: a few cents off, for a slow beating.
  - **Attack, Decay, Sustain, Release**: how its level moves from the key
    down to after the key up, drawn above the knobs. On a modulator this is
    how the tone changes: a short decay gives a bright attack that mellows.
    It is heard when you let go of the knob (a new envelope builds new voices,
    which start the notes held again).
  - Operator 3 also has **Grit**, its feedback, which turns its sine toward a
    sawtooth.
- **Overall**: the volume and the reverb. **Keyboard**: Touch, how much a key
  played harder is louder and brighter (0: every key alike). **Wind
  controller**: how much the breath brightens the sound, and how long the
  glide between notes is.

Double-click a knob to put it back where the preset had it.

## Where things are kept

Your presets and settings live in the platform's folder for a program's data
(`Sys.data_dir("culebra-fm-synth")`): `~/Library/Application
Support/culebra-fm-synth` on macOS, `%APPDATA%\culebra-fm-synth` on Windows,
`~/.local/share/culebra-fm-synth` elsewhere. There is one JSON file per preset
in `presets/`, and `settings.json` holds the sound you played last, each
device's mode and the breath's source. The app starts where you left off.

## How it works

| File | Role |
| --- | --- |
| `main.cul` | `Desktop.run`: serves the page and the `/api/synth` WebSocket |
| `engine/fm.cul` | the sound: a patch made `Synth` expressions |
| `engine/engine.cul` | the playing synth: the MIDI inputs, the notes, the page's connection |
| `engine/presets.cul` | the presets that come with the app, yours, and the settings |
| `ui/` | the page |
| `engine/test_*.cul` | its tests, which the examples sweep runs |

The sound is the program's, not the page's. The page sends the patch over the
WebSocket as it changes. The engine moves `Synth.control`s for knobs it can
move while notes sound (levels, pitches, feedback, volume), and builds the
sound anew for the rest (the wiring, the envelopes, the reverb). The
keyboard's voices are a `Synth.Voices` and the wind controller's voice an
expression of its own, each in a `Synth.Proxy`, so a new sound crossfades in
while the notes held keep sounding. A third box hears both through the reverb
and a limiter. The device runs at a 3 ms period, as `kauai live` does. A
key's velocity is the voice's `Synth.velocity()`, which Touch scales each
operator's envelope by.
