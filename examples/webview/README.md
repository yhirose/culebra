# WebView — web-tech desktop GUI from culebra (Spikes 0–3)

A proof that culebra can drive a **native WebView window** through the
`culebra wrap` mechanism, wire it to a **local HTTP server** (Spike 1), serve a
real frontend (`dist/`) that's **baked into a single-file binary** under AOT
while staying **live from disk in dev** (Spike 2), and wrap the whole thing in a
one-call **`Desktop.run`** facade with a UI-driven quit (Spike 3) — the
Tauri-shaped shape, end to end.

This wraps [webview/webview](https://github.com/webview/webview) (MIT),
which renders HTML/CSS/JS in each OS's native engine (WKWebView on macOS,
WebKitGTK on Linux, WebView2 on Windows) — no bundled Chromium.

## Files

| File | Role |
|---|---|
| `webview.h` | Vendored amalgamated single header (pinned, see below) |
| `webview_binding.cpp` | `culebra wrap` declaration: a thin facade over webview's C API, exposed as `Webview.Window` |
| `hello.cul` | Spike 0: opens a 640×480 window with inline HTML (blocks in the event loop) |
| `smoke.cul` | Non-blocking binding check (ctor → setters → drop); safe for CI/headless |
| `spike1.cul` | Spike 1: a local `Http.server` serves the UI + a JSON API; the WebView navigates to it and the page talks back over `fetch()` |
| `dist/` | Spike 2: the frontend as real files (`index.html`, `style.css`, `app.js`) |
| `spike2.cul` | Spike 2: serves `dist/` via `Embed.dir` — live disk in dev, baked into the binary under AOT |
| `desktop.cul` | Spike 3: the `Desktop.run` / `Desktop.quit` facade (server + window + assets + shutdown in one call) |
| `spike3.cul` | Spike 3: the same app as Spike 2, written against the facade |

## Build

```sh
culebra wrap examples/webview/webview_binding.cpp \
    --link "-framework WebKit -ldl" -o webview-culebra        # macOS
```

The binding only needs link flags — `webview.h` sits next to the binding
TU, so its quoted `#include` resolves without an `-I`.

Linux (untested in this spike):

```sh
culebra wrap examples/webview/webview_binding.cpp \
    --link "$(pkg-config --libs gtk4 webkitgtk-6.0) -ldl" -o webview-culebra
```

## Run

```sh
./webview-culebra examples/webview/hello.cul          # Spike 0: interpreter
./webview-culebra --jit examples/webview/hello.cul    # Spike 0: JIT
./webview-culebra examples/webview/smoke.cul          # no window, just the binding
./webview-culebra examples/webview/spike1.cul         # Spike 1: server + WebView
./webview-culebra examples/webview/spike2.cul         # Spike 2: server serves dist/
./webview-culebra examples/webview/spike3.cul         # Spike 3: Desktop.run facade
```

## API surface

```culebra
let w = Webview.Window.new()
w.set_title("Hello")
w.set_size(640, 480)
w.set_html("<h1>It works</h1>")    # or: w.navigate("https://…" / "data:…" / "file://…")
w.run()                            # blocks the GUI thread until terminate()
w.terminate()                      # safe from another thread
```

The window is a resource with culebra's full lifetime model: scope-exit
deterministic drop, idempotent `drop()`, `ClosedError` on use-after-drop.
It is `__nonsendable__` (a GUI handle never crosses an isolate boundary).

## Spike 1 — local server + fetch bridge

`spike1.cul` is the Tauri-shaped shape: a local HTTP server serves the UI
and a small JSON API, and the page reaches it over `fetch()`. The concurrency
model stays intact because the only bridge across the thread boundary is
loopback HTTP — no shared culebra heap crosses it:

- The **server** is started with `srv.listen_async(port, workers: 4)`, which
  binds synchronously and then serves on a background pool. Because the bind is
  synchronous, the call returns ready — no readiness polling. Use a small pool
  (not the default single worker): a browser loads the page over several
  parallel connections, and cpp-httplib holds each keep-alive connection on a
  worker for up to 5s — one worker would serialize the load and leave the window
  blank for seconds.
- The **WebView** runs on the main thread and parks it in the native event
  loop (`run()`), navigating to `http://127.0.0.1:PORT`.
- Closing the window returns from `run()`; `srv.stop()` then stops the server
  and joins its thread (cross-thread-safe — no isolate-cancel polling).
- Handlers run on the background worker thread, so they must be Sendable (the
  examples capture nothing, or a `String`).

`bind`/`eval` (a native, in-process JS↔culebra bridge) is deliberately not
used: the HTTP bridge keeps culebra off the UI thread, so no dispatch /
serialization is needed.

## Single binary + a real dev loop (Spike 2)

`spike2.cul` keeps the UI in real files (`dist/`) and serves them with one line:

```culebra
srv.static("/", Embed.dir("dist"))
```

`Embed.dir(name)` is the whole trick, and it resolves *per backend* with no code
change:

- **Dev** (run from source): it serves the live on-disk directory, resolved
  relative to the entry script. Edit `dist/index.html`, reload the window, and
  the change is there — no rebuild.
- **AOT** (`culebra build`): the directory is walked at build time and its bytes
  are baked into the executable (a generated asset table linked in). The binary
  needs no `dist/` next to it — copy it anywhere.

`srv.static` itself is source-agnostic: it serves a directory (`Embed.dir(...)`),
not "an embed". A plain `srv.static(mount, "dir")` String still serves a live
disk directory at runtime (handy, but not single-binary).

```sh
# dev: live disk
./webview-culebra examples/webview/spike2.cul

# single binary: assets baked in (the build prints what it embedded)
./webview-culebra build examples/webview/spike2.cul -o desktop-app
# culebra build: embedded 3 file(s) (...) from 'dist'
./desktop-app          # runs with no dist/ present
```

`culebra build` composes with the wrap mechanism: because the script names a
wrapped namespace (`Webview`), the build force-loads the wrap archive and its
link flags. The result links only OS-provided frameworks — on macOS `otool -L`
shows just `WebKit`, `CoreFoundation`, `Security`, `libz`, `libc++`, `libobjc`,
`libSystem`; there is no external culebra runtime, and (with the assets baked
in) no external files. One file is the whole app.

## One call (Spike 3)

`desktop.cul` collapses the whole "server + window + assets + shutdown" dance
into `Desktop.run({...})`:

```culebra
import Desktop from './desktop.cul'

Desktop.run({
  title:  "My App",
  size:   [720, 560],
  assets: Embed.dir("dist"),       # dev: live disk / AOT: baked into the binary
  routes: fn(srv) {                # just the API — the facade does the rest
    srv.get("/api/hello", fn(req) { ... })
  }
})
```

`run` starts the server with `listen_async`, opens the window, and stops the
server when it closes. It also registers a built-in `POST /__quit`, so a UI
button can close the app over the HTTP bridge:

```js
fetch("/__quit", { method: "POST" })   // the "Quit" button in dist/index.html
```

The quit route calls **`Webview.Window.quit()`** — a static that terminates the
running window. It's safe to call from the server's worker thread (`webview`'s
terminate is thread-safe) and never touches the non-sendable window handle, so
the UI can close the app without a native JS↔culebra bridge. `Desktop.quit()`
exposes the same for a custom handler.

## Vendoring note

`webview.h` is pinned to a **post-0.12.0 master commit** of webview. The
0.12.0 release fails to compile under the current libc++ (Xcode 26 /
LLVM 22): `user_script` holds a `std::unique_ptr<impl>` to an incomplete
type and the new standard library eagerly instantiates the deleter in a
throwing constructor. master fixed it. The header carries its source
commit at the top; regenerate with webview's `scripts/amalgamate/amalgamate.py`.

## Not in these spikes

A `Dir`-trait-shaped source for `srv.static` (so a zip or an overlay FS could
back it too) is a future generalization. Auto-picking a free port, multiple
windows, and a native app menu are facade niceties left for later. The native
`bind`/`eval`/`init`/`dispatch` JS↔culebra bridge stays out by design (see
above) — see the project roadmap.
