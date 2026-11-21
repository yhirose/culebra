// MIDI for a page that runs programs in the Playground's worker (app.js,
// fm-synth.html): the browser's Web MIDI, for the worker (web-midi.js,
// stdlib/audio_web.h). Asked for when a program first asks for a port, which
// waits for the answer (the first time the browser asks the user), and again
// for sysexes; a refusal leaves what was granted. From then on the worker is
// sent the ports as they change and every message of every input, dated on
// the wall clock; it keeps those of the inputs a program holds open. Web MIDI
// is the page's alone (a worker has none). A browser without it (Safari) has
// no ports.

// `post` sends to the current worker (none: dropped). Answers the handler of
// the worker's messages, true for one of MIDI's.
export function hostMidi(post) {
  let access = null;

  function listen() {
    for (const input of access.inputs.values()) {
      input.onmidimessage = (e) => {
        post({ type: "midi", id: input.id, data: e.data, at: performance.timeOrigin + e.timeStamp });
      };
    }
  }

  function sendPorts() {
    const list = (ports) =>
      [...ports.values()].filter((p) => p.state === "connected").map((p) => ({ id: p.id, name: p.name }));
    post({ type: "midiPorts", inputs: list(access.inputs), outputs: list(access.outputs) });
  }

  async function answer(sysex) {
    try {
      if (navigator.requestMIDIAccess && (!access || (sysex && !access.sysexEnabled))) {
        const next = await navigator.requestMIDIAccess({ sysex });
        // An access has ports of its own: the old one's stop, or each message
        // would come twice.
        if (access) {
          access.onstatechange = null;
          for (const input of access.inputs.values()) input.onmidimessage = null;
        }
        access = next;
        access.onstatechange = () => {
          listen();
          sendPorts();
        };
        listen();
      }
    } catch {
      // Refused, or no MIDI here: what was granted before stays.
    }
    if (access) sendPorts();
    post({ type: "midiAccess", ok: access !== null, sysex: access?.sysexEnabled === true });
  }

  return (msg) => {
    if (msg.type === "midiAccess") {
      answer(msg.sysex);
      return true;
    }
    if (msg.type === "midiSend") {
      const out = access && access.outputs.get(msg.id);
      try {
        if (out) out.send(msg.bytes);
      } catch {
        // Unplugged since: what is sent to a device gone is lost, as natively.
      }
      return true;
    }
    return false;
  };
}
