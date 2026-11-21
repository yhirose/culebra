// The browser's mixer: the audio engine (audio-engine.wasm, built from
// playground/audio_engine_wasm.cc) on the page's audio thread, as a native
// build's mixer runs on its device's. The page compiles the module and hands
// it over (an AudioWorkletGlobalScope cannot fetch); the worker running the
// program sends each tone, schedule and graph call over the port the page
// connects (stdlib/audio_web.h), and is told back where the stream is.
// The context runs at the mixer's rate, so what plays here is the samples a
// native mixer plays.

import { engineImports } from "./audio-engine.js";

// Blocks of 128 frames between two words to the worker of where the stream
// is: some 23 ms.
const STATUS_EVERY = 8;

class CulebraEngine extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.wasm = new WebAssembly.Instance(options.processorOptions.module, engineImports).exports;
    this.wasm._initialize();
    this.worker = null;
    this.blocks = 0;
    this.lr = null;  // the rendered frames' view, made again when memory grows
    // A worker's port: its calls from now on, after a reset of what the last
    // one left playing (a worker resets the mixer itself, in line with its
    // calls, before and after each run: audio_web.h's reset_playing).
    this.port.onmessage = (e) => {
      if (this.worker) this.worker.close();
      this.wasm.engine_reset();
      this.worker = e.data.port;
      this.worker.onmessage = (w) => this.apply(w.data);
      this.status();
    };
  }

  // One call: its numbers, and a schedule's samples beside them. Each buffer
  // is asked for before memory.buffer is read, as asking may grow it.
  apply({ numbers, pcm }) {
    const w = this.wasm;
    const m = w.engine_numbers(numbers.length);
    new Float64Array(w.memory.buffer, m, numbers.length).set(numbers);
    if (pcm) {
      const p = w.engine_pcm(pcm.length);
      new Float32Array(w.memory.buffer, p, pcm.length).set(pcm);
    }
    w.engine_apply(pcm ? 1 : 0, Date.now());
  }

  status() {
    if (!this.worker) return;
    const w = this.wasm;
    const count = w.engine_status();
    const at = w.engine_status_at();
    // Dated on the wall clock, which the worker reads too (audio_web.h).
    this.worker.postMessage({ status: new Float64Array(w.memory.buffer, at, count).slice(), at: Date.now() });
  }

  process(_inputs, outputs) {
    const out = outputs[0];
    const frames = out[0].length;
    const at = this.wasm.engine_render(frames);
    const buffer = this.wasm.memory.buffer;
    if (!this.lr || this.lr.buffer !== buffer || this.lr.byteOffset !== at || this.lr.length !== 2 * frames)
      this.lr = new Float32Array(buffer, at, 2 * frames);
    const lr = this.lr;
    const left = out[0], right = out[1] || out[0];
    for (let i = 0; i < frames; i++) {
      left[i] = lr[2 * i];
      right[i] = lr[2 * i + 1];
    }
    if (++this.blocks % STATUS_EVERY === 0) this.status();
    return true;
  }
}

registerProcessor("culebra-engine", CulebraEngine);
