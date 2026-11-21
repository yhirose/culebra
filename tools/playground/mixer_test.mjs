// The Playground's mixer (site/playground/audio-engine.wasm, run by
// audio-worklet.js on the page's audio thread) fed what the committed
// Playground wasm sends it, as the page wires them: a graph played is the
// graph the same program renders, sample for sample; a tone sounds; a Sound
// decodes for a song; and a second run in the same instance plays as the
// first did (what a run leaves playing is let go).
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
// reset that lets go of what it left playing).
async function play(src, frames) {
  const sent = [];
  self.__audioPort = { postMessage: (m) => sent.push(m) };
  self.__audioStatus = null;
  self.__soundPcm = {};
  const { out, failed } = await playground.run("/work/mixer.cul", src);
  if (failed) throw new Error("the program failed:\n" + out);
  const mixer = new Processor({ processorOptions: { module } });
  const worker = { postMessage() {}, close() {}, onmessage: null };
  mixer.port.onmessage({ data: { port: worker } });
  const calls = sent.slice(0, -1);
  for (const m of calls) worker.onmessage({ data: m });
  const left = new Float32Array(frames), right = new Float32Array(frames);
  const block = [new Float32Array(128), new Float32Array(128)];
  for (let at = 0; at < frames; at += 128) {
    mixer.process([], [block]);
    left.set(block[0].subarray(0, Math.min(128, frames - at)), at);
    right.set(block[1].subarray(0, Math.min(128, frames - at)), at);
  }
  return { out, left, right, calls };
}

let bad = 0;
function check(ok, what) {
  if (!ok) {
    console.error("mixer FAIL: " + what);
    bad++;
  }
}

// A graph played is the graph rendered, twice over in one instance.
const graph = `let {saw, line, render, play} = Synth
let sound = saw(220.0).lowpass(line(400.0, 2000.0, 0.5)) * 0.3
println(JSON.stringify(render(sound, 0.5)))
play(sound, wait: false)
`;
for (const time of ["first", "second"]) {
  const { out, left, right } = await play(graph, 22050);
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

// A tone sounds, and is gone at the next run.
{
  const { left } = await play("Audio.tone(440, 30)\n", 4410);
  check(left.some((v) => v !== 0), "a tone is silent");
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
console.log("mixer OK (a graph played twice, a sound played to its end, a tone, a Sound decoded)");
