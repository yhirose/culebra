# WebView — web-tech desktop GUI from culebra

culebra drives a **native WebView window** through the builtin `Webview`
namespace, serves the UI from a **local HTTP server**, bakes the frontend
(`dist/`) into a **single-file binary** under AOT while staying **live from
disk in dev**, and collapses the whole thing into one **`Desktop.run`** call
with a UI-driven quit — the Tauri-shaped shape, end to end.

`Webview` is a builtin namespace, ON by default (`CULEBRA_ENABLE_WEBVIEW`):
these run on the stock `culebra` — no build flag, no `culebra wrap` step.
The binding wraps
[webview/webview](https://github.com/webview/webview) (MIT), which renders
HTML/CSS/JS in each OS's native engine (WKWebView on macOS, WebKitGTK on Linux,
WebView2 on Windows) — no bundled Chromium. `culebra build` force-loads it only
when the program references `Webview`/`Desktop`, so other programs pay nothing.
The binding lives in `src/runtime/culebra_rt_webview.cc` + `vendor/webview/`.

## Files

| File | Role |
|---|---|
| `desktop_app.cul` | The recommended shape: `Desktop.run` + `Embed.dir` — a complete desktop app |
| `hello.cul` | Minimal raw `Webview.Window`: one window with inline HTML, no server |
| `smoke.cul` | Non-blocking binding check (ctor → setters → drop); safe for CI/headless |
| `dist/` | The frontend as real files (`index.html`, `style.css`, `app.js`) |

To see the plumbing `Desktop.run` hides (server, window, loopback URL, quit,
shutdown order), read the facade itself: `src/preambles/desktop.cul`.

## Build

`Webview` is ON by default in the culebra build (macOS links `-framework
WebKit`; on Linux it auto-disables unless the `gtk4` + `webkitgtk-6.0` dev
packages are present — `sudo apt install libgtk-4-dev libwebkitgtk-6.0-dev`
on Debian/Ubuntu). Opt out with `-DCULEBRA_ENABLE_WEBVIEW=OFF`.

The released Linux binary is built with Webview OFF on purpose: linking
WebKitGTK puts `libgtk-4.so.1` and `libwebkitgtk-6.0.so.4` in the driver's
`DT_NEEDED`, and it would then fail to start on any headless box. Build from
source with the dev packages installed to get it.

## Run

```sh
culebra examples/webview/desktop_app.cul          # the full app (interpreter)
culebra --jit examples/webview/desktop_app.cul    # same, JIT
culebra examples/webview/hello.cul                # minimal window
culebra examples/webview/smoke.cul                # no window, just the binding
```

## The app in one call

The builtin `Desktop` module (a stdlib preamble, `src/preambles/desktop.cul`)
collapses the whole "server + window + assets + shutdown" dance into
`Desktop.run({...})` — no import needed:

```culebra
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

## Single binary + a real dev loop

`Embed.dir(name)` is the whole trick behind `assets:`, and it resolves *per
backend* with no code change:

- **Dev** (run from source): it serves the live on-disk directory, resolved
  relative to the entry script. Edit `dist/index.html`, reload the window, and
  the change is there — no rebuild.
- **AOT** (`culebra build`): the directory is walked at build time and its bytes
  are baked into the executable (a generated asset table linked in). The binary
  needs no `dist/` next to it — copy it anywhere.

```sh
# dev: live disk
culebra examples/webview/desktop_app.cul

# single binary: assets baked in (the build prints what it embedded)
culebra build examples/webview/desktop_app.cul -o desktop-app
# culebra build: embedded 3 file(s) (...) from 'dist'
./desktop-app          # runs with no dist/ present
```

`culebra build` gates the feature on usage: because the script names `Desktop`
(or `Webview`), the build force-loads the webview feature archive and appends
the OS WebView framework — a program that doesn't links zero WebKit. The result
links only OS-provided frameworks — on macOS `otool -L` shows just `WebKit`,
`libc++`, `libobjc`, `libSystem`; there is no external culebra runtime, and
(with the assets baked in) no external files. One file is the whole app.

## How the bridge works

The concurrency model stays intact because the only bridge across the thread
boundary is loopback HTTP — no shared culebra heap crosses it:

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

## Raw window API

For a window without the facade (see `hello.cul`):

```culebra
let w = Webview.Window.new()
w.set_title("Hello")
w.set_size(640, 480)
w.set_html("<h1>It works</h1>")    # or: w.navigate("https://…" / "data:…" / "file://…")
w.run()                            # blocks the GUI thread until terminate()
w.terminate()                      # safe from another thread

Webview.Window.quit()              # static: terminate the window currently in
                                   # run(), callable from any thread (e.g. an
                                   # HTTP handler)
```

The window is a resource with culebra's full lifetime model: scope-exit
deterministic drop, idempotent `drop()`, `ClosedError` on use-after-drop.
It is `__nonsendable__` (a GUI handle never crosses an isolate boundary).

## Vendoring note

`vendor/webview/webview.h` is pinned to a **post-0.12.0 master commit** of webview. The
0.12.0 release fails to compile under the current libc++ (Xcode 26 /
LLVM 22): `user_script` holds a `std::unique_ptr<impl>` to an incomplete
type and the new standard library eagerly instantiates the deleter in a
throwing constructor. master fixed it. The header carries its source
commit at the top; regenerate with webview's `scripts/amalgamate/amalgamate.py`.

## Known upstream wart (GTK4)

Creating a window and destroying it with nothing in between —
`webview_create` immediately followed by `webview_destroy`, no HTML and no
navigation — segfaults inside GTK. It reproduces against upstream webview on
its own, with no culebra in the process, so it is the vendored header's bug,
not the binding's. Every real path (and `smoke.cul`) sets content first.

## Not here yet

A `Dir`-trait-shaped source for `srv.static` (so a zip or an overlay FS could
back it too) is a future generalization. Auto-picking a free port, multiple
windows, and a native app menu are facade niceties left for later. The native
`bind`/`eval`/`init`/`dispatch` JS↔culebra bridge stays out by design (see
above) — see the project roadmap.
