# WebView — web-tech desktop GUI from culebra

For a step-by-step walkthrough of building the app in this directory, see
[`docs/guides/desktop-app.md`](../../docs/guides/desktop-app.md). This file
covers the implementation in depth instead — the bridge, packaging, and
per-platform build requirements.

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
| `dist/` | The frontend as real files (`index.html`, `style.css`, `app.js`) |

To see the plumbing `Desktop.run` hides (server, window, loopback URL, quit,
shutdown order), read the facade itself: `src/preambles/desktop.cul`.

## Build

`Webview` is ON by default in the culebra build. Each platform needs its own
engine's headers, and CMake switches the namespace off when they are missing
rather than failing the build:

| Platform | Engine | What the build needs |
|---|---|---|
| macOS | WKWebView | nothing — links `-framework WebKit` |
| Linux | WebKitGTK | `gtk4` + `webkitgtk-6.0` dev packages (`sudo apt install libgtk-4-dev libwebkitgtk-6.0-dev`) |
| Windows | WebView2 | `WebView2.h` (`pacman -S mingw-w64-x86_64-webview2-loader` under MSYS2) |

Opt out with `-DCULEBRA_ENABLE_WEBVIEW=OFF`.

The dev packages are a **build-time** requirement on every platform, not a
run-time one: the binary they produce finds its engine itself.

- **Windows** — the vendored header carries a builtin loader that locates the
  Edge WebView2 runtime through the registry at run time (it uses
  `WebView2Loader.dll` when present, but does not require it), so there is no
  import library and no DLL to ship alongside.
- **Linux** — `src/runtime/webview_gtk_dynload.cc` defines every GTK/WebKit
  symbol the binding reaches and `dlopen`s the engine when a window is created.
  Linking it instead would put `libgtk-4.so.1` and `libwebkitgtk-6.0.so.4` in
  `DT_NEEDED`, and the binary would then fail to *start* — not just fail to open
  a window — on a headless server, in a container, or on a desktop that never
  installed webkitgtk-6.0. `culebra --version` should not need a WebKit stack.
- **macOS** — nothing to arrange: WebKit.framework is part of the OS.

So a machine with no engine gets the same failure everywhere, at the same
point: `Webview.Window.new()` raises `webview: failed to create window`. A
program that never opens a window is unaffected on all three.

`tools/check_webview_dynload.sh` (in the `just test` gate) holds the Linux end
of that: neither the driver nor a `culebra build` output may carry the engine in
`DT_NEEDED`, and neither may export the forwarders — exported, they would
interpose on GTK's and WebKit's own internal calls once dlopen loads them.

### The sandbox on Ubuntu-family systems (AppArmor)

One distro family needs a note; none of it is culebra-specific, and none of it
is baked into the binary. On Debian, Raspberry Pi OS, Fedora, Arch — any Linux
that does not carry Ubuntu's AppArmor patches — unprivileged user namespaces
are simply available, WebKit's sandbox works, and there is nothing to set up.

Ubuntu 23.10+ restricts them: an unconfined binary may *create* one but cannot
use capabilities inside it. WebKit's sandbox (bwrap) then starts and fails
halfway — the window crashes with `bwrap: loopback: Failed RTM_NEWADDR:
Operation not permitted` followed by a `failed to receive credentials` abort.
Every WebKitGTK embedder hits this identically (Tauri apps run the same engine),
as do Chromium and Electron; it is a property of the OS policy, not of the app.

An up-to-date Ubuntu already resolves it for itself: since 24.04.2 the
apparmor/bubblewrap packages ship a `bwrap-userns-restrict` profile, and
desktops with it run WebKitGTK apps out of the box. What still lacks the
profile is the stripped image — a CI runner, a container, an unupdated 24.04.
There, either restore Ubuntu's own profile:

```sh
sudo apt install --reinstall bubblewrap apparmor
```

or grant the namespace to the one binary, the same 3-line shape Ubuntu ships
for Chrome and Discord:

```sh
sudo tee /etc/apparmor.d/my-app <<EOF
abi <abi/4.0>,
include <tunables/global>

profile my-app /path/to/my-app flags=(unconfined) {
  userns,
}
EOF
sudo apparmor_parser -r /etc/apparmor.d/my-app
```

CI's `linux-webview` job runs its window probes under the per-binary form.
(Servers, containers, and programs that never open a window are unaffected
either way — the restriction only bites when the sandbox actually launches.)

## Run

```sh
culebra examples/webview/desktop_app.cul          # the full app (interpreter)
culebra --jit examples/webview/desktop_app.cul    # same, JIT
culebra examples/webview/hello.cul                # minimal window
```

`just smoke-webview` drives the event loop on both backends — the same probe
CI runs. It opens a window; `tests/gui/webview_pending_quit.cul` is the one
that checks the binding without ever showing one.

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
running window. It is safe to call from the server's worker thread (the
binding hops onto the loop thread — see the wart below) and never touches the
non-sendable window handle, so the UI can close the app without a native
JS↔culebra bridge. `Desktop.quit()` exposes the same for a custom handler.

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
w.terminate()                      # from the thread that called run()

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

## Known upstream warts

**`webview_terminate` is not thread-safe, despite saying it is.** Its doc
comment reads "safe to call from another background thread"; only the GTK
backend honours that. Win32 calls `PostQuitMessage`, which posts WM_QUIT to the
*calling* thread, so a quit from an HTTP worker never reaches the loop and the
app hangs. Cocoa calls `NSApplication_stop`, which AppKit allows on the main
thread only. The binding works around it by going through `webview_dispatch`
(genuinely thread-safe on all three) — see `terminate_on_loop_thread` in
`src/runtime/culebra_rt_webview.cc`. **Re-check this when regenerating the
header**: the workaround is downstream, so an upstream fix will not remove it
and a change to `dispatch` semantics would break it silently. `just
smoke-webview` is what catches that.

**GTK4 crashes on a create/destroy pair with no content.**

Creating a window and destroying it with nothing in between —
`webview_create` immediately followed by `webview_destroy`, no HTML and no
navigation — segfaults inside GTK. It reproduces against upstream webview on
its own, with no culebra in the process, so it is the vendored header's bug,
not the binding's. Every real path sets content first.

## Not here yet

A `Dir`-trait-shaped source for `srv.static` (so a zip or an overlay FS could
back it too) is a future generalization. Auto-picking a free port, multiple
windows, and a native app menu are facade niceties left for later. The native
`bind`/`eval`/`init`/`dispatch` JS↔culebra bridge stays out by design (see
above) — see the project roadmap.
