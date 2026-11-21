// The page's audio, for a page that runs programs in the Playground's worker
// (app.js, fm-synth.html). One AudioContext at the mixer's rate
// (stdlib/audio_engine.h's kSampleRate), made as the page opens with the
// mixer in it, so the first sound waits on nothing but the context resuming.
// A program's tones, schedules and graphs play in it on the mixer, an
// AudioWorklet (audio-worklet.js) running the engine a native build mixes
// with (audio-engine.wasm), so they play the samples a native build plays. A
// worker sends the mixer its calls over a port of its own (channel()).

const MIXER_RATE = 44100;

export function hostAudio() {
  let ctx = null;
  let mixer = null;      // the AudioWorkletNode, once it is up
  let waiting = null;    // the current worker's port to it, until it is up

  function connect() {
    if (!mixer || !waiting) return;
    mixer.port.postMessage({ port: waiting }, [waiting]);
    waiting = null;
  }

  async function startMixer() {
    try {
      const [module] = await Promise.all([
        WebAssembly.compileStreaming(fetch("./audio-engine.wasm")),
        ctx.audioWorklet.addModule("./audio-worklet.js"),
      ]);
      mixer = new AudioWorkletNode(ctx, "culebra-engine", {
        numberOfInputs: 0,
        outputChannelCount: [2],
        processorOptions: { module },
      });
      mixer.connect(ctx.destination);
      connect();
    } catch {
      // No worklet here: tones, songs and graphs stay silent, as with no device.
    }
  }

  // Browsers keep a context made before a user gesture "suspended" (Chrome
  // says so in the console): the mixer comes up in it all the same and takes
  // a worker's calls, and once it renders its stream catches up the time it
  // was held (stdlib/audio_wire.h). With no audio here (no device, or
  // blocked) a program stays playable, silent.
  try {
    ctx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: MIXER_RATE });
    startMixer();
  } catch {
    ctx = null;
  }

  // Resumed if suspended, from the first click or key anywhere on the page
  // and from a sound played, so a program that makes noise right away is
  // not silent. Answers the context, or null with no audio here.
  function ensure() {
    // Refused (not yet a gesture), it stays suspended and the program plays on.
    if (ctx && ctx.state === "suspended") ctx.resume().catch(() => {});
    return ctx;
  }
  for (const ev of ["pointerdown", "keydown", "touchstart"]) {
    window.addEventListener(ev, ensure, { passive: true });
  }

  // A worker's way to the mixer, once the worker is ready (a message posted
  // before its handler is up is lost): one end of a channel goes to it, the
  // other to the mixer once that is up; what the worker sends before then
  // waits in the channel.
  function channel(worker) {
    const ch = new MessageChannel();
    worker.postMessage({ type: "audioPort", port: ch.port2 }, [ch.port2]);
    waiting = ch.port1;
    connect();
  }

  return { ensure, channel };
}
