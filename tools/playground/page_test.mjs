// Page.socket in the Playground: the committed JSPI build with the worker's
// half (site/playground/web-page.js, worker-wake.js) and a page that answers
// as fm-synth-host.js does. A program opens the page's socket and the two
// talk both ways; a wait for the page ends as soon as the page sends, and a
// MIDI message ends one too, which is what lets the FM Synth's loop read
// both; the page going away reads as Closed; a second socket is refused; the
// end of a run closes the page's side; a socket touches only itself (one let
// go after the next opened, one the page went away from); and without JSPI
// there is no socket.
// Then the FM Synth's engine (examples/fm-synth/browser.cul) serves the page
// as it serves the desktop app's window.
//
//   node tools/playground/page_test.mjs

import fs from "node:fs";
import path from "node:path";
import { root, loadPlayground } from "./wasm.mjs";

globalThis.self = globalThis;
const site = path.join(root, "site/playground");
const { workerMidi } = await import(path.join(site, "web-midi.js"));
const { workerPage } = await import(path.join(site, "web-page.js"));
const { workerWake } = await import(path.join(site, "worker-wake.js"));
const wake = workerWake(globalThis);
const onPage = workerPage(globalThis, wake);

// A wait that never ends leaves the program and the page each waiting for
// the other: that fails here rather than at the runner's timeout.
setTimeout(() => {
  console.error("page FAIL: the test did not end in 60 s (a wait for the page never ended)");
  process.exit(1);
}, 60_000).unref();

let bad = 0;
function check(ok, what) {
  if (!ok) {
    console.error("page FAIL: " + what);
    bad++;
  }
}

// The page's MIDI: one input, granted at once.
const onMidi = workerMidi(globalThis, (m) => {
  if (m.type !== "midiAccess") return;
  setTimeout(() => {
    onMidi({ type: "midiPorts", inputs: [{ id: "in-1", name: "EWI-USB" }], outputs: [] });
    onMidi({ type: "midiAccess", ok: true, sysex: false });
  }, 5);
}, wake);

const playground = await loadPlayground("full");
const post = globalThis.postMessage;
// What the program tells the page, in order; `answer` is how the page
// answers what the program sends (later, as a page would).
let told = [];
let answer = () => {};
globalThis.postMessage = (m) => {
  if (m && m.type && m.type.startsWith("page")) {
    told.push(m.type === "pageSend" ? "send " + m.text : m.type);
    if (m.type === "pageSend") setTimeout(() => answer(m.text), 50);
  } else {
    post(m);
  }
};
const say = (text) => onPage({ type: "pageMessage", text });

// The page answers "hello" and plays a note for "midi"; it goes away on
// "bye". Each wait has 5 s, so a run well under that was woken each time.
answer = (text) => {
  if (text === "hello") say("hi");
  else if (text === "midi") onMidi({ type: "midi", id: "in-1", data: new Uint8Array([0x90, 60, 100]), at: performance.timeOrigin + performance.now() });
  else if (text === "bye") onPage({ type: "pageGone" });
};
say("before it listens");
const started = performance.now();
const { out, failed } = await playground.run("/work/page.cul", `fn show(r) {
  match r {
    Message(text) => "Message {text}",
    Empty() => "Empty",
    Closed() => "Closed",
  }
}
let page = Page.socket()
page.set_timeout(5000)
page.send("hello")
println(show(page.try_receive()))
let ewi = MIDI.Input("in-1")
page.send("midi")
println(show(page.try_receive()))
println(ewi.read().map(|m| m.kind).join(", "))
try {
  Page.socket()
} catch e {
  println(e.message)
}
page.send("bye")
println(show(page.try_receive()))
println(page.is_open())
println(page.send("after"))
`);
const took = performance.now() - started;
check(!failed, "the program failed:\n" + out);
check(took < 3000, `the run took ${Math.round(took)} ms: a wait did not end when the page or a note came`);
const want = [
  "Message hi",
  "Empty",
  "note_on",
  "Page.socket: the page has a socket open already",
  "Closed",
  "false",
  "false",
].join("\n") + "\n";
check(out === want, `the program printed\n${out}instead of\n${want}`);
check(told.join(" | ") === "pageOpen | send hello | send midi | send bye | pageClose",
      "the page was told " + JSON.stringify(told));

// A run that leaves its socket open closes the page's side as it ends, and
// the next run opens it anew; a message sent while no one listens is not kept.
told = [];
answer = () => {};
const left = await playground.run("/work/page.cul", `let page = Page.socket()\npage.send("left open")\n`);
check(!left.failed, "the run leaving its socket open failed:\n" + left.out);
check(told.join(" | ") === "pageOpen | send left open | pageClose", "a run leaving its socket open told " + JSON.stringify(told));
say("to no one");
check(globalThis.__page.queue.length === 0, "a message to a closed socket was kept");

// A socket touches only itself: one closed and let go after the next opened
// leaves the new one open, and one the page went away from leaves room for
// the next while it is still held.
told = [];
answer = (text) => {
  if (text === "again") say("heard");
  else if (text === "bye") onPage({ type: "pageGone" });
};
const reopened = await playground.run("/work/page.cul", `mut page = Page.socket()
page.close()
page = Page.socket()
page.set_timeout(5000)
page.send("again")
match page.try_receive() {
  Message(text) => println(text),
  _ => println("nothing"),
}
`);
check(!reopened.failed && reopened.out === "heard\n", "the run opening its socket again printed " + JSON.stringify(reopened.out));
check(told.join(" | ") === "pageOpen | pageClose | pageOpen | send again | pageClose",
      "the run opening its socket again told " + JSON.stringify(told));
// What the page sent before it went is still read, then the socket closes.
told = [];
answer = (text) => {
  say("last");
  onPage({ type: "pageGone" });
};
const gone = await playground.run("/work/page.cul", `let page = Page.socket()
page.send("bye")
println(page.receive())
println(page.receive())
let next = Page.socket()
println(next.is_open())
`);
check(!gone.failed && gone.out === "last\nnil\ntrue\n", "the run opening a socket after the page went printed " + JSON.stringify(gone.out));
check(told.join(" | ") === "pageOpen | send bye | pageOpen | pageClose",
      "the run opening a socket after the page went told " + JSON.stringify(told));

// The FM Synth's engine serving the page, staged as worker.js stages the
// catalog's entry: the patch comes first, a key played is told back as
// sounding, a preset loads, and when the page goes the engine keeps what was
// played last and ends the run.
const files = ["browser.cul", "engine/engine.cul", "engine/fm.cul", "engine/presets.cul"];
for (const f of files) {
  const at = "/work/examples/fm-synth/" + f;
  playground.FS.mkdirTree(path.dirname(at));
  playground.FS.writeFile(at, fs.readFileSync(path.join(root, "examples/fm-synth", f), "utf8"));
}
const seen = [];
const send = (msg) => say(JSON.stringify(msg));
globalThis.postMessage = (m) => {
  if (!m || m.type !== "pageSend") return post(m);
  const msg = JSON.parse(m.text);
  if (msg.type === "patch") {
    seen.push("patch " + msg.patch.name);
    if (msg.patch.name === "Electric Piano") send({ type: "key", on: true, note: 60 });
    else if (msg.patch.name === "Bell") onPage({ type: "pageGone" });
  } else if (msg.type === "played" && msg.keys.join() === "60" && !seen.includes("played 60")) {
    seen.push("played 60");
    send({ type: "load", name: "Bell" });
  }
};
const synthStarted = performance.now();
const synth = await playground.run("/work/examples/fm-synth/browser.cul",
                                   fs.readFileSync(path.join(root, "examples/fm-synth/browser.cul"), "utf8"));
check(!synth.failed, "the FM Synth's engine failed:\n" + synth.out);
check(performance.now() - synthStarted < 10000, "the FM Synth's engine did not end when the page went");
check(seen.join(" | ") === "patch Electric Piano | played 60 | patch Bell", "the FM Synth's page saw " + JSON.stringify(seen));
const synthRun = await playground.run("/work/kept.cul", `println(JSON.parse(FS.read(FS.join(Sys.data_dir("culebra-fm-synth"), "settings.json"))).patch.name)\n`);
check(synthRun.out === "Bell\n", "the FM Synth's engine kept " + JSON.stringify(synthRun.out));

// Without JSPI nothing can wait for the page.
const basic = await loadPlayground("basic");
const plain = await basic.run("/work/page.cul", `try {
  Page.socket()
} catch e {
  println("{e.kind}: {e.message}")
}
`);
check(!plain.failed && plain.out === "IOError: Page.socket: this browser cannot wait for the page (it has no JSPI)\n",
      "the build without JSPI printed " + JSON.stringify(plain.out));

if (bad) process.exit(1);
console.log("page OK (both ways, woken by the page and by MIDI, Closed, one socket, closed at the end, each socket its own, no JSPI, the FM Synth's engine)");
