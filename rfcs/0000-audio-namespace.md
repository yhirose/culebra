# RFC 0000: `Audio`, one namespace for sound

- Status: Accepted
- Author: yhirose
- Date: 2026-09-25

## 1. What

A standalone stdlib namespace `Audio` that owns the audio device and
everything that plays through it: `Audio.tone` (the WASM-4 style APU),
`Audio.Sound` (a one-shot sample), `Audio.Music` (a streamed file) and
`Audio.Stream` (PCM the script synthesises). Canvas and Scene stop
carrying audio of their own; a program that makes sound names `Audio`,
whichever of the two it draws with, or neither.

## 2. Use cases

1. **A game drawn with Canvas.** `rocci-bird` plays three WASM-4 tones
   and `retro-run` plays a step-sequenced chiptune, sampled barks and an
   optional music file. Today all of it lives under `Canvas`:

   ```culebra
   # rocci-bird
   let flap_tone = || Canvas.tone(700, 0, 10, Canvas.PULSE, 870, 10, 5, 3, 20, Canvas.DUTY_QUARTER)

   # retro-run
   Canvas.music(art(MUSIC_FILE), vol: MUSIC_VOL) if MUSIC_FILE != nil
   Canvas.music_volume(Math.round(MUSIC_VOL * gain))   # every frame of the fade-out
   ep_voices[key] = Canvas.Sound.new(pcm_wav(ep_samples(pitch, bark)))
   ```

   The same game drawn with Scene would have to be rewritten against a
   second, different audio API (below).

2. **A game drawn with Scene.** Scene has its own `Sound`, `Music` and
   `Audio`, with different loading, units and pumping:

   ```culebra
   let hit = Scene.Sound.new("assets/hit.wav")   # a path; a bad one is silently silent
   hit.volume(0.5)                               # 0.0..1.0, where Canvas says 0..100
   let bgm = Scene.Music.new("assets/bgm.ogg")
   bgm.play()
   while !view.closing() {
     bgm.update()                                # forget this and the music stops
     # ...
   }
   ```

3. **An emulator's own mixer.** The NES port produces ~735 samples per
   frame from its APU and hands them to a stream. It was written against
   `Canvas.pcm_*`, then `Canvas.Pcm`; Scene's equivalent,
   `Scene.Audio`, takes one sample per call, 735 calls a frame.

4. **A program with no window.** A synthesiser, a music-file player or a
   test that checks a generated waveform plays through the speakers
   without drawing anything. Today it has to reach into `Canvas` (and,
   under AOT, link the window code and SDL) to get a sound out.

## 3. Why existing features fall short

The two graphics namespaces each grew an audio layer, and the two
layers disagree about everything a program can see and several things
it cannot:

| | Canvas | Scene |
| --- | --- | --- |
| loads | bytes (`FS.read`, `Embed`), sniffed: `ValueError` on anything but WAV/MP3/Ogg | a file path; a bad path gives a silent handle, no error |
| volume | `0..100` | `0.0..1.0` |
| music | one global slot, fed from `present()` | a handle per file, fed only by the script's `update()` |
| synthesised PCM | `Pcm.push(samples: Array)`, whole blocks | `Audio.push(s)` / `push2(l, r)`, one sample per call |
| browser | tone / Sound / music through WebAudio | not built for the browser |

Beyond the surface:

- **Two owners of one device.** raylib's audio device is process-global.
  Canvas opens it in its own `ensure_audio` and closes it at exit; Scene
  opens it in another `ensure_audio` and deliberately never closes it,
  because its handles can outlive the `View`. A program that uses Scene
  audio first and then Canvas audio calls `InitAudioDevice` twice, and
  raylib has no guard against that. The other order lets Canvas's exit
  teardown close the device under Scene's live handles.
- **Audio drags in the window.** Canvas's audio and window share one
  translation unit and one AOT feature archive, triggered by the name
  `Canvas`. There is no way to link sound without SDL and the window
  code, and raylib itself is only built when the window or Scene axis
  is on, so a headless build has no sound at all.
- **Small defects that a single owner would fix once:** Scene's pan
  comment says `0..1` with `0.5` centred, but the vendored raylib takes
  `-1..1` with `0` centred; Scene's comment promises FLAC, which the
  vendored raylib config turns off.

None of this can be fixed by aliasing: the device, its teardown and the
AOT archive all need one owner.

## 4. Syntax

### 4.1 The namespace

```culebra
# tone: WASM-4's APU, unchanged. Times are 1/60 s ticks, levels 0..100.
Audio.tone(freq, dur, vol = 100, wave = 0, end_freq = nil, attack = 0,
           decay = 0, release = 0, peak = nil, duty = 2)
Audio.PULSE  Audio.PULSE2  Audio.TRIANGLE  Audio.NOISE  Audio.SAWTOOTH
Audio.DUTY_EIGHTH  Audio.DUTY_QUARTER  Audio.DUTY_HALF  Audio.DUTY_THREE_QUARTER

# A one-shot sample: WAV, MP3 or Ogg bytes. play() restarts it.
let s = Audio.Sound.new(bytes)
s.play()  s.stop()  s.playing()
s.volume(v)  s.pitch(p)  s.pan(p)          # v 0.0..1.0, p 1.0 = normal, pan -1.0..1.0

# A streamed file: MP3 or Ogg bytes. Fed by the runtime, not the script.
let m = Audio.Music.new(bytes, loop = true)
m.play()  m.stop()  m.pause()  m.resume()  m.playing()
m.seek(seconds)  m.volume(v)  m.pitch(p)  m.pan(p)

# PCM the script synthesises, a block at a time.
let st = Audio.Stream.new(rate, channels, buffer)
st.ready()  st.needed()  st.push(samples)  st.submit()  st.latency()
st.play()  st.stop()  st.pause()  st.resume()  st.playing()
st.volume(v)  st.pitch(p)  st.pan(p)

Audio.available()   # false with no audio device: everything plays silently
```

Every handle is released with its last reference, as `Canvas.Sound` is
today; the runtime also releases whatever is left before it closes the
device at exit.

### 4.2 Options

**Option A (proposed): one namespace, handle volumes `0.0..1.0`,
`tone` keeps WASM-4's units.**

- `0.0..1.0` is what raylib, LÖVE, pygame, Godot's linear volume and Web
  Audio's `GainNode` use, and what Scene already uses.
- `tone` is defined as WASM-4's APU, and `rocci-bird` states that its
  tones use "exact WASM-4 parameters"; keeping `0..100` and 60 Hz ticks
  keeps a WASM-4 cart portable line for line. The exception is one
  function whose whole signature is foreign-defined, and the docs say so.
- Music is a handle: several can play (a loop plus a sting), and nothing
  is global. The runtime feeds it (§5), so there is no `update()` to
  forget.
- Sound and Music load **bytes**, as Canvas does: `FS.read`, `Embed.dir`
  and the Playground's virtual files all produce bytes, the format is
  sniffed before any backend runs, and a bad file is a `ValueError` on
  every backend instead of a silent handle.

The use cases in #2, written against it:

```culebra
# rocci-bird
let flap_tone = || Audio.tone(700, 0, 10, Audio.PULSE, 870, 10, 5, 3, 20, Audio.DUTY_QUARTER)

# retro-run
let music = MUSIC_FILE == nil ? nil : Audio.Music.new(art(MUSIC_FILE))
music?.volume(MUSIC_VOL)
music?.play()
music?.volume(MUSIC_VOL * gain)            # the fade-out, still every frame
ep_voices[key] = Audio.Sound.new(pcm_wav(ep_samples(pitch, bark)))

# the NES frontend
let pcm = Audio.Stream.new(apu_mod.SAMPLE_RATE, 1, PCM_BUFFER)
pcm.play()
fn pump_audio() {
  pcm_pending.extend(sys.take_audio())
  let took = pcm.push(pcm_pending)
  pcm_pending = pcm_pending.slice(took, pcm_pending.size())
  pcm.submit()
}
```

**Option B: one namespace, everything `0..100`.** Consistent with
today's Canvas and with `tone`, and `retro-run`'s `Math.round(MUSIC_VOL
* gain)` survives unchanged. But it is the odd one out among the
libraries above, and it quantises a fade to 101 steps, which is audible
on a quiet track.

**Option C: keep audio under Canvas and Scene, share only the C++.**
The runtime gets one device owner and one teardown, and both namespaces
forward to it. This fixes the device hazards without an API change, but
keeps two surfaces for one thing, keeps sound tied to a window under AOT,
and leaves a windowless program no way to play anything.

`Audio.Stream.push` takes an Array only. Scene's per-sample `push(s)` /
`push2(l, r)` go: a stream is fed a block per frame (735 frames at 60 fps
and 44.1 kHz), and one native call per sample is the cost the block form
exists to avoid. Godot's generator keeps both (`push_frame`,
`push_buffer`); one form is enough here, because a single sample is
`st.push([s])`. `pending()` and `dropped()` go with it: a block push
answers how many frames it took and never drops.

## 5. Performance

- **Music is fed by the runtime.** `Audio` runs one feeder thread that
  calls raylib's `UpdateMusicStream` for every live `Music` every few
  milliseconds, the way LÖVE's audio thread updates streaming sources.
  Music no longer stalls when a frame runs long or when a program has
  no frame loop, and neither Canvas's `present()` nor the script pays
  for decoding. The thread exists only once a `Music` is created.
- **AOT gets an `Audio` axis.** A program that names `Audio` force-loads
  a `culebra_rt_audio` archive; one that doesn't links weak no-op stubs,
  as Canvas does today. If raylib's `raudio.c` builds standalone (see
  #8), Canvas and Scene stop carrying miniaudio, and an audio-only
  program stops carrying SDL and the window code.
- `tone`, `Sound`, `Stream` and the per-frame calls cost what they cost
  now: the same natives behind a different namespace object.

## 6. Safety

- **One device owner.** `Audio` opens the device once and closes it
  once, at exit, after releasing every live handle and before the
  window closes. The two hazards in #3 go away because nothing else
  calls `InitAudioDevice` or `CloseAudioDevice`.
- **The feeder thread shares raylib state with the main thread.** Music
  calls from the script (`play`, `seek`, `volume`) and the feeder's
  `UpdateMusicStream` go through one `Audio`-owned mutex; raylib's own
  lock covers only its mixer. The feeder stops and joins before
  teardown.
- **No script code runs on an audio or feeder thread.** Streams stay
  push-based, as today.
- **No device is not an error.** With no device (CI, a server, a browser
  tab before its first click), `available()` is false, nothing plays,
  and every call answers as it does today on such a machine. `Stream`
  keeps its no-device semantics from `PcmBlock`: `push` counts the same
  frames on every backend, so a program that holds back what `push` did
  not take behaves the same everywhere.
- **`PcmStream`'s constructor calls `SetAudioStreamBufferSizeDefault`,**
  a raylib global that also sizes every stream loaded after it. The move
  restores the default after the stream is loaded.

## 7. Can this be done in a preamble (pure .cul), with no core changes?

No. The device, its teardown, the music feeder and the AOT archive are
native. The preamble part is small: `src/preambles/audio.cul` with the
`tone` wrapper, the constants and the four classes, built the way
`Canvas.Sound` and `Canvas.Pcm` are built today.

## 8. Implementation size estimate

Large, but mostly moving code that exists.

**Phase 0, spikes (decide before building):**

1. Build raylib with `SUPPORT_MODULE_RAUDIO=OFF` and compile `raudio.c`
   standalone (`RAUDIO_STANDALONE`) into `culebra_rt_audio`, linked next
   to it. Confirm the window and Scene builds still link, that an
   audio-only AOT binary carries no SDL, and that it plays on Linux
   (PulseAudio), macOS and Windows. If this fails, `Audio` still ships,
   with its archive linking the whole of raylib as Canvas's does now.
2. The feeder thread against raylib's music API on all three OSes,
   including `seek` and `stop` racing a feed.

**Phase 1, the namespace:**

- `include/stdlib/audio.h`: the backend choke, as `canvas.h` is for
  Canvas. Native declarations, the weak no-op stubs for the base AOT
  archive, the silent inline backend for builds without raylib, and the
  browser backend (the `EM_JS` WebAudio calls moved out of `canvas.h`).
- `src/runtime/culebra_rt_audio.cc`: the device, `tone`'s synth and
  mixer thread, the Sound / Music / Stream registries and the feeder,
  moved from `culebra_rt_canvas.cc` and `culebra_rt_scene.cc`.
- `include/stdlib/pcm_block.h` stays, raylib-free, for the silent backends;
  `PcmStream` moves into `culebra_rt_audio.cc`, its only user once Scene's
  audio is gone.
- `bindings.h` natives and rows, `canon_sigs_table.h`, `builtin_var_names`,
  `lazy_namespace_static_name`, the preamble, `kFeatureAxes`, the CMake
  archive and its link fragment, `check_aot_feature_axes.sh`, the
  keyword map, following the new-namespace checklist.
- The Playground's `app.js` / `worker.js` message names move from
  Canvas to Audio.

**Phase 2, callers:**

- `examples/games/rocci-bird.cul`: three `tone` calls and six
  constants.
- `examples/games/retro-run/retro-run.cul` and its README: four `tone`
  call sites (the step sequencer `sound()`, `drum()`, the wave noise,
  the count-in), ten constants, the bark `Sound`, and the optional music
  file with its per-frame fade.
- The copies of both under `site/playground/examples/games/`.
- `tests/test_canvas_module.cul`'s audio sections, `tests/test_canvas.cul`'s
  raw `_Canvas.tone`, `tests/scene_api_test.sh`'s `Scene.Audio` section,
  and `tools/difftest/gen.cul`'s `cvmusic*` cases move to a new
  `tests/test_audio.cul` and to `_Audio.*`.
- Docs: a new `Audio` chapter in `stdlib.md` / `stdlib.ja.md` replacing
  Canvas's Audio / Sound effects / Music / PCM sections and Scene's
  Audio section; the Embed example that plays music; the quick guide,
  handbook and README mentions.
- Outside this repository: the NES frontend's four `pcm` calls.

**Phase 3, removal.** Scene's `Sound`, `Music` and `Audio` go when
`Audio` lands (Scene is experimental). `Canvas.Pcm` never shipped and
goes too. Canvas's `tone`, constants, `Sound` and `music*` shipped in
releases, so they stay one release as documented aliases that forward to
`Audio` (with `music` keeping its one-slot behaviour on top of a
`Music` handle), then go.

## 9. Backend symmetry

The executor, `--jit` and AOT call the same natives, so behaviour,
errors and their timing match: the format sniff raises `ValueError`
before any backend runs, `Stream.push` raises `TypeError` for a
non-number before taking a sample, and a missing device answers the
same everywhere. AOT differs only in what it links, which the `Audio`
axis settles by name as it does for Canvas. The browser is a host
rather than a backend: `tone`, `Sound` and `Music` keep their WebAudio
implementation, and `Stream` stays silent there until it has an
AudioWorklet relay.

## Notes

Decisions, taken 2026-09-25 (the proposed option in each case):

1. Handle volumes are `0.0..1.0` (Option A).
2. `tone` keeps WASM-4's units: `0..100` and 1/60 s ticks.
3. Canvas's shipped audio stays one release as aliases forwarding to
   `Audio`, then goes. Scene's audio and `Canvas.Pcm` go when `Audio`
   lands.
4. `Sound` and `Music` load bytes only.
5. Music is fed by a runtime feeder thread, subject to spike 2.

Spike results, 2026-09-25 (Linux, WSLg PulseAudio):

1. raylib builds with `CUSTOMIZE_BUILD=ON SUPPORT_MODULE_RAUDIO=OFF`
   ("Audio Backend: None") and defines no audio or miniaudio symbol.
   `raudio.c` compiles with `RAUDIO_STANDALONE` once given a `raudio.h`
   (the audio half of `raylib.h`, plus the `RL_MALLOC` family it expects
   the header to define) and a `TRACELOG` that is not `printf` to stdout.
   A program linking that audio-less raylib, SDL3 and standalone raudio
   opens a window, closes it, then plays a stream and a music file, with
   no duplicate symbol. An audio-only program is 561 KB and links no
   SDL; with the window half it is 6.1 MB. macOS and Windows are checked
   by CI once the axis exists.
2. `UpdateMusicStream` decodes outside raylib's lock and
   `StopMusicStream` rewinds the decoder without taking it. Under
   ThreadSanitizer, a feeder thread against random play / stop / seek /
   volume / pause from the main thread gave 13 reports, in the decoder,
   with no Audio-owned lock, and none in 7,597 operations with one lock
   around every Music call and the feeder's update.

Not in this proposal: polyphony for one `Sound` (raylib's
`LoadSoundAlias` would give it), a PCM stream in the browser, audio
input, and effects (filters, reverb).
