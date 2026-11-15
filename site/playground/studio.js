// Synth Studio: the editor's code runs in the Playground's worker
// (studio-program.js says what it runs), and the WAV that run leaves is
// played here, through an AnalyserNode the two graphs read every frame. The
// sound is the page's, so it outlives the run: Stop silences it, and stops a
// run still rendering by terminating the worker, as the Playground's Stop does.
import { createEditor } from "./editor.js";
import { encodeShareParam, decodeShareParam } from "./share-link.js";
import { ENTRY, OUT, buildProgram, editorReport } from "./studio-program.js";

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
const scope = $("scope");
const spectrum = $("spectrum");

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

// --- the worker --------------------------------------------------------------

let worker = null;
let ready = false;
let rendering = false;
let printed = "";  // what the run in flight has printed
let lines = 0;     // the editor's lines it carries (studio-program.js)
// Bumped by each Play and Stop: a run whose sound is still being decoded when
// either comes is not played.
let take = 0;

function spawnWorker() {
  ready = false;
  worker = new Worker("./worker.js", { type: "module" });
  worker.onmessage = (e) => {
    const m = e.data;
    if (m.type === "ready") {
      ready = true;
      // Not over an error the page already has to say (an unreadable link).
      if (!statusEl.classList.contains("err")) setStatus("ready");
      update();
    } else if (m.type === "output") {
      printed += m.text;
      showConsole(printed, false);
    } else if (m.type === "done") {
      finish(m);
    }
    // The rest (the Sound the run builds its WAV with, a Canvas frame) is the
    // Playground's to show: here the sound is the WAV the run leaves.
  };
  worker.onerror = (e) => {
    setStatus("worker error", true);
    showConsole(String(e.message || e), true);
    if (rendering) {
      rendering = false;
      worker.terminate();
      spawnWorker();
    }
    update();
  };
  update();
}

function showConsole(text, isErr) {
  consoleEl.textContent = text;
  consoleEl.hidden = text === "";
  consoleEl.classList.toggle("err", isErr);
}

function update() {
  playBtn.disabled = !ready || rendering;
  stopBtn.disabled = !rendering && !source;
}

// --- Play and Stop -------------------------------------------------------------

function play() {
  if (!ready || rendering) return;
  ensureAudio();  // in the click or the key press, which is what lets it start
  stopSound();
  take++;
  const program = buildProgram(editor.getValue(), editor.getSelection(),
                               Number(lengthSel.value));
  lines = program.lines;
  printed = "";
  showConsole("", false);
  editor.clearError();
  rendering = true;
  update();
  setStatus("rendering…");
  worker.postMessage({
    type: "run", src: program.src, path: ENTRY, base: "./", files: [], args: [],
    collect: [OUT],
  });
}

async function finish(m) {
  rendering = false;
  update();
  const failed = m.rc !== 0;
  const { text, line } = editorReport(printed, failed, lines);
  showConsole(text, failed);
  if (line) editor.setError(line);
  const wav = m.files?.[OUT];
  if (failed || !wav) {
    setStatus("error", true);
    return;
  }
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
  if (rendering) {
    worker.terminate();
    rendering = false;
    setStatus("stopped — reloading…");
    spawnWorker();
  } else if (source) {
    setStatus("stopped");
  }
  stopSound();
  update();
}

playBtn.addEventListener("click", play);
stopBtn.addEventListener("click", stop);
loopBox.addEventListener("change", () => {
  if (source) source.loop = loopBox.checked;
});

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
  s.start();
  setStatus("playing");
  update();
  animate();
}

function stopSound() {
  if (!source) return;
  const s = source;
  source = null;
  s.onended = null;
  s.stop();
  s.disconnect();
}

// --- the two graphs ------------------------------------------------------------
//
// Drawn every frame while a sound plays, from the analyser's latest samples;
// when it stops, the last frame stays. A resize redraws what was last read.

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
  drawScope();
  drawSpectrum();
  if (audio && duration) {
    let t = audio.currentTime - startedAt;
    t = source ? (source.loop ? t % duration : Math.min(t, duration)) : Math.min(t, duration);
    clockEl.textContent = `${t.toFixed(2)} / ${duration.toFixed(2)} s`;
  }
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
  const { g, w, h, dpr } = fit(spectrum);
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
spawnWorker();
await loadSamples();
const code = await linkOrDraft();
const opening = samples.find((s) => s.opening);
if (code !== null) editor.setValue(code);
else if (opening) await loadSample(opening.file);
draw();
