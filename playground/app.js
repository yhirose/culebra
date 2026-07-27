// Playground UI. The WASM interpreter runs inside worker.js; this file owns
// the editor, toolbar, worker lifecycle (Stop = terminate + respawn), and
// the Output/TUI tabs.
import { createEditor } from "./editor.js";
import { Terminal } from "https://esm.sh/@xterm/xterm@5.5.0";

const $ = (id) => document.getElementById(id);
const editor = createEditor($("editor"), "");
const output = $("output");
const runBtn = $("run");
const stopBtn = $("stop");
const clearBtn = $("clear");
const examplesSel = $("examples");
const status = $("status");
const backendEl = $("backend");

// --- TUI pane --------------------------------------------------------------
//
// Term.app's fixed layout math (see e.g. samegame.cul's offsets()) reads
// _Term.cols()/rows() once per frame, so both sides need to agree on a
// single size — no dynamic resize in this first pass. worker.js can't reach
// this file's `self` (a Worker has its own global scope), so the size is
// sent as a message once the worker is ready, for _wasm_term_cols/rows
// (term.h) to read.
const TERM_COLS = 80;
const TERM_ROWS = 24;
const term = new Terminal({ cols: TERM_COLS, rows: TERM_ROWS, cursorBlink: true });
term.open($("tui"));

// --- Canvas pane -----------------------------------------------------------
//
// A Canvas program (canvas.h / src/preambles/canvas.cul) posts RGBA frames the
// worker forwards as "frame" messages; here they become putImageData on a
// <canvas>. The framebuffer's own w/h drive the backing store, and CSS scales
// it up crisply. Input (keys/pointer) is captured on the pane and forwarded to
// the worker, which the wasm side polls — see the input block near onData.
const gameCanvas = $("game");
const gctx = gameCanvas.getContext("2d");
const canvasPane = $("canvas-pane");

// --- tabs --------------------------------------------------------------

const TABS = ["output", "tui", "canvas"];
const tabButtons = { output: $("tab-output"), tui: $("tab-tui"), canvas: $("tab-canvas") };
const panes = { output, tui: $("tui"), canvas: canvasPane };
let activeTab = "output";

function switchTab(name) {
  if (name === activeTab) return;
  activeTab = name;
  for (const k of TABS) {
    tabButtons[k].classList.toggle("active", k === name);
    tabButtons[k].setAttribute("aria-selected", k === name ? "true" : "false");
    panes[k].classList.toggle("active", k === name);
  }
  if (name === "tui") term.focus();
  if (name === "canvas") canvasPane.focus();
}
tabButtons.output.addEventListener("click", () => switchTab("output"));
tabButtons.tui.addEventListener("click", () => switchTab("tui"));
tabButtons.canvas.addEventListener("click", () => switchTab("canvas"));

// A TUI program's Term.app enters/exits the alternate screen with these two
// escapes (src/preambles/term.cul) — reliable markers for "now drawing a
// game" vs "now printing lines", so tabs switch automatically instead of the
// user having to guess which pane a script will use.
const ALT_SCREEN_ENTER = "\x1b[?1049h";
const ALT_SCREEN_EXIT = "\x1b[?1049l";
let inTui = false;
let inCanvas = false;   // a "frame" message has switched to the Canvas pane

// Leave TUI/Canvas mode and show the Output pane. The routing state (inTui /
// inCanvas) and the visible tab must move together, so run/stop/onerror share
// this.
function resetToOutput() {
  inTui = false;
  inCanvas = false;
  heldButtons = 0;
  switchTab("output");
}

function appendOutput(text) {
  if (text) output.textContent += text;
}

// Output streams in arbitrary-sized chunks (see wasm_main.cc's StreamingBuf,
// one postMessage per std::cout/cerr flush) — a chunk can contain a whole
// alt-screen marker, straddle one, or contain neither, so route byte-range
// by byte-range rather than assuming one marker per message.
function routeOutput(text) {
  let rest = text;
  while (rest.length > 0) {
    if (!inTui) {
      const i = rest.indexOf(ALT_SCREEN_ENTER);
      if (i === -1) { appendOutput(rest); return; }
      appendOutput(rest.slice(0, i));
      inTui = true;
      switchTab("tui");
      rest = rest.slice(i);
    } else {
      const i = rest.indexOf(ALT_SCREEN_EXIT);
      if (i === -1) { term.write(rest); return; }
      let end = i + ALT_SCREEN_EXIT.length;
      // term.cul's Term.app always follows the exit marker with an SGR
      // reset in the same IO.print call; consume it too so it doesn't leak
      // into the Output pane as literal "[0m" once routing switches back.
      if (rest.startsWith("\x1b[0m", end)) end += 4;
      term.write(rest.slice(0, end));
      inTui = false;
      switchTab("output");
      rest = rest.slice(end);
    }
  }
}

// --- examples ---------------------------------------------------------------
//
// examples.json lists {title, path, assets?} per category. `assets` is every
// extra file the program needs beside its entry source — imported modules as
// well as data — since both are just files it opens at run time. `path` and
// each entry of `assets` double as the fetch URL (relative to this file) and,
// at build time, the repo-root source path build.sh copies from — one list, so
// the copy and the fetch cannot drift. The worker mirrors them into its
// in-memory filesystem under the same relative paths before the program runs.

let EXAMPLE_PATHS = {};  // title -> path
let EXAMPLE_ASSETS = {}; // title -> [path]
let EXAMPLE_ARGS = {};   // title -> [arg], the program's Sys.argv here
let currentExample = null;

async function loadExampleCatalog() {
  const res = await fetch("./examples.json");
  const { categories } = await res.json();
  for (const category of categories) {
    const group = document.createElement("optgroup");
    group.label = category.name;
    for (const example of category.examples) {
      EXAMPLE_PATHS[example.title] = example.path;
      EXAMPLE_ASSETS[example.title] = example.assets || [];
      EXAMPLE_ARGS[example.title] = (example.args || []).map(String);
      const opt = document.createElement("option");
      opt.value = example.title;
      opt.textContent = example.title;
      group.appendChild(opt);
    }
    examplesSel.appendChild(group);
  }
}

async function loadExample(title) {
  currentExample = title;
  const res = await fetch(EXAMPLE_PATHS[title]);
  return res.text();
}

// --- worker lifecycle -----------------------------------------------------

let worker = null;
let running = false;

function setStatus(text, isErr = false) {
  status.textContent = text;
  status.classList.toggle("err", isErr);
}

function spawnWorker() {
  worker = new Worker("./worker.js", { type: "module" });
  worker.onmessage = (e) => {
    const msg = e.data;
    if (msg.type === "ready") {
      // Term.h's cols()/rows() read this from the worker's own global scope
      // (a Worker can't see app.js's `self` directly) — must arrive before
      // the program runs, so send it as soon as the module is up.
      worker.postMessage({ type: "termSize", cols: TERM_COLS, rows: TERM_ROWS });
      runBtn.disabled = false;
      // full = JSPI available: Tensor can reach WebGPU, and Term.read_key
      // can wait for a real keypress so a TUI script is actually playable.
      backendEl.textContent = msg.backend === "full" ? "Tensor: GPU" : "Tensor: CPU";
      backendEl.title = msg.backend === "full"
        ? "WebGPU + interactive TUI available — auto mode runs large tensor ops on the GPU"
        : "Basic build (this browser lacks JSPI/WebGPU) — a TUI script runs non-interactively";
      if (!running) setStatus("ready");
      return;
    }
    if (msg.type === "output") {
      routeOutput(msg.text);
      return;
    }
    if (msg.type === "frame") {
      drawFrame(msg);
      return;
    }
    if (msg.type === "tone") {
      playTone(msg);
      return;
    }
    if (msg.type === "music") {
      handleMusic(msg);
      return;
    }
    if (msg.type === "sound") {
      handleSound(msg);
      return;
    }
    if (msg.type === "done") {
      running = false;
      stopRafPump();
      resetMusic();   // the native analogue: process exit silences the slot
      resetSounds();
      stopBtn.disabled = true;
      runBtn.disabled = false;
      if (!inTui && !inCanvas && output.textContent === "") output.textContent = "(no output)";
      output.classList.toggle("err", msg.rc !== 0);
      if (msg.rc !== 0) {
        markErrorFromOutput();
        switchTab("output");   // a crash mid-TUI/Canvas: show the message, not a frozen frame
      }
      setStatus(msg.rc === 0 ? `done in ${Math.round(msg.ms)} ms` : "error", msg.rc !== 0);
    }
  };
  worker.onerror = (e) => {
    setStatus("worker error", true);
    stopRafPump();
    resetToOutput();
    appendOutput(String(e.message || e));
    output.classList.add("err");
    stopBtn.disabled = true;
    if (running) { // recover like Stop; load failures stay down
      running = false;
      worker.terminate();
      spawnWorker();
    }
  };
}

function run() {
  if (running || runBtn.disabled) return;
  ensureAudio();    // this click is a user gesture — unlock audio for any tones
  resetMusic();     // a fresh run must not inherit the previous run's BGM
  resetSounds();
  running = true;
  runBtn.disabled = true;
  stopBtn.disabled = false;
  output.classList.remove("err");
  output.textContent = "";
  editor.clearError();
  term.reset();
  resetToOutput();
  startRafPump();   // drives Canvas present()'s frame wait; harmless otherwise
  setStatus("running…");
  // The path travels with the source: it is what the program sees as
  // `Sys.script` and what `import` resolves against, so an example finds its
  // assets by the same expression it would use natively.
  worker.postMessage({
    type: "run",
    src: editor.getValue(),
    path: EXAMPLE_PATHS[currentExample] || "main.cul",
    assets: EXAMPLE_ASSETS[currentExample] || [],
    args: EXAMPLE_ARGS[currentExample] || [],
  });
}

// culebra errors end with `at LINE:COL`; surface the first such location in the
// editor so a failed run points at the offending line, not just red text.
function markErrorFromOutput() {
  const m = output.textContent.match(/\bat (\d+):(\d+)/);
  if (m) editor.setError(parseInt(m[1], 10));
}

function stop() {
  if (!running) return;
  worker.terminate();
  running = false;
  stopRafPump();
  resetMusic();     // terminating the worker must also silence a looping BGM
  resetSounds();
  stopBtn.disabled = true;
  runBtn.disabled = true; // until the fresh worker reports ready
  resetToOutput();
  setStatus("stopped — reloading…");
  appendOutput("\n[stopped]");
  spawnWorker();
}

// --- toolbar wiring -------------------------------------------------------

runBtn.addEventListener("click", run);
stopBtn.addEventListener("click", stop);
clearBtn.addEventListener("click", () => {
  output.textContent = "";
  output.classList.remove("err");
});
examplesSel.addEventListener("change", () => {
  const name = examplesSel.value;
  if (!name) return;
  loadExample(name).then((src) => {
    editor.setValue(src);
    editor.focus();
  });
});

document.addEventListener("keydown", (e) => {
  if ((e.metaKey || e.ctrlKey) && e.key === "Enter") {
    e.preventDefault();
    run();
  }
});

// xterm.js decodes real keyboard/mouse DOM events into terminal byte
// sequences itself (including SGR mouse reports, once a TUI script's
// `\x1b[?1002h\x1b[?1006h` mouse-tracking escape reaches it) and delivers
// them all through onData — one registration covers keys and clicks for
// every run, forwarded to whichever worker is currently live.
term.onData((data) => {
  if (worker) worker.postMessage({ type: "key", key: data });
});

// --- Canvas frame / input / audio -----------------------------------------

// Paint a posted RGBA frame. The framebuffer's own dimensions drive the
// backing store (so a script can pick any size); CSS scales it up crisply.
// The first frame of a run auto-switches to the Canvas tab, mirroring how the
// TUI tab reacts to the alt-screen marker.
function drawFrame(msg) {
  if (gameCanvas.width !== msg.w || gameCanvas.height !== msg.h) {
    gameCanvas.width = msg.w;
    gameCanvas.height = msg.h;
  }
  gctx.putImageData(new ImageData(new Uint8ClampedArray(msg.buf), msg.w, msg.h), 0, 0);
  if (!inCanvas) {
    inCanvas = true;
    switchTab("canvas");
  }
}

// A requestAnimationFrame heartbeat paced to a fixed 60 Hz: each forwarded tick
// lets the worker's suspended present() (self.__nextFrame) resolve. Canvas games
// are frame-count based (WASM-4 heritage — gravity is px/frame at 60fps), so the
// tick rate must NOT follow the display's refresh; on a 120 Hz / ProMotion panel
// that ran the game at 2x. We forward at most one tick per 1/60 s of real time,
// carrying the remainder so the average holds at 60 fps on any refresh rate.
// Started for every run (a plain script never waits on it) and stopped when the
// run ends.
const FRAME_MS = 1000 / 60;
let rafId = null;
let nextTickAt = 0;
function startRafPump() {
  if (rafId !== null) return;
  nextTickAt = 0;
  const pump = (now) => {
    if (nextTickAt === 0) nextTickAt = now;
    if (now >= nextTickAt) {
      if (worker) worker.postMessage({ type: "tick" });
      nextTickAt += FRAME_MS;
      // A stall (backgrounded tab, GC pause) must not bank catch-up ticks that
      // fast-forward the game — resync to now instead of replaying the gap.
      if (now >= nextTickAt) nextTickAt = now + FRAME_MS;
    }
    rafId = requestAnimationFrame(pump);
  };
  rafId = requestAnimationFrame(pump);
}
function stopRafPump() {
  if (rafId !== null) {
    cancelAnimationFrame(rafId);
    rafId = null;
  }
}

// WebAudio implementation of Canvas.tone — a small WASM-4-style APU. A note
// slides start->end frequency under an ADSR envelope on one of four channels:
// two pulse waves with a selectable duty cycle (via a cached PeriodicWave), a
// triangle, and noise (white noise through a sweeping lowpass, which reads as a
// pitched hiss). Channels are monophonic — a new note cuts the previous one on
// the same channel, like the real APU. The context is created lazily so the
// page needs no audio permission until a program actually plays something.
let audioCtx = null;
const activeVoices = [null, null, null, null, null];  // one slot per channel
const pulseWaves = {};  // duty index -> PeriodicWave, built on demand

// Browsers start an AudioContext "suspended" until a user gesture. Create it
// (once) and resume it if suspended — called both from the first click/key
// anywhere on the page and from playTone, so a program that makes noise right
// away isn't silent. A resume() on an already-running context is a no-op.
function ensureAudio() {
  try {
    if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    if (audioCtx.state === "suspended") audioCtx.resume();
  } catch {
    // Audio unavailable (no device / blocked) — a program stays playable.
  }
  return audioCtx;
}
for (const ev of ["pointerdown", "keydown", "touchstart"]) {
  window.addEventListener(ev, ensureAudio, { passive: true });
}

// A pulse wave of the given duty cycle as a PeriodicWave. Sine-series
// coefficients b_n = (2/(n*pi))(1 - cos(2*pi*n*D)); D=0.5 recovers a square.
function pulseWave(dutyIndex) {
  if (pulseWaves[dutyIndex]) return pulseWaves[dutyIndex];
  const D = [0.125, 0.25, 0.5, 0.75][dutyIndex] ?? 0.5;
  const N = 32;
  const real = new Float32Array(N + 1);
  const imag = new Float32Array(N + 1);
  for (let n = 1; n <= N; n++) {
    imag[n] = (2 / (n * Math.PI)) * (1 - Math.cos(2 * Math.PI * n * D));
  }
  const w = audioCtx.createPeriodicWave(real, imag, { disableNormalization: false });
  pulseWaves[dutyIndex] = w;
  return w;
}

// One second of white noise, reused for every noise voice.
let noiseBuffer = null;
function whiteNoise() {
  if (noiseBuffer) return noiseBuffer;
  const buf = audioCtx.createBuffer(1, audioCtx.sampleRate, audioCtx.sampleRate);
  const d = buf.getChannelData(0);
  for (let i = 0; i < d.length; i++) d[i] = Math.random() * 2 - 1;
  noiseBuffer = buf;
  return buf;
}

function playTone(m) {
  try {
    if (!ensureAudio()) return;
    const now = audioCtx.currentTime;
    const F = (frames) => Math.max(0, frames) / 60;  // frames@60fps -> seconds
    const attackT = F(m.attack), decayT = F(m.decay);
    const sustainT = F(m.sustain), releaseT = F(m.release);
    let total = attackT + decayT + sustainT + releaseT;
    if (total <= 0) total = 1 / 60;  // guarantee an audible blip
    const startF = Math.max(1, m.startFreq);
    const endF = Math.max(1, m.endFreq);
    const G = (v) => Math.max(0, Math.min(1, v / 100)) * 0.2;  // keep it gentle
    const peakG = G(m.peak), susG = G(m.vol);
    const channel = m.channel | 0;

    // Cut any note still playing on this channel (monophony).
    const prev = activeVoices[channel];
    if (prev) { try { prev.stop(now); } catch {} }

    const gain = audioCtx.createGain();
    let src;
    if (channel === 3) {  // noise: white noise through a lowpass that sweeps
      src = audioCtx.createBufferSource();
      src.buffer = whiteNoise();
      src.loop = true;
      const lp = audioCtx.createBiquadFilter();
      lp.type = "lowpass";
      lp.frequency.setValueAtTime(startF * 8, now);
      lp.frequency.linearRampToValueAtTime(endF * 8, now + total);
      src.connect(lp).connect(gain);
    } else {
      const osc = audioCtx.createOscillator();
      if (channel === 0 || channel === 1) osc.setPeriodicWave(pulseWave(m.duty | 0));
      else if (channel === 2) osc.type = "triangle";
      else osc.type = "sawtooth";  // channel 4: culebra extension
      osc.frequency.setValueAtTime(startF, now);
      osc.frequency.linearRampToValueAtTime(endF, now + total);
      osc.connect(gain);
      src = osc;
    }

    // ADSR: 0 -> peak (attack) -> sustain vol (decay) -> hold -> 0 (release).
    // Zero-length phases collapse to instant steps at the same instant.
    let t = now;
    gain.gain.setValueAtTime(0.0001, t);
    t += attackT;  gain.gain.linearRampToValueAtTime(Math.max(0.0001, peakG), t);
    t += decayT;   gain.gain.linearRampToValueAtTime(Math.max(0.0001, susG), t);
    t += sustainT; gain.gain.setValueAtTime(Math.max(0.0001, susG), t);
    t += releaseT; gain.gain.linearRampToValueAtTime(0.0001, t);

    gain.connect(audioCtx.destination);
    src.start(now);
    src.stop(now + total);
    activeVoices[channel] = src;
    src.onended = () => { if (activeVoices[channel] === src) activeVoices[channel] = null; };
  } catch {
    // Audio unavailable (autoplay policy, no device) — a game stays playable.
  }
}

// --- Canvas.music: one streamed-file slot, decoded and driven here ----------
// The worker posts play/stop/pause/resume/volume/seek commands (canvas.h's
// EM_JS side keeps the script-visible playing flags optimistically, in sync
// with the call); this side owns the decoded AudioBuffer and the live source
// node. What only this side can observe — a failed decode, a non-looping file
// running out — is pushed back as a "musicState" correction. A paused voice is
// just a remembered offset: an AudioBufferSourceNode can't restart, so resume
// builds a fresh one, which is also how seek works.
let musicBuffer = null;   // decoded audio, while a file is loaded
let musicVoice = null;    // { src, gain, startedAt } while audible
let musicLoop = true;
let musicGain = 0.2;
let musicPausedAt = null; // seconds into the buffer, while paused
let musicSeq = 0;         // play generation: a stale decode must not resurrect

const musicG = (v) => Math.max(0, Math.min(1, v / 100)) * 0.2; // tone's scale

function musicNotify(playing, loaded) {
  if (worker) worker.postMessage({ type: "musicState", playing, loaded });
}

function stopMusicVoice() {
  if (!musicVoice) return;
  const v = musicVoice;
  musicVoice = null;               // cleared first: onended sees it was told to
  try { v.src.stop(); } catch {}
}

function startMusicVoice(offset) {
  if (!musicBuffer || !ensureAudio()) return;
  stopMusicVoice();
  const src = audioCtx.createBufferSource();
  src.buffer = musicBuffer;
  src.loop = musicLoop;
  const gain = audioCtx.createGain();
  gain.gain.value = musicGain;
  src.connect(gain).connect(audioCtx.destination);
  const at = musicBuffer.duration > 0 ? offset % musicBuffer.duration : 0;
  const v = { src, gain, startedAt: audioCtx.currentTime - at };
  src.onended = () => {
    if (musicVoice === v) {        // ran out on its own (non-looping)
      musicVoice = null;
      musicBuffer = null;
      musicNotify(false, false);
    }
  };
  src.start(0, at);
  musicVoice = v;
  musicPausedAt = null;
}

function musicPosition() {
  if (musicVoice && musicBuffer && musicBuffer.duration > 0) {
    return (audioCtx.currentTime - musicVoice.startedAt) % musicBuffer.duration;
  }
  return musicPausedAt ?? 0;
}

function resetMusic() {
  stopMusicVoice();
  musicBuffer = null;
  musicPausedAt = null;
  musicSeq++;
}

function handleMusic(m) {
  try {
    switch (m.cmd) {
      case "play": {
        resetMusic();
        musicLoop = !!m.loop;
        musicGain = musicG(m.vol);
        if (!ensureAudio()) { musicNotify(false, false); return; }
        // If the context is still suspended (no gesture yet) the voice is
        // created anyway: currentTime is frozen, so playback simply begins
        // when the first click/keydown resumes the context.
        const seq = musicSeq;
        audioCtx.decodeAudioData(m.buf.buffer).then((buf) => {
          if (seq !== musicSeq) return;   // superseded while decoding
          musicBuffer = buf;
          startMusicVoice(Math.max(0, m.start || 0));
        }).catch(() => {
          if (seq === musicSeq) musicNotify(false, false);
        });
        break;
      }
      case "stop":
        resetMusic();
        break;
      case "pause":
        if (musicVoice) {
          musicPausedAt = musicPosition();
          stopMusicVoice();
        }
        break;
      case "resume":
        if (!musicVoice && musicBuffer && musicPausedAt !== null) {
          startMusicVoice(musicPausedAt);
        }
        break;
      case "volume":
        musicGain = musicG(m.vol);
        if (musicVoice) musicVoice.gain.gain.value = musicGain;
        break;
      case "seek": {
        const s = Math.max(0, m.seconds || 0);
        if (musicVoice) startMusicVoice(s);
        else if (musicBuffer) musicPausedAt = s;
        break;
      }
    }
  } catch {
    // Audio unavailable (autoplay policy, no device) — a game stays playable.
  }
}

// Sound effects: many decoded buffers keyed by the wasm-side handle, one live
// voice per handle (play restarts it, like raylib's PlaySound). Only what
// this side can observe — a one-shot running out, a failed decode — goes back
// as a "soundState" correction.
const soundBuffers = new Map();  // id -> AudioBuffer
const soundVoices = new Map();   // id -> { src } while audible

function soundNotify(id, playing) {
  if (worker) worker.postMessage({ type: "soundState", id, playing });
}

function stopSoundVoice(id) {
  const v = soundVoices.get(id);
  if (!v) return;
  soundVoices.delete(id);          // cleared first: onended sees it was told to
  try { v.src.stop(); } catch {}
}

function resetSounds() {
  for (const id of [...soundVoices.keys()]) stopSoundVoice(id);
  soundBuffers.clear();
}

function handleSound(m) {
  try {
    switch (m.cmd) {
      case "load":
        if (!ensureAudio()) return;
        audioCtx.decodeAudioData(m.buf.buffer)
          .then((buf) => soundBuffers.set(m.id, buf))
          .catch(() => soundNotify(m.id, false));
        break;
      case "play": {
        const buf = soundBuffers.get(m.id);
        if (!buf || !ensureAudio()) { soundNotify(m.id, false); break; }
        stopSoundVoice(m.id);
        const src = audioCtx.createBufferSource();
        src.buffer = buf;
        const gain = audioCtx.createGain();
        gain.gain.value = musicG(m.vol);   // the same 0..100 scale as tone
        src.connect(gain).connect(audioCtx.destination);
        const v = { src };
        src.onended = () => {
          if (soundVoices.get(m.id) === v) {   // ran out on its own
            soundVoices.delete(m.id);
            soundNotify(m.id, false);
          }
        };
        src.start();
        soundVoices.set(m.id, v);
        break;
      }
      case "stop":
        stopSoundVoice(m.id);
        break;
      case "free":
        stopSoundVoice(m.id);
        soundBuffers.delete(m.id);
        break;
    }
  } catch {
    // Audio unavailable (autoplay policy, no device) — a game stays playable.
  }
}

// Keyboard → button bitmask (bits match src/preambles/canvas.cul: LEFT=1,
// RIGHT=2, UP=4, DOWN=8, A=16, B=32). WASD doubles the d-pad so a game that
// wants a hand on each side of the keyboard works without remapping. Captured
// on the focused Canvas pane so arrows/space don't also scroll the page. The
// worker keeps the held mask in self.__canvasButtons for the wasm side to poll.
const KEY_BITS = {
  ArrowLeft: 1, ArrowRight: 2, ArrowUp: 4, ArrowDown: 8,
  a: 1, A: 1, d: 2, D: 2, w: 4, W: 4, s: 8, S: 8,
  " ": 16, z: 16, Z: 16, x: 32, X: 32,
};
let heldButtons = 0;

// e.code → the culebra key name (Term.read_key's vocabulary): a printable
// character or a special-key name. Keyed on the physical code so a key's
// identity survives Shift changing e.key between its down and up events;
// e.key fills in printable keys the table doesn't list (other layouts).
const CODE_NAMES = {
  Space: " ", ArrowLeft: "left", ArrowRight: "right", ArrowUp: "up",
  ArrowDown: "down", Enter: "enter", Escape: "escape", Tab: "tab",
  Backspace: "backspace", Insert: "insert", Delete: "delete", Home: "home",
  End: "end", PageUp: "pageup", PageDown: "pagedown",
  Minus: "-", Equal: "=", Comma: ",", Period: ".", Slash: "/",
  Semicolon: ";", Quote: "'", BracketLeft: "[", BracketRight: "]",
  Backslash: "\\", Backquote: "`",
};
function keyName(e) {
  const c = e.code;
  if (CODE_NAMES[c] !== undefined) return CODE_NAMES[c];
  if (/^Key[A-Z]$/.test(c)) return c.slice(3).toLowerCase();
  if (/^Digit[0-9]$/.test(c)) return c.slice(5);
  if (/^F([1-9]|1[0-2])$/.test(c)) return c.toLowerCase();
  if (e.key.length === 1) return e.key.toLowerCase();
  return null;
}
// Held names + this frame's presses/typed characters ride the same "input"
// message as the button mask; worker.js keeps the wasm-visible state.
const heldKeys = new Set();
function sendInput(extra) {
  if (worker) worker.postMessage(Object.assign(
      { type: "input", buttons: heldButtons, keys: [...heldKeys] }, extra));
}
canvasPane.addEventListener("keydown", (e) => {
  const name = keyName(e);
  const bit = KEY_BITS[e.key];
  if (bit === undefined && name === null) return;
  e.preventDefault();
  if (bit !== undefined) heldButtons |= bit;
  const extra = {};
  if (name !== null) {
    heldKeys.add(name);
    if (!e.repeat) extra.keyEvents = [name];
    // The typed character respects Shift/layout (e.key), unlike the name.
    if (e.key.length === 1) extra.chars = [e.key];
  }
  sendInput(extra);
});
canvasPane.addEventListener("keyup", (e) => {
  const name = keyName(e);
  const bit = KEY_BITS[e.key];
  if (bit === undefined && name === null) return;
  e.preventDefault();
  if (bit !== undefined) heldButtons &= ~bit;
  if (name !== null) heldKeys.delete(name);
  sendInput({});
});
// Alt-tab / focus loss never delivers the keyup: release everything.
window.addEventListener("blur", () => {
  heldButtons = 0;
  heldKeys.clear();
  sendInput({});
});

// Pointer → framebuffer coordinates (accounting for the CSS scale-up).
function sendMouse(e) {
  const r = gameCanvas.getBoundingClientRect();
  const x = Math.floor(((e.clientX - r.left) / r.width) * gameCanvas.width);
  const y = Math.floor(((e.clientY - r.top) / r.height) * gameCanvas.height);
  if (worker) worker.postMessage({ type: "canvasMouse", x, y, buttons: e.buttons });
}
for (const ev of ["pointermove", "pointerdown", "pointerup"]) {
  gameCanvas.addEventListener(ev, sendMouse);
}
// The right mouse button is a real input (Rocci Bird restarts on it), so
// suppress the context menu over the canvas.
gameCanvas.addEventListener("contextmenu", (e) => e.preventDefault());

// --- boot -----------------------------------------------------------------

loadExampleCatalog()
  .then(() => loadExample("Hello"))
  .then((src) => editor.setValue(src))
  .catch((err) => console.error("failed to load example catalog", err));
setStatus("loading…");
spawnWorker();
