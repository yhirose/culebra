// The FM Synth's page: the patch's knobs, the presets, the MIDI inputs and a
// keyboard. The sound is the program's (engine/engine.cul): the page sends it
// the patch as it is edited, and the notes it plays, over one WebSocket.

"use strict";

// ---- the wiring's pictures; which operator modulates which is the engine's
// (fm.ALGORITHMS), sent with the patch

const PICTURES = {
  stack: { name: "Stack", about: "3 → 2 → 1. Rich, brassy", at: [[32, 50], [32, 30], [32, 10]] },
  pair: { name: "Two into one", about: "2 and 3 → 1. Bells, metal", at: [[32, 46], [16, 18], [48, 18]] },
  split: { name: "Pair + tone", about: "2 → 1, and 3 alone. Electric piano", at: [[18, 46], [18, 18], [46, 46]] },
  organ: { name: "All heard", about: "1, 2 and 3 side by side. Organ", at: [[12, 40], [32, 40], [52, 40]] },
};

// ---- how a slider's position (0..1000) maps to a value in [lo, hi], and back
// (the ranges are the engine's, sent with the patch)

const log = ([lo, hi]) => ({
  value: (p) => lo * Math.pow(hi / lo, p / 1000),
  pos: (v) => (1000 * Math.log(v / lo)) / Math.log(hi / lo),
});
const linear = ([lo, hi]) => ({
  value: (p) => lo + ((hi - lo) * p) / 1000,
  pos: (v) => (1000 * (v - lo)) / (hi - lo),
});
// A ratio near a whole number (or a half) is that number: the harmonic ones
// are the ones most wanted.
const ratio = (range) => {
  const scale = log(range);
  return {
    value: (p) => {
      const r = scale.value(p);
      const near = r < 1 ? Math.round(r * 2) / 2 : Math.round(r);
      return Math.abs(r - near) < 0.04 * r ? near : Math.round(r * 100) / 100;
    },
    pos: scale.pos,
  };
};

const showTime = (s) => (s < 1 ? `${Math.round(s * 1000)} ms` : `${s.toFixed(2)} s`);
const showRatio = (r) => `× ${r % 1 === 0 ? r : r.toFixed(2)}`;
const showPercent = (v) => `${Math.round(v)}`;
const showCents = (c) => `${c > 0 ? "+" : ""}${Math.round(c)} ct`;

// ---- state

let patch = null;        // the patch as edited
let loaded = "";         // the patch as loaded or saved, to tell an edit
let presetNames = { factory: [], mine: [] };
let inputs = [];
let socket = null;
let algorithms = null;   // the engine's wiring, by id; null until the first patch
const knobs = [];        // every knob, to show the patch

const $ = (id) => document.getElementById(id);
const el = (tag, props = {}, children = []) => {
  const e = Object.assign(document.createElement(tag), props);
  for (const c of children) e.append(c);
  return e;
};

// ---- the connection

function connect() {
  socket = new WebSocket(`ws://${location.host}/api/synth`);
  socket.onopen = () => hideMessage();
  socket.onmessage = (ev) => receive(JSON.parse(ev.data));
  socket.onclose = () => {
    showMessage("The sound engine is not answering. Trying again…");
    setTimeout(connect, 1000);
  };
}

function send(msg) {
  if (socket && socket.readyState === WebSocket.OPEN) socket.send(JSON.stringify(msg));
}

// The patch is sent as it changes, at most every 30 ms while a knob moves.
let sendTimer = null;
function patchChanged() {
  showEdited();
  if (sendTimer) return;
  sendTimer = setTimeout(() => {
    sendTimer = null;
    send({ type: "patch", patch });
  }, 30);
}

function receive(msg) {
  if (msg.type === "patch") {
    if (!algorithms) build(msg.algorithms, msg.ranges);
    patch = msg.patch;
    loaded = JSON.stringify(patch);
    presetNames = msg.presets;
    showPresets();
    showPatch();
  } else if (msg.type === "presets") {
    presetNames = msg.presets;
    showPresets();
  } else if (msg.type === "inputs") {
    inputs = msg.inputs;
    $("breath-source").value = msg.breath;
    showInputs();
  } else if (msg.type === "played") {
    showPlayed(msg);
  } else if (msg.type === "error") {
    showMessage(msg.text);
  }
}

// ---- messages

let messageTimer = null;
function showMessage(text) {
  const m = $("message");
  m.textContent = text;
  m.hidden = false;
  clearTimeout(messageTimer);
  messageTimer = setTimeout(hideMessage, 6000);
}
function hideMessage() {
  $("message").hidden = true;
}

// ---- presets

function allPresets() {
  return [...presetNames.factory, ...presetNames.mine];
}

function showPresets() {
  const select = $("preset");
  select.replaceChildren();
  const group = (label, names) => {
    if (names.length === 0) return;
    const g = el("optgroup", { label });
    for (const n of names) g.append(el("option", { value: n, textContent: n }));
    select.append(g);
  };
  group("Presets", presetNames.factory);
  group("My presets", presetNames.mine);
  if (!allPresets().includes(patch.name)) {
    select.prepend(el("option", { value: patch.name, textContent: patch.name }));
  }
  select.value = patch.name;
  $("delete").disabled = !presetNames.mine.includes(patch.name);
  disarmDelete();
  showEdited();
}

function showEdited() {
  $("edited").hidden = JSON.stringify(patch) === loaded;
}

function loadPreset(name) {
  send({ type: "load", name });
}

function step(by) {
  const names = allPresets();
  const i = names.indexOf(patch.name);
  // A patch that is no preset steps to the first or the last.
  if (i < 0) return loadPreset(names[by > 0 ? 0 : names.length - 1]);
  loadPreset(names[(i + by + names.length) % names.length]);
}

function askName(initial) {
  const form = $("name-form");
  form.hidden = false;
  const input = $("name-input");
  input.value = initial;
  input.focus();
  input.select();
}

function save() {
  // One of the player's own is saved over; one of the app's needs a name.
  if (presetNames.mine.includes(patch.name)) send({ type: "save", name: patch.name, patch });
  else askName(`My ${patch.name}`);
}

// Delete asks for a second click within 3 s, on the same preset.
let deleteArmed = null;
function disarmDelete() {
  clearTimeout(deleteArmed);
  deleteArmed = null;
  $("delete").textContent = "Delete";
}
function deletePreset() {
  if (!deleteArmed) {
    $("delete").textContent = "Click again to delete";
    deleteArmed = setTimeout(disarmDelete, 3000);
    return;
  }
  disarmDelete();
  const names = allPresets();
  const next = names[(names.indexOf(patch.name) + 1) % names.length];
  send({ type: "delete", name: patch.name });
  if (next && next !== patch.name) loadPreset(next);
}

// ---- inputs

function showInputs() {
  const box = $("inputs");
  box.replaceChildren();
  if (inputs.length === 0) {
    box.append(el("span", { className: "no-inputs", textContent: "No MIDI device. Plug one in, or play the keys below." }));
  }
  for (const input of inputs) {
    const mode = el("div", { className: "mode" });
    for (const [value, label] of [["keys", "Keys"], ["wind", "Wind"]]) {
      const b = el("button", { textContent: label, className: input.mode === value ? "active" : "" });
      b.title = value === "wind" ? "A wind controller (EWI): one voice, played by breath" : "A keyboard: chords, with the sustain pedal";
      b.onclick = () => send({ type: "mode", name: input.name, mode: value });
      mode.append(b);
    }
    box.append(el("div", { className: "input-chip" }, [
      el("span", { className: "dot", id: `dot-${input.id}` }),
      el("span", { textContent: input.name }),
      mode,
    ]));
  }
  $("breath-source-box").hidden = !inputs.some((i) => i.mode === "wind");
}

// ---- knobs

// A slider for `get`/`set` of the patch, shown on `scale` as `show` says;
// `onChange` redraws what else shows the value. One not `live` sends the
// patch when let go: its change builds new voices, restarting held notes.
function knob(label, scale, show, get, set, { big = false, reset = null, onChange = null, live = true } = {}) {
  const value = el("span", { className: "value" });
  const labelEl = el("span", { textContent: label });
  const input = el("input", { type: "range", min: 0, max: 1000, step: 1 });
  input.oninput = () => {
    set(scale.value(Number(input.value)));
    value.textContent = show(get());
    if (live) patchChanged();
    else showEdited();
    onChange?.();
  };
  input.onchange = () => {
    if (!live) patchChanged();
  };
  // A double click puts the knob back where it was when the preset loaded.
  input.ondblclick = () => {
    if (reset === null) return;
    set(reset());
    k.update();
    patchChanged();
    onChange?.();
  };
  const k = {
    root: el("div", { className: `knob${big ? " big" : ""}` }, [el("div", { className: "top" }, [labelEl, value]), input]),
    update() {
      input.value = Math.round(scale.pos(get()));
      value.textContent = show(get());
    },
    label(text) {
      labelEl.textContent = text;
    },
  };
  knobs.push(k);
  return k;
}

const loadedPatch = () => JSON.parse(loaded);

// The page's parts that need the engine's wiring and ranges, made once they
// come with the first patch.
function build(wiring, ranges) {
  algorithms = wiring;
  buildAlgorithms();
  buildOps(ranges.op, ranges.patch);
  buildOverall(ranges.patch);
}

function buildAlgorithms() {
  const box = $("algorithms");
  for (const [id, picture] of Object.entries(PICTURES)) {
    const a = { ...algorithms[id], ...picture };
    const b = el("button", { className: "algorithm" });
    b.dataset.id = id;
    b.append(diagram(a), el("div", {}, [
      el("div", { className: "name", textContent: a.name }),
      el("div", { className: "about", textContent: a.about }),
    ]));
    b.onclick = () => {
      patch.algorithm = id;
      patchChanged();
      showPatch();
    };
    box.append(b);
  }
}

// The algorithm's picture: a numbered box per operator, an arrow from each
// modulator to what it changes, and a line from each heard one to the
// speaker (the bar at the bottom).
function diagram(a) {
  const ns = "http://www.w3.org/2000/svg";
  const svg = document.createElementNS(ns, "svg");
  svg.setAttribute("width", "64");
  svg.setAttribute("height", "68");
  const add = (tag, attrs) => {
    const e = document.createElementNS(ns, tag);
    for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
    svg.append(e);
    return e;
  };
  const colors = ["var(--op1)", "var(--op2)", "var(--op3)"];
  a.mods.forEach((ms, to) => {
    for (const from of ms) {
      const [x1, y1] = a.at[from], [x2, y2] = a.at[to];
      add("line", { x1, y1: y1 + 7, x2, y2: y2 - 7, stroke: "#9aa1b0", "stroke-width": 1.5 });
      add("circle", { cx: x2, cy: y2 - 8, r: 2, fill: "#9aa1b0" });
    }
  });
  for (const c of a.carriers) {
    const [x, y] = a.at[c];
    add("line", { x1: x, y1: y + 7, x2: x, y2: 64, stroke: "#9aa1b0", "stroke-width": 1.5 });
  }
  add("rect", { x: 4, y: 63, width: 56, height: 3, rx: 1.5, fill: "#5fd37a" });
  a.at.forEach(([x, y], i) => {
    add("rect", { x: x - 8, y: y - 7, width: 16, height: 14, rx: 3, fill: colors[i] });
    const t = add("text", { x, y: y + 4, "text-anchor": "middle", "font-size": 11, "font-weight": 700, fill: "#1a1a1a" });
    t.textContent = String(i + 1);
  });
  return svg;
}

const opCards = [];

function buildOps(range, patchRange) {
  const box = $("ops");
  for (let i = 0; i < 3; i++) {
    const op = () => patch.ops[i];
    const reset = (key) => () => loadedPatch().ops[i][key];
    const role = el("div", { className: "role" });
    const roleAbout = el("div", { className: "role-about" });
    const opKnob = (label, key, scale, show, options = {}) =>
      knob(label, scale(range[key]), show, () => op()[key], (v) => (op()[key] = v), { reset: reset(key), ...options });
    const level = opKnob("Volume", "level", linear, showPercent, { big: true });
    const r = opKnob("Pitch", "ratio", ratio, showRatio);
    const fine = opKnob("Fine tune", "detune", linear, showCents);
    const env = el("canvas", { className: "env-graph", width: 240, height: 52 });
    // A canvas takes a color, not a CSS variable: the variable read once.
    const color = getComputedStyle(document.documentElement).getPropertyValue(`--op${i + 1}`).trim();
    const envelopeKnob = { onChange: () => drawEnvelope(env, op(), color), live: false };
    const parts = [
      el("header", {}, [el("span", { className: "num", textContent: String(i + 1) }), el("div", {}, [role, roleAbout])]),
      level.root,
      r.root,
      fine.root,
    ];
    if (i === 2) {
      parts.push(knob("Grit (feedback)", linear(patchRange.feedback), showPercent, () => patch.feedback, (v) => (patch.feedback = v), { reset: () => loadedPatch().feedback }).root);
    }
    parts.push(env, el("div", { className: "env-grid" }, [
      opKnob("Attack", "attack", log, showTime, envelopeKnob).root,
      opKnob("Decay", "decay", log, showTime, envelopeKnob).root,
      opKnob("Sustain", "sustain", linear, showPercent, envelopeKnob).root,
      opKnob("Release", "release", log, showTime, envelopeKnob).root,
    ]));
    const card = el("section", { className: "op" }, parts);
    card.style.setProperty("--op-color", `var(--op${i + 1})`);
    box.append(card);
    opCards.push({ card, role, roleAbout, level, redraw: envelopeKnob.onChange });
  }
}

function buildOverall(range) {
  const reset = (key) => () => loadedPatch()[key];
  const g = (label, key, show) =>
    knob(label, linear(range[key]), show, () => patch[key], (v) => (patch[key] = v), { big: true, reset: reset(key) }).root;
  $("overall").append(
    g("Volume", "volume", showPercent),
    g("Reverb", "reverb", showPercent),
  );
  $("wind").append(
    g("Breath brightness", "breath_bright", showPercent),
    g("Glide", "glide", showTime),
  );
}

// The operators' roles as the wiring makes them, and each envelope's shape.
function showRoles() {
  const a = algorithms[patch.algorithm];
  document.querySelectorAll(".algorithm").forEach((b) => b.classList.toggle("active", b.dataset.id === patch.algorithm));
  opCards.forEach((c, i) => {
    const target = a.mods.findIndex((ms) => ms.includes(i));
    if (a.carriers.includes(i)) {
      c.role.textContent = "Heard";
      c.roleAbout.textContent = "You hear this one. Its level is its volume.";
      c.level.label("Volume");
    } else {
      c.role.textContent = `Changes ${target + 1}`;
      c.roleAbout.textContent = `Not heard itself: the more of it, the brighter ${target + 1} sounds.`;
      c.level.label("Brightness");
    }
    c.redraw();
  });
}

function drawEnvelope(canvas, op, color) {
  const ctx = canvas.getContext("2d");
  const w = canvas.width, h = canvas.height, pad = 4;
  ctx.clearRect(0, 0, w, h);
  // Times are drawn on a soft scale, so a long decay does not hide a short attack.
  const span = (s) => Math.sqrt(s);
  const a = span(op.attack), d = span(op.decay), hold = 0.6, r = span(op.release);
  const total = a + d + hold + r;
  const x = (t) => pad + ((w - 2 * pad) * t) / total;
  const y = (level) => h - pad - (h - 2 * pad) * level;
  const s = op.sustain / 100;
  ctx.beginPath();
  ctx.moveTo(x(0), y(0));
  ctx.lineTo(x(a), y(1));
  ctx.lineTo(x(a + d), y(s));
  ctx.lineTo(x(a + d + hold), y(s));
  ctx.lineTo(x(total), y(0));
  ctx.strokeStyle = color;
  ctx.lineWidth = 2;
  ctx.stroke();
}

function showPatch() {
  for (const k of knobs) k.update();
  showRoles();
  showEdited();
}

// ---- what is played

function showPlayed(msg) {
  const sounding = new Set(msg.keys);
  for (const [note, key] of keyEls) {
    key.classList.toggle("down", sounding.has(note) || mouseNotes.has(note));
    key.classList.toggle("finger", msg.finger === note);
  }
  $("breath-meter").style.width = `${Math.round(msg.breath * 100)}%`;
  const active = new Set(msg.active);
  for (const input of inputs) $(`dot-${input.id}`)?.classList.toggle("on", active.has(input.id));
}

// ---- the keyboard on the page

const KEY_COUNT = 25;
// The computer's keys by position (KeyboardEvent.code), so that Shift or the
// keyboard's layout does not change which note a key is.
const PC_KEYS = ["KeyA", "KeyW", "KeyS", "KeyE", "KeyD", "KeyF", "KeyT", "KeyG", "KeyY", "KeyH",
  "KeyU", "KeyJ", "KeyK", "KeyO", "KeyL", "KeyP", "Semicolon", "Quote"];
const PC_LABELS = "awsedftgyhujkolp;'";
const BLACK = new Set([1, 3, 6, 8, 10]);
let base = 48;
const keyEls = new Map();
const mouseNotes = new Set();
const pcNotes = new Map();

function noteOn(note) {
  send({ type: "key", on: true, note });
  keyEls.get(note)?.classList.add("down");
}
function noteOff(note) {
  send({ type: "key", on: false, note });
  keyEls.get(note)?.classList.remove("down");
}

function buildKeyboard() {
  const kb = $("keyboard");
  kb.replaceChildren();
  keyEls.clear();
  const whites = [];
  for (let i = 0; i < KEY_COUNT; i++) if (!BLACK.has(i % 12)) whites.push(i);
  const whiteWidth = 100 / whites.length;
  for (let i = 0; i < KEY_COUNT; i++) {
    const note = base + i;
    const black = BLACK.has(i % 12);
    const key = el("div", { className: `key${black ? " black" : ""}` });
    if (i < PC_KEYS.length) key.append(el("span", { className: "label", textContent: PC_LABELS[i].toUpperCase() }));
    if (black) {
      const left = whites.filter((w) => w < i).length;
      key.style.left = `calc(${left * whiteWidth}% - ${whiteWidth * 0.3}%)`;
      key.style.width = `${whiteWidth * 0.6}%`;
    }
    // The left button plays; dragging with it held plays the keys passed.
    // Mouse events, not pointer events: WKWebView sends no pointerdown for
    // the first click after a <select>'s menu closes.
    key.onmousedown = (e) => {
      if (e.button !== 0) return;
      e.preventDefault();
      mouseNotes.add(note);
      noteOn(note);
    };
    key.onmouseenter = (e) => {
      if (!(e.buttons & 1)) return;
      mouseNotes.add(note);
      noteOn(note);
    };
    key.onmouseleave = () => {
      if (!mouseNotes.has(note)) return;
      mouseNotes.delete(note);
      noteOff(note);
    };
    kb.append(key);
    keyEls.set(note, key);
  }
  $("octave-label").textContent = `C${Math.floor(base / 12) - 1}`;
}

window.addEventListener("mouseup", () => {
  for (const n of mouseNotes) noteOff(n);
  mouseNotes.clear();
});

function shiftOctave(by) {
  const next = base + 12 * by;
  if (next < 24 || next > 84) return;
  for (const n of pcNotes.values()) noteOff(n);
  pcNotes.clear();
  base = next;
  buildKeyboard();
}

const typing = (e) => e.target instanceof HTMLInputElement && e.target.type !== "range";

window.addEventListener("keydown", (e) => {
  if (typing(e) || e.metaKey || e.ctrlKey || e.altKey) return;
  const k = e.code;
  if (k === "KeyZ") return shiftOctave(-1);
  if (k === "KeyX") return shiftOctave(1);
  const i = PC_KEYS.indexOf(k);
  if (i < 0 || e.repeat || pcNotes.has(k)) return;
  e.preventDefault();
  pcNotes.set(k, base + i);
  noteOn(base + i);
});

window.addEventListener("keyup", (e) => {
  const k = e.code;
  if (!pcNotes.has(k)) return;
  noteOff(pcNotes.get(k));
  pcNotes.delete(k);
});

// A key held when the window loses focus never gets its keyup.
window.addEventListener("blur", () => {
  for (const n of pcNotes.values()) noteOff(n);
  pcNotes.clear();
});

// ---- wiring the page up

$("preset").onchange = (e) => loadPreset(e.target.value);
$("prev").onclick = () => step(-1);
$("next").onclick = () => step(1);
$("save").onclick = save;
$("save-as").onclick = () => askName(patch.name);
$("delete").onclick = deletePreset;
$("panic").onclick = () => send({ type: "all_off" });
$("oct-down").onclick = () => shiftOctave(-1);
$("oct-up").onclick = () => shiftOctave(1);
$("breath-source").onchange = (e) => send({ type: "breath", source: e.target.value });
$("name-form").onsubmit = (e) => {
  e.preventDefault();
  const name = $("name-input").value.trim();
  if (!name) return;
  $("name-form").hidden = true;
  send({ type: "save", name, patch });
};
$("name-cancel").onclick = () => ($("name-form").hidden = true);

buildKeyboard();
connect();
