# Building a desktop app

A walkthrough of building a small desktop app with culebra's `Webview` and
`Desktop` namespaces: a native window, a UI written in plain HTML/CSS/JS, and
a culebra backend behind a local HTTP server — the whole thing shipping as one
binary. It builds the same app that lives at
[`examples/webview/`](../../examples/webview/), one piece at a time; run that
directory's files directly if you'd rather read finished code first.

This guide covers *using* `Desktop`/`Webview`. For the API reference see
[`stdlib.md` §29](../stdlib.md#29-desktop--webview); for how the pieces fit
together internally (the loopback bridge, the `Embed.dir` dev/AOT split,
platform build requirements, and the Ubuntu sandbox note in full) see
[`examples/webview/README.md`](../../examples/webview/README.md).

Contents
--------

- Part 1 — the walkthrough
  1. [A window with one line](#1-a-window-with-one-line)
  2. [Serving your own HTML, CSS, and JavaScript](#2-serving-your-own-html-css-and-javascript)
  3. [Adding an API route](#3-adding-an-api-route)
  4. [Calling it from JavaScript](#4-calling-it-from-javascript)
  5. [Closing the window from the page](#5-closing-the-window-from-the-page)
  6. [Shipping one binary](#6-shipping-one-binary)
- Part 2 — recipes
  - [Picking a fixed port](#picking-a-fixed-port)
  - [Keeping state safe across workers](#keeping-state-safe-across-workers)
  - [Multiple pages in one window](#multiple-pages-in-one-window)
  - [The window won't open on Ubuntu](#the-window-wont-open-on-ubuntu)
- [Where to go next](#where-to-go-next)

## Part 1 — the walkthrough

### 1. A window with one line

The raw binding is `Webview.Window`: create one, give it something to show,
run its event loop.

```culebra
# doctest: skip
let w = Webview.Window.new()
w.set_title("Hello")
w.set_size(640, 480)
w.set_html("<h1>It works</h1>")
w.run()
```

`run()` blocks the calling thread until the window closes — that's the whole
GUI thread's job. `set_html` takes a literal HTML string, so this version has
no server and no separate files; see
[`examples/webview/hello.cul`](../../examples/webview/hello.cul) for the
complete, runnable version. Real apps serve HTML/CSS/JS from files instead —
that's `Desktop.run`, next.

### 2. Serving your own HTML, CSS, and JavaScript

`Desktop.run` is the facade built on top of `Webview.Window`: it starts a
local HTTP server, points a window at it, and blocks until the window closes.
Point `assets:` at a directory and the server serves it at `/`:

```culebra
# doctest: skip
Desktop.run({title: "My App", size: [
  720,
  560,
], assets: Embed.dir("dist")})
```

`Embed.dir("dist")` resolves *per backend* with no code change: run from
source and it reads the live directory on disk next to the entry script (edit
`dist/index.html`, reload the window, see the change); `culebra build` walks
the directory at build time and bakes its bytes into the executable instead.
More on the second half in [§6](#6-shipping-one-binary).

Lay out `dist/` like any static site:

```
dist/
  index.html
  style.css
  app.js
```

[`examples/webview/dist/`](../../examples/webview/dist/) has the finished
three files this guide builds toward.

### 3. Adding an API route

A `routes:` closure registers the app's own endpoints on the underlying
`Http` server (the same server object [§15](../stdlib.md#15-http) describes)
before the window opens:

```culebra
# doctest: skip
Desktop.run({assets: Embed.dir("dist"), routes: fn (srv) {
  srv.get("/api/hello", fn (req) {
    {content_type: "application/json", body: JSON.stringify({message: "Hello from culebra's local server"})}
  })
  srv.post("/api/echo", fn (req) {
    let input = JSON.parse(req.body)
    {content_type: "application/json", body: JSON.stringify({reply: "You said: " + input["text"]})}
  })
}})
```

This is the same shape as
[`examples/webview/desktop_app.cul`](../../examples/webview/desktop_app.cul):
a `GET` that returns a fixed message, a `POST` that echoes back whatever the
page sent (the real file has a third route too, a persisted visit counter —
see [Keeping state safe across workers](#keeping-state-safe-across-workers)
in the recipes below). A handler's return value becomes the response the
same way it does for a plain `Http.server()` — a `String` for
`200 text/plain`, an `Object` with `content_type`/`body`/`status`/`headers`
for anything else.

### 4. Calling it from JavaScript

The page is a normal web page — it reaches the API with `fetch`, exactly as
it would against any other HTTP backend:

```js
async function load() {
  const r = await fetch("/api/hello");
  const d = await r.json();
  document.getElementById("msg").textContent = d.message;
}

async function send() {
  const text = document.getElementById("text").value;
  const r = await fetch("/api/echo", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ text: text })
  });
  const d = await r.json();
  document.getElementById("reply").textContent = d.reply;
}
```

That's `dist/app.js` calling the two routes from [§3](#3-adding-an-api-route).
No native JS↔culebra bridge is involved: the page talks to `127.0.0.1:PORT`
over plain HTTP, the same as it would to any remote server. That also means
the usual web habits carry over unchanged — dev tools, `fetch`, relative
URLs, browser caching.

### 5. Closing the window from the page

A `Desktop.run` app registers `POST /__quit` for you, so a page can end the
app over the same HTTP bridge instead of relying on the frame's own close
button:

```js
document.getElementById("quit").addEventListener("click", () => {
  fetch("/__quit", { method: "POST" });
});
```

For the page to *decide* when it's safe to close — say, after a confirmation
dialog — the native side looks for two conventionally-named properties on
the page's own `window` global before honoring the frame's close button:

```js
document.getElementById("quit").addEventListener("click", requestQuit);

// The native runtime calls this instead of closing immediately, if it's
// defined. Without it, the frame's close button closes the window at once.
window.__culebra_before_close__ = requestQuit;

async function requestQuit() {
  const ok = await confirmDialog("Would you like to quit?");
  if (!ok) return;
  window.__culebra_close__();
}
```

`__culebra_close__()` is the function that actually tells the native window
to close; `__culebra_before_close__` is the opt-in hook that lets the page
gate the call to it. `examples/webview/dist/app.js` has the full version,
including the in-page `confirmDialog` (a plain `Promise`-returning helper —
not `window.confirm()` — so the dialog matches the app's own look). See
["The page's `window` object"](../stdlib.md#the-pages-window-object) in the
stdlib reference for the exact sequencing.

### 6. Shipping one binary

`culebra build` compiles the whole thing — culebra backend and baked-in
`dist/` assets — into one executable:

```sh
culebra build examples/webview/desktop_app.cul -o app
# culebra build: embedded 3 file(s) (...) from 'dist'
./app
```

The binary needs no `dist/` directory next to it — copy it anywhere and run
it. `culebra build` also gates the WebView framework link on whether the
program actually references `Webview`/`Desktop`, so a program that doesn't
use either links none of it.

Run the finished app during development instead of building it every time:

```sh
culebra examples/webview/desktop_app.cul          # interpreter
culebra --jit examples/webview/desktop_app.cul    # same output, through the JIT
```

## Part 2 — recipes

### Picking a fixed port

By default `Desktop.run` tries port `8731`, then falls back to an
OS-assigned free one if that's taken — handy for running two culebra desktop
apps side by side, but it means the page's origin (and so its
`localStorage`) can shift between runs. Pass `port:` to pin it — and to fail
loudly instead of falling back when it's unavailable:

```culebra
# doctest: skip
Desktop.run({assets: Embed.dir("dist"), port: 5173, routes: fn (srv) {
  # ...
}})
```

### Keeping state safe across workers

`Desktop.run`'s server handles requests on a worker pool (`workers:`,
default `4`) — and each worker runs on **its own runtime, its own heap**.
That's why a route handler must be Sendable: it can't capture a mutable
variable or a non-Sendable handle from outside itself (a `SQLite` connection
included), the same rule [§15](../stdlib.md#15-http) and
[§12](../stdlib.md#12-isolate) describe for `Http.server()` and
`Isolate.spawn()` generally. Trying to share one `db` handle across handlers
raises `SendError` before a single request runs.

The fix is the same one those sections give: don't capture the resource,
open it fresh inside the handler that needs it. For a small file-backed
store, that's cheap enough to do on every request:

```culebra
let db = SQLite.open(":memory:")
db.execute("CREATE TABLE notes (id INTEGER PRIMARY KEY, text TEXT)")
db.execute("INSERT INTO notes (text) VALUES (?)", ["buy milk"])
db.execute("INSERT INTO notes (text) VALUES (?)", ["walk the dog"])

let rows = db.query("SELECT id, text FROM notes ORDER BY id")
inspect(rows[0]["text"])  # => 'buy milk'
inspect(rows.size())      # => 2

db.execute("DELETE FROM notes WHERE id = ?", [rows[0]["id"]])
inspect(db.query("SELECT text FROM notes")[0]["text"])  # => 'walk the dog'
db.close()
```

Inside a route, the same three lines — `SQLite.open`, do the work, `close()`
(or just let it drop at the end of the handler) — replace the in-memory path
above with a real file. `examples/webview/desktop_app.cul` applies exactly
this to a persisted visit counter, one more route alongside `/api/hello` and
`/api/echo`:

```culebra
# doctest: skip
srv.post("/api/visit", fn (req) {
  let db = SQLite.open(VISITS_DB)
  db.execute("CREATE TABLE IF NOT EXISTS visits (id INTEGER PRIMARY KEY, n INTEGER)")
  let rows = db.query("SELECT n FROM visits WHERE id = 1")
  let n = if rows.size() == 0 { 0 } else { rows[0]["n"] } + 1
  db.execute("INSERT OR REPLACE INTO visits (id, n) VALUES (1, ?)", [n])
  db.close()
  {content_type: "application/json", body: JSON.stringify({visits: n})}
})
```

`dist/app.js`'s `trackVisit()` calls it on load and shows the result — run
`culebra examples/webview/desktop_app.cul`, close the window, and run it
again: the count picks up where it left off, because it lives in
`visits.db` next to where the app ran, not in the process. SQLite's own file
locking does the work of serializing concurrent writers — there's no
culebra-level coordination to get right beyond not capturing the handle.

### Multiple pages in one window

`assets:` serves a whole directory, so more than one HTML file works exactly
as it would on a static site — no extra culebra code, just a link.
`dist/about.html` is a second page in
[`examples/webview/dist/`](../../examples/webview/dist/), and
`dist/index.html` reaches it the ordinary way:

```html
<p class="nav"><a href="about.html">About this app</a></p>
```

That's it — the server already answers `GET /about.html` because it's
serving the whole directory. Programmatic navigation (no click involved) is
a different layer: it's `Webview.Window.navigate(url)` on the raw window
handle from [§1](#1-a-window-with-one-line), which `Desktop.run` doesn't
expose to `routes:`. There's no multi-window API yet — one `Desktop.run`
call is one window for the process's lifetime; see
[examples/webview/README.md's "Not here yet"](../../examples/webview/README.md#not-here-yet).

### The window won't open on Ubuntu

On Ubuntu 23.10+, `webview: failed to create window` (or a `bwrap` /
`RTM_NEWADDR` crash) is an AppArmor restriction on unprivileged user
namespaces, which WebKitGTK's own sandbox needs — every WebKitGTK embedder
hits it identically, culebra included. An up-to-date Ubuntu (24.04.2+)
already ships the fix; on an older or stripped image, either:

```sh
sudo apt install --reinstall bubblewrap apparmor
```

or grant the namespace to just your binary — see
[examples/webview/README.md's AppArmor section](../../examples/webview/README.md#the-sandbox-on-ubuntu-family-systems-apparmor)
for the exact profile. This is a property of the OS policy, not of the app —
Debian, Fedora, Arch, and non-Ubuntu-patched kernels need none of it.

## Where to go next

- [`stdlib.md` §29](../stdlib.md#29-desktop--webview) — the full
  `Desktop`/`Webview` API reference.
- [`examples/webview/README.md`](../../examples/webview/README.md) — how the
  loopback bridge, single-binary packaging, and per-platform build
  requirements work under the hood; known upstream WebView quirks.
- [`deployment.md` §1](../deployment.md#1-standalone-binary-build-culebra-build) —
  `culebra build` flags, cross-compilation, and trimming what a binary links.
- [`stdlib.md` §15](../stdlib.md#15-http) — the full `Http` server API
  (`routes:` is a plain `Http.server()` underneath).
