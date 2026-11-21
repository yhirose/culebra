// The worker's half of MIDI: what stdlib/audio_web.h reads (self.__midi,
// __midiAccess, __midiWait) and what the page's half (app.js, the browser's
// Web MIDI) sends it. audio_web.h asks for MIDI when a program first asks for
// a port: the page asks the browser (the first time, the user), sends the
// ports, then answers whether MIDI is there and whether sysexes are. The page
// then sends the ports as they change and every message of every input; those
// of the ports a program holds open wait here until audio_web.h takes them,
// and one coming wakes a wait for it. A run that never reads keeps at most
// MIDI_WAITING; the rest is passed over, which dropped() does not count.

const MIDI_WAITING = 65536;

// Installs it on `scope` (the worker's self), `post` sending to the page;
// answers the handler of the page's messages, true for one of MIDI's.
export function workerMidi(scope, post) {
  const midi = { version: 0, inputs: [], outputs: [], held: {}, queue: [], wake: null };
  let answer = null;
  scope.__midi = midi;
  scope.__midiAccess = (sysex) =>
    new Promise((resolve) => {
      answer = resolve;
      post({ type: "midiAccess", sysex });
    });
  scope.__midiWait = (ms) =>
    new Promise((resolve) => {
      const timer = setTimeout(() => {
        midi.wake = null;
        resolve();
      }, ms);
      midi.wake = () => {
        clearTimeout(timer);
        midi.wake = null;
        resolve();
      };
    });
  return (data) => {
    switch (data.type) {
      case "midiAccess":
        if (answer) answer({ ok: data.ok, sysex: data.sysex });
        answer = null;
        return true;
      case "midiPorts":
        midi.inputs = data.inputs;
        midi.outputs = data.outputs;
        midi.version++;
        return true;
      case "midi":
        if (midi.held[data.id] > 0 && midi.queue.length < MIDI_WAITING) {
          midi.queue.push(data);
          if (midi.wake) midi.wake();
        }
        return true;
      default:
        return false;
    }
  };
}
