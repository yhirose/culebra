// Synth Studio: the editor's code runs in the Playground's worker, or in the
// desktop app on its server (studio-program.js says what it runs), and the WAV
// that run leaves is played here, through an AnalyserNode the wave and the
// spectrum read every frame. The sound is the page's, so it outlives the run:
// Stop silences it, and stops a run still rendering by starting what runs it
// afresh, as the Playground's Stop does. The harmonics are a second run's, on
// the WAV the first one left, while the sound plays.
import { createEditor } from "./editor.js";
import { encodeShareParam, decodeShareParam } from "./share-link.js";
import {
  ENTRY, OUT, ANALYSIS_ENTRY, ANALYSIS_OUT, FRAME_SECONDS, HARMONICS, DB_FLOOR,
  buildProgram, buildAnalysis, editorReport,
} from "./studio-program.js";

const $ = (id) => document.getElementById(id);
const playBtn = $("play");
const stopBtn = $("stop");
const lengthSel = $("length");
const loopBox = $("loop");
const statusEl = $("status");
const shareBtn = $("share");
const sampleSel = $("sample");
const consoleEl = $("console");
const clockEl = $("clock");
const pitchEl = $("pitch");
const scope = $("scope");
const lower = $("lower");
const viewTabs = { spectrum: $("view-spectrum"), harmonics: $("view-harmonics") };

function setStatus(text, isErr = false) {
  statusEl.textContent = text;
  statusEl.classList.toggle("err", isErr);
}

// --- the editor, and the draft it keeps ------------------------------------

const DRAFT_KEY = "culebra-studio-draft";
let draftTimer = null;

function loadDraft() {
  try { return localStorage.getItem(DRAFT_KEY); } catch { return null; }
}

function writeDraft() {
  clearTimeout(draftTimer);
  try { localStorage.setItem(DRAFT_KEY, editor.getValue()); } catch { /* not kept */ }
}

const editor = createEditor($("editor"), "", {
  onChange() {
    clearTimeout(draftTimer);
    draftTimer = setTimeout(writeDraft, 400);
  },
  keys: [
    { key: "Mod-Enter", run: () => (play(), true) },
    { key: "Mod-.", run: () => (stop(), true) },
  ],
});
addEventListener("pagehide", writeDraft);

// --- what runs the program -----------------------------------------------------
//
// In the browser, the Playground's worker; in the desktop app
// (examples/synth-studio), the app's own server over a WebSocket, which runs
// the program with Isolate.spawn_source. The app says which in the page's
// <meta name="studio-runner">. Either one runs a program of a kind, "play" or
// "analysis" (which reads the last WAV a play left), one at a time: it tells
// the page `ready`, what the run prints, and `done` with the run's verdict and
// its WAV or its analysis. A run asked for before `ready` waits for it.
// `start` abandons any run in flight (the worker terminated, the connection
// closed, with the WAV it kept) and starts afresh.

// Each kind of run: the path its program goes by, the file it leaves in the
// browser, and how the page reads that file.
const RUNS = {
  play: { path: ENTRY, out: OUT, read: (bytes) => bytes },
  analysis: { path: ANALYSIS_ENTRY, out: ANALYSIS_OUT,
              read: (bytes) => JSON.parse(new TextDecoder().decode(bytes)) },
};

// One run at a time, `kind` the one in flight; a run asked for before what
// runs it has `opened` waits for it. `send(src, kind)` hands one over.
function runQueue(send) {
  let open = false;
  let kind = null;
  let waiting = null;
  const go = (src, k) => {
    kind = k;
    send(src, k);
  };
  return {
    reset() {
      open = false;
      kind = waiting = null;
    },
    opened() {
      open = true;
      const w = waiting;
      waiting = null;
      if (w) go(w.src, w.kind);
    },
    run(src, k) {
      if (open) go(src, k);
      else waiting = { src, kind: k };
    },
    get kind() { return kind; },
    finished() {
      const k = kind;
      kind = null;
      return k;
    },
  };
}

function workerRunner(on) {
  let worker = null;
  const runs = runQueue((src, kind) => {
    const { path, out } = RUNS[kind];
    worker.postMessage({ type: "run", src, path, base: "./", files: [], args: [], collect: [out] });
  });
  return {
    out: "file",
    start() {
      worker?.terminate();
      runs.reset();
      worker = new Worker("./worker.js", { type: "module" });
      worker.onmessage = (e) => {
        const m = e.data;
        if (m.type === "ready") {
          on.ready();
          runs.opened();
        } else if (m.type === "output") {
          on.output(runs.kind, m.text);
        } else if (m.type === "done") {
          const kind = runs.finished();
          const file = m.files?.[RUNS[kind].out];
          on.done(kind, m.rc, file ? RUNS[kind].read(file) : null);
        }
        // The rest (the Sound the run builds its WAV with, a Canvas frame) is
        // the Playground's to show: here the sound is the WAV the run leaves.
      };
      worker.onerror = (e) => on.error("worker error", String(e.message || e));
    },
    run: (src, kind) => runs.run(src, kind),
  };
}

// The bytes of a binary string, as atob answers one.
function bytesOf(s) {
  const bytes = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) bytes[i] = s.charCodeAt(i);
  return bytes;
}

// The server answers a run with `{rc, wav}` (the WAV in base64), `{rc, value}`
// (an analysis) or `{rc, text}` (the failure, as the command line prints it).
function socketRunner(on) {
  let ws = null;
  const runs = runQueue((src, kind) => ws.send(JSON.stringify({ kind, src, path: RUNS[kind].path })));
  return {
    out: "value",
    start() {
      if (ws) {
        ws.onclose = null;  // closed on purpose: the server drops the run
        ws.close();
      }
      runs.reset();
      ws = new WebSocket(new URL("api/run", location.href.replace(/^http/, "ws")));
      ws.onopen = () => {
        on.ready();
        runs.opened();
      };
      ws.onmessage = (e) => {
        const m = JSON.parse(e.data);
        const kind = runs.finished();
        if (m.text) on.output(kind, m.text);
        on.done(kind, m.rc, m.wav ? bytesOf(atob(m.wav)) : (m.value ?? null));
      };
      ws.onclose = () => on.error("the app's server is gone", "", true);
    },
    run: (src, kind) => runs.run(src, kind),
  };
}

let ready = false;
let rendering = false;
let printed = "";  // what the play in flight has printed
let lines = 0;     // the editor's lines it carries (studio-program.js)
// Bumped by each Play and Stop: a run whose sound is still being decoded when
// either comes is not played.
let take = 0;
// Whether what runs the program keeps the WAV of this take, which an analysis
// reads; and the analysis of it: null, or {state: "running" | "done" |
// "stopped" | "failed", frames, hz, harmonics, text}: the frames worked out so
// far of `frames` in all (null before the first part), and what the analysis
// printed. A Stop keeps the frames that have come and asks for no more.
let kept = false;
let analysis = null;

const runnerKind = document.querySelector('meta[name="studio-runner"]')?.content;
const runner = (runnerKind === "socket" ? socketRunner : workerRunner)({
  ready() {
    ready = true;
    // Not over an error the page already has to say (an unreadable link), nor
    // over a Play that restarted the runner and waits for it.
    if (!rendering && !statusEl.classList.contains("err")) setStatus("ready");
    update();
  },
  output(kind, text) {
    if (kind === "play") {
      printed += text;
      showConsole(printed, false);
    } else if (analysis?.state === "running") {
      analysis.text += text;
    }
  },
  done(kind, rc, result) {
    if (kind === "play") finish(rc, result);
    else analysed(rc, result);
  },
  // `gone`: what runs the program cannot take another until it is started
  // again (a closed connection; a worker's error leaves the worker running).
  error(status, detail, gone = false) {
    if (gone) ready = false;
    setStatus(status, true);
    showConsole(detail, true);
    if (rendering || analysis?.state === "running") {
      rendering = false;
      if (analysis?.state === "running") analysis = { state: "failed" };
      startRunner();
    }
    update();
    draw();
  },
});

// A fresh runner keeps no WAV, so an analysis running is dropped with it.
function startRunner() {
  ready = false;
  kept = false;
  if (analysis?.state === "running") analysis = null;
  runner.start();
  update();
}

function showConsole(text, isErr) {
  consoleEl.textContent = text;
  consoleEl.hidden = text === "";
  consoleEl.classList.toggle("err", isErr);
}

function update() {
  playBtn.disabled = !ready || rendering;
  stopBtn.disabled = !rendering && !source && analysis?.state !== "running";
}

// --- Play and Stop -------------------------------------------------------------

function play() {
  if (!ready || rendering) return;
  ensureAudio();  // in the click or the key press, which is what lets it start
  stopSound();
  take++;
  // The runner takes one run at a time: an analysis still running goes.
  if (analysis?.state === "running") startRunner();
  analysis = null;
  kept = false;
  const program = buildProgram(editor.getValue(), editor.getSelection(),
                               Number(lengthSel.value), runner.out);
  lines = program.lines;
  printed = "";
  showConsole("", false);
  editor.clearError();
  rendering = true;
  update();
  setStatus("rendering…");
  draw();
  runner.run(program.src, "play");
}

async function finish(rc, wav) {
  rendering = false;
  update();
  const failed = rc !== 0;
  const { text, line } = editorReport(printed, failed, lines);
  showConsole(text, failed);
  if (line) editor.setError(line);
  if (failed || !wav) {
    setStatus("error", true);
    return;
  }
  kept = true;
  analyse();
  const mine = take;
  let buffer;
  try {
    buffer = await audio.decodeAudioData(wav.buffer);
  } catch (err) {
    setStatus("the sound could not be played: " + err.message, true);
    return;
  }
  if (mine === take) start(buffer);
}

function stop() {
  take++;
  if (rendering) setStatus("stopped — reloading…");
  else if (source) setStatus("stopped");
  const analysing = analysis?.state === "running";
  if (analysing) analysis.state = "stopped";
  if (rendering || analysing) {
    rendering = false;
    startRunner();
  }
  stopSound();
  update();
  draw();
}

playBtn.addEventListener("click", play);
stopBtn.addEventListener("click", stop);
loopBox.addEventListener("change", () => {
  if (source) source.loop = loopBox.checked;
});

// --- the harmonics --------------------------------------------------------------
//
// Worked out when the harmonics are shown and the runner keeps a WAV with none
// worked out yet: after a Play, or on switching to them. It comes a part at a
// time from the start, each part twice the last, so the first frames are
// there soon after the sound starts and the rest stay ahead of it.

const FIRST_PART = 50;  // frames

function analyse() {
  if (view !== "harmonics" || !kept || analysis) return;
  analysis = { state: "running", frames: null, hz: [], harmonics: [], text: "" };
  analysePart(0, FIRST_PART);
  update();
  draw();
}

function analysePart(from, to) {
  runner.run(buildAnalysis(runner.out, from, to), "analysis");
}

function analysed(rc, result) {
  if (analysis?.state !== "running") return;  // dropped by a Play or a Stop
  if (rc !== 0 || !result) {
    // Its error under the code, as a Play's is: the graph has no room for it.
    showConsole(analysis.text, true);
    analysis = { state: "failed" };
  } else {
    analysis.frames = result.frames;
    analysis.hz.push(...result.hz);
    analysis.harmonics.push(...result.harmonics);
    const next = analysis.hz.length;
    if (next < result.frames) analysePart(next, next + 2 * result.hz.length);
    else analysis.state = "done";
  }
  update();
  draw();
}

// --- the lower graph's view ---------------------------------------------------------

const VIEW_KEY = "culebra-studio-view";
let view = "spectrum";
try { if (localStorage.getItem(VIEW_KEY) === "harmonics") view = "harmonics"; } catch { /* not kept */ }

function showView(v) {
  view = v;
  try { localStorage.setItem(VIEW_KEY, v); } catch { /* not kept */ }
  for (const [name, tab] of Object.entries(viewTabs)) tab.setAttribute("aria-selected", String(name === v));
  lower.setAttribute("aria-label", v === "spectrum" ? "The spectrum playing now"
                                                    : "The levels of the harmonics playing now");
  analyse();
  draw();
}
for (const [name, tab] of Object.entries(viewTabs)) tab.addEventListener("click", () => showView(name));

// --- the sound -----------------------------------------------------------------

let audio = null;     // AudioContext, made at the first Play
let analyser = null;  // between every sound and the speakers
let source = null;    // the sound playing, or null
let startedAt = 0;    // when it started, on the context's clock
let duration = 0;

function ensureAudio() {
  if (!audio) {
    audio = new AudioContext();
    analyser = audio.createAnalyser();
    analyser.fftSize = 8192;
    analyser.smoothingTimeConstant = 0.6;
    analyser.connect(audio.destination);
  }
  if (audio.state === "suspended") audio.resume();
}

function start(buffer) {
  stopSound();
  const s = audio.createBufferSource();
  s.buffer = buffer;
  s.loop = loopBox.checked;
  s.connect(analyser);
  s.onended = () => {
    if (source !== s) return;
    source = null;
    setStatus("ready");
    update();
  };
  source = s;
  duration = buffer.duration;
  startedAt = audio.currentTime;
  stoppedAt = null;
  s.start();
  setStatus("playing");
  update();
  animate();
}

function stopSound() {
  if (!source) return;
  stoppedAt = playhead();
  const s = source;
  source = null;
  s.onended = null;
  s.stop();
  s.disconnect();
}

// Where the sound is, in seconds from its start, or null before the first: one
// stopped stays where it stopped, one that ended at its end.
let stoppedAt = null;
function playhead() {
  if (!audio || !duration) return null;
  const t = audio.currentTime - startedAt;
  if (!source) return stoppedAt ?? Math.min(t, duration);
  return source.loop ? t % duration : Math.min(t, duration);
}

// --- the graphs ------------------------------------------------------------------
//
// Drawn every frame while a sound plays, from the analyser's latest samples
// and the analysis; when it stops, the last frame stays. A resize redraws what
// was last read.

let timeData = null;
let freqData = null;
let frame = 0;

function animate() {
  if (!frame) frame = requestAnimationFrame(tick);
}

function tick() {
  frame = 0;
  if (!timeData) {
    timeData = new Float32Array(analyser.fftSize);
    freqData = new Float32Array(analyser.frequencyBinCount);
  }
  analyser.getFloatTimeDomainData(timeData);
  analyser.getFloatFrequencyData(freqData);
  draw();
  if (source) animate();
}

let colors = null;
function palette() {
  const css = getComputedStyle(document.documentElement);
  const v = (name) => css.getPropertyValue(name).trim();
  colors = { line: v("--brand"), grid: v("--border"), label: v("--muted"),
             font: getComputedStyle(document.body).fontFamily };
}
palette();
matchMedia("(prefers-color-scheme: dark)").addEventListener("change", () => {
  palette();
  draw();
});

// The canvas at its displayed size in device pixels.
function fit(canvas) {
  const r = canvas.getBoundingClientRect();
  const dpr = devicePixelRatio || 1;
  const w = Math.max(1, Math.round(r.width * dpr));
  const h = Math.max(1, Math.round(r.height * dpr));
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w;
    canvas.height = h;
  }
  const g = canvas.getContext("2d");
  g.clearRect(0, 0, w, h);
  g.font = `${10 * dpr}px ${colors.font}`;
  return { g, w, h, dpr };
}

function draw() {
  const t = playhead();
  pitchEl.textContent = "";  // a pitched frame of the harmonics says its pitch
  drawScope();
  if (view === "spectrum") drawSpectrum();
  else drawHarmonics(t);
  if (t !== null) clockEl.textContent = `${t.toFixed(2)} / ${duration.toFixed(2)} s`;
}

const SCOPE_SECONDS = 0.03;

function drawScope() {
  const { g, w, h, dpr } = fit(scope);
  const mid = h / 2;
  g.strokeStyle = colors.grid;
  g.lineWidth = dpr;
  g.beginPath();
  for (const y of [h / 4, mid, (3 * h) / 4]) {
    g.moveTo(0, y);
    g.lineTo(w, y);
  }
  g.stroke();
  if (!timeData) return;

  // 30 ms from where the wave last rose through 0 before the latest 30 ms
  // (an oscilloscope's trigger), so a steady tone stands still.
  const n = timeData.length;
  const span = Math.min(Math.round(audio.sampleRate * SCOPE_SECONDS), n >> 1);
  let from = n - span;
  for (let i = n - span; i > 0; i--) {
    if (timeData[i - 1] < 0 && timeData[i] >= 0) {
      from = i;
      break;
    }
  }
  g.strokeStyle = colors.line;
  g.lineWidth = 1.5 * dpr;
  g.beginPath();
  for (let i = 0; i < span; i++) {
    const x = (i / (span - 1)) * w;
    const y = mid - timeData[from + i] * mid * 0.92;
    if (i === 0) g.moveTo(x, y);
    else g.lineTo(x, y);
  }
  g.stroke();
}

const F_LOW = 30;
const DB_LOW = -110;
const DB_HIGH = -20;
const F_MARKS = [50, 100, 200, 500, 1000, 2000, 5000, 10000];

function drawSpectrum() {
  const { g, w, h, dpr } = fit(lower);
  const fHigh = Math.min(16000, (audio ? audio.sampleRate : 44100) / 2);
  const xOf = (f) => (Math.log(f / F_LOW) / Math.log(fHigh / F_LOW)) * w;
  const fOf = (x) => F_LOW * (fHigh / F_LOW) ** (x / w);
  const yOf = (db) => h * (1 - (db - DB_LOW) / (DB_HIGH - DB_LOW));

  const dbMarks = [-100, -80, -60, -40];
  g.strokeStyle = colors.grid;
  g.lineWidth = dpr;
  g.beginPath();
  for (const f of F_MARKS) {
    const x = Math.round(xOf(f));
    g.moveTo(x, 0);
    g.lineTo(x, h);
  }
  for (const db of dbMarks) {
    const y = Math.round(yOf(db));
    g.moveTo(0, y);
    g.lineTo(w, y);
  }
  g.stroke();
  if (freqData) drawLevels(g, w, h, dpr, fOf, yOf);
  // The labels last, over the levels.
  g.fillStyle = colors.label;
  for (const f of F_MARKS) {
    g.fillText(f >= 1000 ? `${f / 1000}k` : String(f), Math.round(xOf(f)) + 3 * dpr, h - 4 * dpr);
  }
  for (const db of dbMarks) g.fillText(`${db} dB`, 3 * dpr, Math.round(yOf(db)) - 3 * dpr);
}

function drawLevels(g, w, h, dpr, fOf, yOf) {
  // A column covers a band that holds many bins at the top and a fraction of
  // one at the bottom: the loudest bin in it, or the level between the two
  // bins it falls between.
  const binHz = audio.sampleRate / analyser.fftSize;
  const level = (k0, k1) => {
    if (Math.floor(k1) > Math.floor(k0) + 1) {
      let m = -Infinity;
      for (let k = Math.ceil(k0); k <= Math.floor(k1); k++) m = Math.max(m, freqData[k]);
      return m;
    }
    const k = Math.floor(k0);
    const t = k0 - k;
    return freqData[k] * (1 - t) + freqData[Math.min(k + 1, freqData.length - 1)] * t;
  };
  g.beginPath();
  g.moveTo(0, h);
  // A column's band ends where the next one's starts.
  let k1 = fOf(0) / binHz;
  for (let x = 0; x < w; x++) {
    const k0 = k1;
    k1 = fOf(x + 1) / binHz;
    g.lineTo(x, Math.min(h, Math.max(0, yOf(level(k0, k1)))));
  }
  g.lineTo(w, h);
  g.closePath();
  g.fillStyle = colors.line;
  g.globalAlpha = 0.25;
  g.fill();
  g.globalAlpha = 1;
  g.strokeStyle = colors.line;
  g.lineWidth = 1.5 * dpr;
  g.stroke();
}

const NOTE_NAMES = ["C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"];

// The note nearest `hz`, as A4 is 440 Hz.
function noteOf(hz) {
  const n = Math.round(69 + 12 * Math.log2(hz / 440));
  return `${NOTE_NAMES[((n % 12) + 12) % 12]}${Math.floor(n / 12) - 1}`;
}

const HARMONIC_MARKS = [1, 5, 10, 15, 20, 25, 30, 35, 40];

// A bar for each harmonic of the analysis's frame at `t`, its height the
// level from DB_FLOOR up to 0 dBFS; where there is no frame to draw, the
// reason, in the middle.
function drawHarmonics(t) {
  const { g, w, h, dpr } = fit(lower);
  // The bars stand right of the levels' labels and above the harmonics'.
  const left = 44 * dpr;
  const base = h - 16 * dpr;
  const yOf = (db) => base * (db / DB_FLOOR);
  const slot = (w - left) / HARMONICS;
  const dbMarks = [0, -20, -40, -60];
  g.strokeStyle = colors.grid;
  g.lineWidth = dpr;
  g.beginPath();
  for (const db of [...dbMarks, DB_FLOOR]) {
    const y = Math.round(yOf(db)) + (db === 0 ? dpr : 0);
    g.moveTo(left, y);
    g.lineTo(w, y);
  }
  g.stroke();
  g.fillStyle = colors.label;
  g.textAlign = "right";
  g.textBaseline = "middle";
  for (const db of dbMarks) g.fillText(`${db} dB`, left - 6 * dpr, Math.max(6 * dpr, yOf(db)));
  g.textAlign = "center";
  g.textBaseline = "alphabetic";
  for (const k of HARMONIC_MARKS) g.fillText(String(k), left + (k - 0.5) * slot, h - 4 * dpr);

  const say = (text) => {
    g.fillStyle = colors.label;
    g.fillText(text, left + (w - left) / 2, base / 2);
  };
  // The frame at the playhead, when its part has come.
  const at = analysis?.frames && t !== null ? Math.min(analysis.frames - 1, Math.round(t / FRAME_SECONDS)) : -1;
  const come = at >= 0 && at < analysis.hz.length;
  if (rendering) say("rendering…");
  else if (analysis?.state === "failed") say("the analysis failed");
  else if (!come) {
    say({ running: "analysing…", stopped: "stopped" }[analysis?.state] ?? "Play a sound to see its harmonics");
  }
  else if (analysis.hz[at] === null) say("no pitch");
  else {
    const hz = analysis.hz[at];
    pitchEl.textContent = `${hz.toFixed(1)} Hz · ${noteOf(hz)}`;
    g.fillStyle = colors.line;
    analysis.harmonics[at].forEach((db, k) => {
      const y = yOf(db);
      g.fillRect(left + k * slot + slot * 0.15, y, slot * 0.7, base - y);
    });
  }
  g.textAlign = "start";
}

new ResizeObserver(() => draw()).observe(scope.closest(".st-right"));

// --- Share ------------------------------------------------------------------------

shareBtn.addEventListener("click", async () => {
  const url = new URL(location.href);
  url.hash = "code=" + (await encodeShareParam(editor.getValue()));
  history.replaceState(null, "", url);
  try {
    await navigator.clipboard.writeText(url.href);
    setStatus("link copied");
  } catch {
    setStatus("the link is in the address bar");
  }
});

// --- start ------------------------------------------------------------------------
//
// A shared link's code, else the draft, else (null) the opening sample.

async function linkOrDraft() {
  const param = new URLSearchParams(location.hash.slice(1)).get("code");
  if (param) {
    try {
      return await decodeShareParam(param);
    } catch {
      setStatus("the link's code could not be read", true);
    }
  }
  return loadDraft();
}

// --- the samples ---------------------------------------------------------------
//
// studio-samples/index.json lists them from the simplest up. Choosing one puts
// it in the editor, as the Playground's Examples do.

const SAMPLES = "./studio-samples/";
let samples = [];

// Into the editor, unless another has been chosen while it was on its way.
async function loadSample(file) {
  sampleSel.value = file;
  try {
    const res = await fetch(SAMPLES + file);
    if (!res.ok) throw new Error(`${file}: HTTP ${res.status}`);
    const text = await res.text();
    if (sampleSel.value === file) editor.setValue(text);
  } catch (err) {
    setStatus("the sample could not be loaded: " + err.message, true);
  }
}

async function loadSamples() {
  try {
    const res = await fetch(SAMPLES + "index.json");
    samples = res.ok ? await res.json() : [];
  } catch {
    samples = [];
  }
  for (const { title, file } of samples) sampleSel.add(new Option(title, file));
}

sampleSel.addEventListener("change", async () => {
  if (!sampleSel.value) return;
  await loadSample(sampleSel.value);
  editor.focus();
});

addEventListener("hashchange", () => location.reload());
startRunner();
showView(view);
await loadSamples();
const code = await linkOrDraft();
const opening = samples.find((s) => s.opening);
if (code !== null) editor.setValue(code);
else if (opening) await loadSample(opening.file);
draw();
