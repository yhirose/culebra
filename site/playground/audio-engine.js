// What the page's mixer (audio-worklet.js) and the worker's decoder
// (worker.js) share of audio-engine.wasm (audio_engine_wasm.cc): what the
// module asks of its host, and a Sound's bytes as the mixer's samples.

// The module asks only for these; its prints (raudio's warnings) go nowhere.
export const engineImports = {
  env: { emscripten_notify_memory_growth: () => {} },
  wasi_snapshot_preview1: { fd_write: () => 0, fd_close: () => 0, fd_seek: () => 0 },
};

// A decoder over an instance's exports: a file's bytes, `fmt` its format
// (".wav", ".ogg", ".mp3"), as the mixer's samples; null when they do not
// decode.
export function mixerDecoder(w) {
  w._initialize();
  const encoder = new TextEncoder();
  return (bytes, fmt) => {
    const name = encoder.encode(fmt + "\0");
    const data = w.engine_alloc(bytes.length), at = w.engine_alloc(name.length), n = w.engine_alloc(4);
    new Uint8Array(w.memory.buffer, data, bytes.length).set(bytes);
    new Uint8Array(w.memory.buffer, at, name.length).set(name);
    const pcm = w.decode_for_mixer(data, bytes.length, at, n);
    const count = new Int32Array(w.memory.buffer, n, 1)[0];
    const samples = pcm ? new Float32Array(w.memory.buffer, pcm, count).slice() : null;
    for (const p of [data, at, n]) w.engine_free(p);
    return samples;
  };
}
