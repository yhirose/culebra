// The FM Synth in the Playground: examples/fm-synth/ui as a page of its own
// (fm-synth.html, made from ui/index.html by copy-frontend.sh) with its engine
// (the catalog's fm-synth.html entry, examples/fm-synth/browser.cul) run in
// the Playground's worker. The socket ui/app.js asks for (openSynthSocket) is
// the program's Page.socket(); the sound is the worker's mixer and MIDI the
// page's Web MIDI, as in the Playground (host-audio.js, host-midi.js). When
// the engine stops, the socket closes, and ui/app.js asking again runs it
// again.
import { hostAudio } from "./host-audio.js";
import { hostMidi } from "./host-midi.js";

const PAGE = "fm-synth.html";
const audio = hostAudio();
let worker = null;
let socket = null;  // the one ui/app.js holds, while the engine runs
const onMidi = hostMidi((msg) => {
  if (worker) worker.postMessage(msg);
});

// The entry and the files beside it, from the catalog the Playground serves.
const engine = (async () => {
  const catalog = await (await fetch("./examples.json")).json();
  const entry = catalog.categories.flatMap((c) => c.examples).find((e) => e.page === PAGE);
  if (!entry) throw new Error("examples.json has no entry for " + PAGE);
  const src = await (await fetch("./" + entry.path)).text();
  return { src, path: entry.path, files: entry.assets || [] };
})();

// What ui/app.js reads and sends, in a WebSocket's shape: a socket of the
// page, which the program opens (Page.socket) and either side closes.
class PageSocket {
  constructor() {
    this.readyState = 0;  // CONNECTING, until the program opens its side
    this.onopen = this.onmessage = this.onclose = null;
  }
  send(text) {
    if (this.readyState === 1 && worker) worker.postMessage({ type: "pageMessage", text });
  }
  close() {
    if (worker) worker.postMessage({ type: "pageGone" });
    this.closed();
  }
  opened() {
    this.readyState = 1;
    if (this.onopen) this.onopen();
  }
  message(text) {
    if (this.onmessage) this.onmessage({ data: text });
  }
  closed() {
    if (this.readyState === 3) return;
    this.readyState = 3;
    if (socket === this) socket = null;
    if (this.onclose) this.onclose();
  }
}

function run() {
  worker = new Worker("./worker.js", { type: "module" });
  worker.onmessage = async (e) => {
    const msg = e.data;
    if (msg.type === "ready") {
      audio.channel(worker);
      const { src, path, files } = await engine;
      worker.postMessage({ type: "run", src, path, base: "./", files, args: [] });
    } else if (onMidi(msg)) {
      // MIDI asked for or sent: the page's Web MIDI answered.
    } else if (msg.type === "pageOpen") {
      if (socket) socket.opened();
    } else if (msg.type === "pageSend") {
      if (socket) socket.message(msg.text);
    } else if (msg.type === "pageClose") {
      if (socket) socket.closed();
    } else if (msg.type === "output") {
      console.log(msg.text);  // what the engine printed: an error, if it stopped
    } else if (msg.type === "done") {
      worker.terminate();
      worker = null;
      if (socket) socket.closed();
    }
  };
}

window.openSynthSocket = () => {
  socket = new PageSocket();
  if (!worker) run();
  return socket;
};

// The page going away: the engine keeps its settings as it does when the
// desktop window closes, and what it saved is written (worker.js's flush).
window.addEventListener("pagehide", () => {
  if (!worker) return;
  worker.postMessage({ type: "pageGone" });
  worker.postMessage({ type: "flush" });
});
