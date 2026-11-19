# Synth Studio, the desktop app

The [Synth Studio](https://yhirose.github.io/culebra/playground/studio.html)
page in a native window, as one binary: write a sound in `Synth`, play it,
and watch its wave, spectrum and harmonics, with no browser and no network.

```sh
# From source (the page is read from the checkout)
culebra examples/synth-studio/main.cul

# One binary (the page baked in)
culebra build examples/synth-studio/main.cul -o synth-studio --with Synth,Audio,Sys,String,Math
./synth-studio
```

`--with` is needed because the sounds are the editor's, not this program's: a
built binary carries the namespaces its program names, and the ones `--with`
adds for the sources it runs. The list is every namespace those sources name
(a sound's program, and the analysis of its harmonics).

## How it works

| File | Role |
| --- | --- |
| `main.cul` | `Desktop.run`: serves the page and the `/api/run` WebSocket |
| `runner/runner.cul` | runs each Play with `Isolate.spawn_source` and answers the page |
| `runner/test_runner.cul` | its test, which the examples sweep runs |

The page is the browser's own (`playground/studio.html` and what it loads),
not a copy. `main.cul` serves it with its `<meta name="studio-runner">` set
to `socket`, which sends its Plays to this program's server rather than to
the Playground's wasm worker, and with no version (see Limits).

A Play sends the program the page builds (`playground/studio-program.js`) over
the WebSocket. The runner runs it on an isolate of its own; the program's
value is the WAV it renders, which the runner sends on in base64 (encoded by
the runner, so the sources need no namespace beyond `Synth` and `Audio`). A
failure comes back worded as the command line prints it, so the page points
at the editor's line as it does in the browser. The page plays the WAV itself,
which is what the scope and the spectrum read. Stop closes the connection, and
the server drops a run still rendering, so a sound that never ends stops too.

When the lower graph shows the harmonics, the page then sends a second
program on the same connection, its `kind` `"analysis"`. The runner keeps the
last WAV a run rendered and passes it to that program as its argument
(`Sys.argv[0]`); the program's value is the pitch and the harmonics' levels
every 10 ms, which go to the page as JSON. A Play sent while it runs reconnects
first, so the analysis is dropped as a Stop drops a run.

## Limits

- What the program prints goes to the app's standard output, not to the
  page's console. Errors do reach the page.
- The page shows no version: the site stamps it when it copies the page, and
  a program has no version to read.
