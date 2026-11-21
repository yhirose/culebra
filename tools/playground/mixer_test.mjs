// The Playground's mixer (site/playground/audio-engine.wasm, run by
// audio-worklet.js on the page's audio thread) fed what the committed
// Playground wasm sends it, as the page wires them: a graph played is the
// graph the same program renders, sample for sample; on a mixer that came
// up late, a graph keeps the program's time; a tone sounds, at once on a
// mixer held still until it was asked for; a Sound decodes for a song; and
// a second run in the same instance plays as the first did (what a run
// leaves playing is let go).
//
//   node tools/playground/mixer_test.mjs

import fs from "node:fs";
import path from "node:path";
import { root, loadPlayground } from "./wasm.mjs";

globalThis.self = globalThis;  // the worker's scope, as wasm.mjs gives it
const site = path.join(root, "site/playground");
const engineBytes = fs.readFileSync(path.join(site, "audio-engine.wasm"));
const { engineImports, mixerDecoder } = await import(path.join(site, "audio-engine.js"));

// The worklet, in a scope that stands in for an AudioWorkletGlobalScope.
let Processor = null;
globalThis.AudioWorkletProcessor = class {
  constructor() {
    this.port = { postMessage() {}, onmessage: null };
  }
};
globalThis.registerProcessor = (_name, cls) => (Processor = cls);
await import(path.join(site, "audio-worklet.js"));
const module = new WebAssembly.Module(engineBytes);

// The worker's decoder, as worker.js makes it.
self.__decodeForMixer = mixerDecoder((await WebAssembly.instantiate(engineBytes, engineImports)).instance.exports);
const playground = await loadPlayground("basic");

// One program run with the mixer's port: what it printed, and the mixer's
// left and right over `frames` after every call but the run's last (the
// reset that lets go of what it left playing). The mixer comes up after the
// run, as late as the run took; with `still`, the wall clock stands still
// meanwhile, so it is up in time.
async function play(src, frames, { still = false } = {}) {
  const wall = Date.now;
  if (still) {
    const at = wall();
    Date.now = () => at;
  }
  try {
    return await playWith(src, frames);
  } finally {
    Date.now = wall;
  }
}
async function playWith(src, frames) {
  const sent = [];
  self.__audioPort = { postMessage: (m) => sent.push(m) };
  self.__audioStatus = null;
  const out = await run(src);
  const worker = { postMessage() {}, close() {}, onmessage: null };
  const mixer = newMixer(worker);
  const calls = sent.slice(0, -1);
  for (const m of calls) worker.onmessage({ data: m });
  return { out, ...render(mixer, frames), calls };
}

// One program run on a mixer made before it and held still (a page's
// AudioContext, made as the page opens, stays suspended until its first
// gesture): the mixer takes each call but the run's last (the reset that
// lets go of what it left playing) as it comes, the wall clock moving
// `stall` ms on after the run's first (its reset), and renders only after.
// Answers the mixer's left over `frames`.
async function playOnHeldMixer(src, frames, stall) {
  const wall = Date.now;
  let now = wall();
  Date.now = () => now;
  try {
    // The worker takes the mixer's word on the stream as worker.js does.
    const worker = {
      postMessage: ({ status, at }) => {
        const held = {};
        for (let i = 1; i + 1 < status.length; i += 2) held[status[i]] = status[i + 1];
        self.__audioStatus = { pos: status[0], at, held };
      },
      close() {},
      onmessage: null,
    };
    self.__audioStatus = null;
    const mixer = newMixer(worker);
    const sent = [];
    self.__audioPort = {
      postMessage: (m) => {
        sent.push({ m, at: now });
        if (sent.length === 1) now += stall;
      },
    };
    await run(src);
    const end = now;
    for (const { m, at } of sent.slice(0, -1)) {
      now = at;
      worker.onmessage({ data: m });
    }
    now = end;
    return render(mixer, frames).left;
  } finally {
    Date.now = wall;
  }
}

// The program run, the worker's audio state fresh (the mixer's word kept):
// what it printed.
async function run(src) {
  self.__soundPcm = {};
  const { out, failed } = await playground.run("/work/mixer.cul", src);
  if (failed) throw new Error("the program failed:\n" + out);
  return out;
}
// The worklet, its port to the worker `worker`.
function newMixer(worker) {
  const mixer = new Processor({ processorOptions: { module } });
  mixer.port.onmessage({ data: { port: worker } });
  return mixer;
}
// The mixer's left and right over `frames`.
function render(mixer, frames) {
  const left = new Float32Array(frames), right = new Float32Array(frames);
  const block = [new Float32Array(128), new Float32Array(128)];
  for (let at = 0; at < frames; at += 128) {
    mixer.process([], [block]);
    left.set(block[0].subarray(0, Math.min(128, frames - at)), at);
    right.set(block[1].subarray(0, Math.min(128, frames - at)), at);
  }
  return { left, right };
}

let bad = 0;
function check(ok, what) {
  if (!ok) {
    console.error("mixer FAIL: " + what);
    bad++;
  }
}

// A graph played on a mixer up in time is the graph rendered, twice over in
// one instance.
const graph = `let {saw, line, render, play} = Synth
let sound = saw(220.0).lowpass(line(400.0, 2000.0, 0.5)) * 0.3
println(JSON.stringify(render(sound, 0.5)))
play(sound, wait: false)
`;
for (const time of ["first", "second"]) {
  const { out, left, right } = await play(graph, 22050, { still: true });
  const want = JSON.parse(out).map((v) => Math.fround(Math.max(-1, Math.min(1, v))));
  const off = want.findIndex((v, i) => v !== left[i] || v !== right[i]);
  check(off < 0, `the ${time} run's graph plays otherwise than it renders, from sample ${off}`);
  check(want.some((v) => v !== 0), `the ${time} run's graph renders silence`);
}

// A sound played to its end returns once it has played, with no word from
// the mixer: the worker's clock runs on its own until one comes.
{
  const t0 = performance.now();
  await play("let {saw, play} = Synth\nplay(saw(220.0) * 0.2, secs: 0.3)\n", 128);
  const ms = performance.now() - t0;
  check(ms >= 250, `a sound of 0.3 s played to its end returned after ${Math.round(ms)} ms`);
}

// A mixer that comes up after a graph started (its module came after the
// program began) plays the graph on the program's time: a change
// for now sounds at once, not as late as the mixer was.
{
  const { left } = await play(`let {sine, control, play} = Synth
let h = play(sine(440.0) * control("level", 0.0), wait: false)
Time.sleep(0.5)
h.set("level", 0.3)
`, 8820);
  const first = left.findIndex((v) => v !== 0);
  check(first >= 0 && first < 441, `a change for now, on a mixer that came up late, sounds from sample ${first}, not at once`);
}

// A tone sounds, and is gone at the next run.
{
  const { left } = await play("Audio.tone(440, 30)\n", 4410);
  check(left.some((v) => v !== 0), "a tone is silent");
}

// A mixer made as the page opens and held still until the first gesture
// (which plays a tone) sounds the tone at once once it renders, not as long
// after as it was held.
{
  const left = await playOnHeldMixer("Audio.tone(440, 30)\n", 4410, 3000);
  const first = left.findIndex((v) => v !== 0);
  check(first >= 0 && first < 441, `a tone on a mixer held 3 s sounds from sample ${first}, not at once`);
}

// A Sound is decoded for a song: the mixer's samples of it are kept.
{
  const wav =
    "524946462800000057415645666d74201000000001000100401f0000401f0000010008006461746104000000" + "80c8803c";
  await play(`let s = Audio.Sound.new(Encoding.hex.decode("${wav}"))\n`, 128);
  const pcm = Object.values(self.__soundPcm);
  check(pcm.length === 1 && pcm[0].length > 0, "a Sound was not decoded for the mixer");
}

if (bad) process.exit(1);
console.log("mixer OK (a graph played twice, a sound played to its end, a change on a mixer up late, a tone, a tone on a mixer held, a Sound decoded)");
