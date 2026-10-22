# Kauai tools

## listen.cul: two ways of a passage, heard as 1 and 2

`listen.cul` renders one passage of a song two ways that differ in one
thing, and writes them as two WAVs named only 1 and 2, at the same
loudness. Which is which is kept in a record and said once the listener
has picked one. The one picked is then heard against the next way to try,
round after round, as an optician keeps the lens that reads better and
offers the next.

```sh
culebra kauai/tools/listen.cul start breath --song cruise.kau \
  --from Theme --to Bridge --sounds kauai/sounds/soprano_sax --set breath=0.5
culebra kauai/tools/listen.cul pick breath 2
culebra kauai/tools/listen.cul next breath --set breath=1
```

`start NAME` renders the two ways. A way is a song, the instrument module
that makes its `external` sounds (as `culebra play --sounds` takes one),
and parameters set over the song's. `--song` and `--sounds` given once are
both ways'; given twice, the first is the one before and the second the
new one. `--set NAME=VALUE` is the new one's: every instrument of its
module plays with the parameter at that value, whatever the song's band
line and directions set it to. `--from` and `--to` choose the passage, each
a mark's name, seconds, or minutes and seconds (`1:23.5`), as
`culebra play --from` and `song.render(from:, to:)` take them.

| Command | What it does |
|---|---|
| `start NAME` | renders the one before and the new one, and writes round 1 |
| `pick NAME 1`, `2` or `same` | records the one picked, and says which was which; `same` keeps the one before |
| `next NAME` | the one kept with one more change (`--song`, `--sounds` or `--set`, the parameters set on top of the ones it has), heard against the one kept, as the next round |

| Option | What it does |
|---|---|
| `--out <dir>` | where the WAVs and the record go (`listen`) |
| `--same-gain` | with `start`, hear the two as they are made instead of at the same loudness: for a change to how loud something is |

Round R is `NAME-R-1.wav` and `NAME-R-2.wav` in `--out`, each round under
names of its own, so a player can keep the last ones open. Which one is
the one before is chosen at random each round. The record, `NAME.json`,
holds the passage and, for each round, the two ways in slots `one` and
`two`, which slot holds the one before (`before`), how many dB louder the
new one was before they were evened out (`differ`, `nil` where one is
silent) and what was picked (`picked`). The one kept is not rendered again:
the next round hears the samples it was heard as.

The two are heard at the same integrated loudness
(`Audio.integrated_loudness`, ITU-R BS.1770), measured from `--from` on: the
new one is made as loud as the one before, since a louder version tends to
sound better whatever else differs. Kauai has weighed each instrument by one
sound already, so what is evened out is how the passage plays. Where either
would then pass full scale, both are lowered alike. A song is rendered as
`culebra play --wav` renders it, on the VM, the two side by side.

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
