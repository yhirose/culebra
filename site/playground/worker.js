// Playground worker: owns the WASM instance so the page never blocks.
// Stop = the main thread terminates this worker and spawns a fresh one.
//
// Two builds ship (see build.sh): culebra-full adds JSPI, needed for both
// the WebGPU Tensor backend and the TUI tab's Term.read_key. It loads only
// where a WebGPU device can actually be acquired — tensorlib's WebGPU init
// crashes at module startup with no device handed in (a known gap, not
// handled gracefully yet), so a browser with JSPI but no usable WebGPU
// device falls back to the basic build and loses TUI too. That coupling is
// a narrow edge case (WebGPU disabled/unavailable on an otherwise-current
// browser), not the common one — pick by capability, never by browser
// version (Chrome 137 shipping JSPI is exactly what broke version-sniffing
// detectors elsewhere).
const hasJSPI = typeof WebAssembly.Suspending === "function";

// The device is acquired here, in JS, and handed to wasm fully formed, which
// is what lets every C++ entry point stay synchronous — the async surface is
// this file, not the library. Without it tensorlib's WebGPU init dereferences
// an undefined device ("cannot read properties of undefined (reading
// 'queue')").
async function acquireDevice() {
  if (!hasJSPI || !navigator.gpu) return null;
  try {
    const adapter = await navigator.gpu.requestAdapter();
    return adapter ? await adapter.requestDevice() : null;
  } catch {
    return null;
  }
}

const device = await acquireDevice();
const useFullBuild = device !== null;

const { default: createCulebra } = await import(
  useFullBuild ? "./culebra-full.js" : "./culebra-basic.js"
);
const mod = await createCulebra(
  useFullBuild ? { preinitializedWebGPUDevice: device } : {}
);

// --- TUI key/mouse input --------------------------------------------------
//
// Term.read_key (full build only; see term.h) suspends the wasm call via
// JSPI until self.__waitForKey resolves. The main thread forwards xterm.js's
// onData bytes here as "key" messages (app.js) — a keystroke or an SGR mouse
// report arrives as one onData call and is passed through verbatim;
// culebra's _parse_event already tells them apart. A key arriving with
// nobody waiting (the interpreter is busy between polls) is queued rather
// than dropped, so fast typing/clicking isn't lost.
const keyQueue = [];
let pendingKeyResolve = null;

function deliverKey(key) {
  if (pendingKeyResolve) {
    const resolve = pendingKeyResolve;
    pendingKeyResolve = null;
    resolve(key);
  } else {
    keyQueue.push(key);
  }
}

self.__waitForKey = (timeoutMs) => {
  if (keyQueue.length > 0) return Promise.resolve(keyQueue.shift());
  return new Promise((resolve) => {
    let done = false;
    const timer = setTimeout(() => {
      if (done) return;
      done = true;
      pendingKeyResolve = null;
      resolve("");
    }, timeoutMs);
    pendingKeyResolve = (key) => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      resolve(key);
    };
  });
};

// --- Canvas frame pacing / input ------------------------------------------
//
// Canvas.present() (canvas.h, full build) suspends via JSPI on self.__nextFrame
// until the main thread's requestAnimationFrame loop forwards a "tick", pacing
// the game to the display. Unlike keys these are NOT queued: present() waits for
// the *next* tick after it's called, so an unheeded tick between frames is just
// dropped. A setTimeout fallback keeps a backgrounded tab (where rAF stalls)
// from wedging the run.
let pendingFrameResolve = null;
self.__nextFrame = () => {
  return new Promise((resolve) => {
    let done = false;
    const timer = setTimeout(() => {
      if (done) return;
      done = true;
      pendingFrameResolve = null;
      resolve();
    }, 100);
    pendingFrameResolve = () => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      resolve();
    };
  });
};

// Input state the wasm side polls synchronously (canvas.h's _wasm_canvas_*).
self.__canvasButtons = 0;
self.__canvasMouseX = 0;
self.__canvasMouseY = 0;
self.__canvasMouseButtons = 0;
// Music playback state: set optimistically by canvas.h's music EM_JS calls,
// corrected by the main thread's "musicState" messages (failed decode, a
// non-looping file ending).
self.__musicLoaded = false;
self.__musicPlaying = false;

postMessage({ type: "ready", backend: useFullBuild ? "full" : "basic" });

// Output streams live: wasm_main.cc's StreamingBuf posts "output" messages
// directly (via EM_ASM's plain `postMessage`, i.e. this same worker's global
// scope) the moment std::cout/cerr flushes — those never pass through this
// onmessage handler at all. This handler only owns the run/key protocol.
let running = false;

// --- in-memory filesystem ----------------------------------------------------
// MEMFS mirrors the repository layout under /work, so a program's own path and
// the paths it reads are the same strings they would be in a checkout. That is
// what lets an example's source be byte-identical here and natively:
// `FS.dirname(Sys.script) + "/assets/x.png"` resolves either way.
const MEMFS_ROOT = "/work";

function mkdirp(dir) {
  let cur = "";
  for (const part of dir.split("/")) {
    if (!part) continue;
    cur += "/" + part;
    try { mod.FS.mkdir(cur); } catch (err) { /* already there */ }
  }
}

function writeFile(path, data) {
  mkdirp(path.slice(0, path.lastIndexOf("/")));
  mod.FS.writeFile(path, data);
}

// Fetch every extra file the catalog lists for this example and drop it where
// the program expects it — imported modules as much as data, since the loader
// opens both the same way. They are named repo-relative in examples.json, which
// is also how build.sh mirrors them next to the page, so one list serves both
// the copy at build time and the fetch here and neither can drift.
const staged = new Set();

async function stageProgram(msg) {
  const rel = msg.path || "main.cul";
  const path = MEMFS_ROOT + "/" + rel;
  writeFile(path, msg.src);          // so FS.read(Sys.script) works too
  for (const asset of msg.assets || []) {
    const dst = MEMFS_ROOT + "/" + asset;
    if (staged.has(dst)) continue;   // assets are immutable; fetch each once
    const res = await fetch("./" + asset);
    if (!res.ok) throw new Error("asset " + asset + ": HTTP " + res.status);
    writeFile(dst, new Uint8Array(await res.arrayBuffer()));
    staged.add(dst);
  }
  return path;
}

onmessage = async (e) => {
  const { type } = e.data;
  if (type === "key") {
    deliverKey(e.data.key);
    return;
  }
  if (type === "tick") {
    if (pendingFrameResolve) pendingFrameResolve();
    return;
  }
  if (type === "input") {
    self.__canvasButtons = e.data.buttons;
    // Arbitrary-key state (Canvas.key / key_queue / typed): the held names
    // replace wholesale; presses and typed characters append to capped queues
    // (oldest first out), matching the native backend's 256-entry cap.
    if (e.data.keys) self.__canvasKeysHeld = new Set(e.data.keys);
    const KEY_QUEUE_CAP = 256;
    if (e.data.keyEvents) {
      const q = self.__canvasKeyQueue || (self.__canvasKeyQueue = []);
      q.push(...e.data.keyEvents);
      if (q.length > KEY_QUEUE_CAP) q.splice(0, q.length - KEY_QUEUE_CAP);
    }
    if (e.data.chars) {
      const q = self.__canvasCharQueue || (self.__canvasCharQueue = []);
      q.push(...e.data.chars);
      if (q.length > KEY_QUEUE_CAP) q.splice(0, q.length - KEY_QUEUE_CAP);
    }
    return;
  }
  if (type === "canvasMouse") {
    self.__canvasMouseX = e.data.x;
    self.__canvasMouseY = e.data.y;
    self.__canvasMouseButtons = e.data.buttons;
    return;
  }
  if (type === "musicState") {
    self.__musicPlaying = e.data.playing;
    self.__musicLoaded = e.data.loaded;
    return;
  }
  if (type === "soundState") {
    // A one-shot ended (or failed to decode) on the main thread; correct the
    // optimistic play-state the wasm side wrote.
    if (self.__soundsPlaying) self.__soundsPlaying[e.data.id] = e.data.playing;
    return;
  }
  if (type === "termSize") {
    // Read synchronously by _wasm_term_cols/rows (term.h) — a Worker has its
    // own global scope, so app.js can't set these directly and sends them
    // as a message instead. Must arrive before the program runs.
    self.__termCols = e.data.cols;
    self.__termRows = e.data.rows;
    return;
  }
  if (type !== "run" || running) return;  // ignore a Run while one is in flight
  running = true;
  keyQueue.length = 0;
  pendingKeyResolve = null;
  pendingFrameResolve = null;
  self.__canvasButtons = 0;
  self.__canvasMouseButtons = 0;
  self.__canvasKeysHeld = new Set();
  self.__canvasKeyQueue = [];
  self.__canvasCharQueue = [];
  self.__musicLoaded = false;
  self.__musicPlaying = false;
  self.__soundsPlaying = {};

  const t0 = performance.now();
  let rc = 1;
  try {
    // JSPI makes run_culebra return a promise in the full build; TUI/GPU
    // waits suspend beneath it. Nothing in the C++ call chain is async.
    const path = await stageProgram(e.data);
    // Newline-separated rather than an array: ccall marshals one string, and an
    // argument that contains a newline is not a thing examples.json can spell.
    const args = (e.data.args || []).join("\n");
    rc = await mod.ccall("run_culebra", "number", ["string", "string", "string"],
                         [e.data.src, path, args], { async: useFullBuild });
  } catch (err) {
    postMessage({ type: "output", text: "internal error: " + err });
  }
  running = false;
  postMessage({ type: "done", rc, ms: performance.now() - t0 });
};
