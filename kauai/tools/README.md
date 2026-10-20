# Kauai tools

## make_sounds.cul: an instrument's sounds made beforehand

`make_sounds.cul` asks an instrument module for every sound Kauai songs need
from it, the way Kauai asks when it plays them, and keeps each one as a file,
with an index to look each one up in. A program that plays the songs then
hands Kauai those files instead of the instrument, so an instrument that is
slow to make its sounds (one played from recorded notes, say) plays at once,
and in the Playground, where only the program's own folder is served.

```sh
culebra kauai/tools/make_sounds.cul --sounds kauai/sounds/soprano_sax \
  --out made song.kau other_song.kau
```

| Option | What it does |
|---|---|
| `--sounds <module>` | the instrument module: a `.cul` file, or a folder and its `main.cul`, as for `culebra play --sounds` |
| `--out <dir>` | where the index and the sounds go |
| `--name <name>` | the index is `NAME.json` and the sounds go in `NAME/` (`sounds`) |
| `--use FILE=OTHER` | where the songs `use` FILE, read OTHER beside it: a band with an external lead in place of the one the songs name (any number of these) |
| `--ogg` | keep the sounds as Ogg Vorbis, by `ffmpeg` (`--quality`, 0 to 10, default 5) |
| `--pack` | all the sounds one after another in `NAME.bin`, in place of a file each |

The index says, for each instrument the songs asked, what it hears (an
instrument given as `{phrase:}` or `{hit:}`) and where each of its sounds
is, keyed by the JSON of what Kauai handed the instrument:

```json
{"voices": {"sax": {"hears": ["len", "level", "marks", "next", "params"],
                    "sounds": {"[[{\"pitch\":62,\"at\":0.0, ...}],{}]": "made/sax_0.ogg", ...}}},
 "drums": {"kick": {"sounds": {"[{}]": "made/kick_0.wav"}}}}
```

The key is `JSON.stringify([notes, params])` for a voice given as
`{phrase:}`, `[pitch, params]` for one given as a function, `[n, params]`
for a drum given as `{hit:}`, and `[params]` for one given as a function.
A sound is a file name beside the index or, with `--pack`, its
`[offset, size]` in `NAME.bin`.

Kauai weighs an instrument by one sound before it plays (a voice's note at the
middle of the pitches the voice plays, at mf, a second long; a drum's hit at
mf), and reads the loudness of a WAV on every backend but of an Ogg or an MP3
on none. That sound is kept as a WAV even with `--ogg`, so the songs play the
made sounds exactly as loud as the instrument's own. If Kauai stops asking
for that sound as its docs say, the tool stops rather than make sounds that
would play at another level.

### Playing them

A program gives Kauai an instrument of the same shape that answers from the
index. A voice given as `{phrase:}`, packed:

```culebra
# doctest: skip (the files are the program's own)
let made = JSON.parse(dir.read("made.json")).voices.sax
let bytes = dir.read("made.bin")
let sax = {
  phrase: fn (notes, params) {
    let at = made.sounds[JSON.stringify([notes, params])]
    Audio.Sound(bytes.slice(at[0], at[0] + at[1]).to_string())
  },
  hears: made.hears,
}
let song = Audio.Kauai.load("song.kau", dir: dir, voices: {sax: sax})
```

A key that is not there is a song changed since its sounds were made: make
them again. `examples/games/retro-run` plays its laps' saxophone this way,
and falls back to its chip lead when a phrase is missing.
