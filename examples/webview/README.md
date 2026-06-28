# WebView — web-tech desktop GUI from culebra (Spike 0)

A proof that culebra can drive a **native WebView window** through the
`culebra wrap` mechanism — the first layer of a Tauri-shaped, single-binary
desktop app (local HTTP server + embedded assets land in later spikes).

This wraps [webview/webview](https://github.com/webview/webview) (MIT),
which renders HTML/CSS/JS in each OS's native engine (WKWebView on macOS,
WebKitGTK on Linux, WebView2 on Windows) — no bundled Chromium.

## Files

| File | Role |
|---|---|
| `webview.h` | Vendored amalgamated single header (pinned, see below) |
| `webview_binding.cpp` | `culebra wrap` declaration: a thin facade over webview's C API, exposed as `Webview.Window` |
| `hello.cul` | Opens a 640×480 window with inline HTML (blocks in the event loop) |
| `smoke.cul` | Non-blocking binding check (ctor → setters → drop); safe for CI/headless |

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
./webview-culebra examples/webview/hello.cul          # interpreter
./webview-culebra --jit examples/webview/hello.cul    # JIT
./webview-culebra examples/webview/smoke.cul          # no window, just the binding
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

## Vendoring note

`webview.h` is pinned to a **post-0.12.0 master commit** of webview. The
0.12.0 release fails to compile under the current libc++ (Xcode 26 /
LLVM 22): `user_script` holds a `std::unique_ptr<impl>` to an incomplete
type and the new standard library eagerly instantiates the deleter in a
throwing constructor. master fixed it. The header carries its source
commit at the top; regenerate with webview's `scripts/amalgamate/amalgamate.py`.

## Not in this spike

`bind`/`eval`/`init`/`dispatch` (the native JS↔culebra bridge), the local
HTTP server (developed separately), embedded assets, and the `culebra build`
single-binary AOT path. The HTTP-only bridge (`fetch` to a localhost server)
is the planned integration shape — see the project roadmap.
