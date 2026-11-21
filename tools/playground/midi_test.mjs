// MIDI in the Playground: the committed JSPI build with the worker's half of
// MIDI (site/playground/web-midi.js) and a page that answers as app.js does,
// with an input and an output of its own. A program lists the ports, opens
// the input and waits for a note, which wakes it as soon as one comes; the
// messages it reads are what was played (a 14-bit controller and velocity
// too), and only its input's; messages come in while it sleeps; and what it
// sends reaches the page, checked first as natively, a sysex only with the
// browser's leave. A run lets go of what it held and was granted. Without
// JSPI there is no MIDI.
//
//   node tools/playground/midi_test.mjs

import path from "node:path";
import { root, loadPlayground } from "./wasm.mjs";

globalThis.self = globalThis;
const site = path.join(root, "site/playground");
const { workerMidi } = await import(path.join(site, "web-midi.js"));

let bad = 0;
function check(ok, what) {
  if (!ok) {
    console.error("midi FAIL: " + what);
    bad++;
  }
}

// The page: it answers with its ports (and never allows sysexes), plays when
// told, and keeps what the program sends.
const sent = [];
let asked = 0;
const onMidi = workerMidi(globalThis, (m) => {
  if (m.type !== "midiAccess") return;
  asked++;
  setTimeout(() => {
    onMidi({ type: "midiPorts", inputs: [{ id: "in-1", name: "EWI-USB" }, { id: "in-2", name: "Keys" }],
             outputs: [{ id: "out-1", name: "Synth" }] });
    onMidi({ type: "midiAccess", ok: true, sysex: false });
  }, 5);
});
const play = (id, ...bytes) =>
  onMidi({ type: "midi", id, data: new Uint8Array(bytes), at: performance.timeOrigin + performance.now() });

const playground = await loadPlayground("full");
const post = globalThis.postMessage;
globalThis.postMessage = (m) => (m && m.type === "midiSend" ? sent.push(m) : post(m));

// The page plays while the program waits: a breath at 14 bits on CC 2 and
// 34, a note on and, on the port the program does not hold, another; then,
// while it sleeps, a note off.
setTimeout(() => {
  play("in-1", 0xb0, 2, 0x40);
  play("in-1", 0xb0, 34, 0x10);
  play("in-1", 0x90, 60, 100);
  play("in-2", 0x90, 61, 100);
}, 300);
setTimeout(() => play("in-1", 0x80, 60, 0), 800);

const started = performance.now();
const { out, failed } = await playground.run("/work/midi.cul", `let ins = MIDI.inputs()
println(ins.map(|p| "{p.id} {p.name}").join(", "))
let ewi = MIDI.Input("in-1")
println(ewi.wait(5.0))
for m in ewi.read() {
  println("{m.kind} {m.get("note", m.get("cc", -1))} {Math.round(m.get("velocity", m.get("value", 0.0)) * 1000.0)}")
}
println(Math.round(ewi.control(2) * 16383.0))
Time.sleep(1.0)
println(ewi.read().map(|m| m.kind).join(", "))
let synth = MIDI.Output(MIDI.outputs()[0].id)
synth.send([0x90, 64, 90, 0xC0, 3])
try {
  synth.send([0x90, 64])
} catch e {
  println(e.message)
}
try {
  synth.send([0xF0, 0x7E, 0xF7])
} catch e {
  println(e.message)
}
`);
const took = performance.now() - started;
check(!failed, "the program failed:\n" + out);
// The wait (5 s at most) ends with the note, at 0.3 s, and the sleep is 1 s.
check(took < 3000, `the run took ${Math.round(took)} ms: the wait did not end when the note came`);
const want = [
  "in-1 EWI-USB, in-2 Keys",
  "true",
  "cc 2 504",
  "cc 34 126",
  "note_on 60 781",
  String((0x40 << 7) | 0x10),
  "note_off",
  "MIDI.Output.send: the message at byte 0 is cut short",
  "MIDI.Output.send: the browser did not allow sysexes",
].join("\n") + "\n";
check(out === want, `the program printed\n${out}instead of\n${want}`);
check(sent.length === 2 && sent[0].id === "out-1" && [...sent[0].bytes].join(",") === "144,64,90" &&
        [...sent[1].bytes].join(",") === "192,3",
      "the page was sent " + JSON.stringify(sent.map((m) => [m.id, [...m.bytes]])));

// A run lets go of the ports it held, and of what it was granted: the next
// run asks again (the page answers at once what it was granted).
check(Object.values(globalThis.__midi.held).every((n) => n === 0), "a port is still held after the run");
check(asked === 2, `MIDI was asked for ${asked} times in the run, not 2 (MIDI, then sysexes to send one)`);
const again = await playground.run("/work/midi.cul", "println(MIDI.inputs().size())\n");
check(!again.failed && again.out === "2\n" && asked === 3, `the next run printed ${JSON.stringify(again.out)} and asked ${asked - 2} times`);

// Without JSPI there is no MIDI, and nothing waits for the page.
const basic = await loadPlayground("basic");
const plain = await basic.run("/work/midi.cul", "println(MIDI.inputs().size())\n");
check(!plain.failed && plain.out === "0\n", "the build without JSPI printed " + JSON.stringify(plain.out));

if (bad) process.exit(1);
console.log("midi OK (ports, a wait woken, 14 bits, read while asleep, send, let go, no JSPI)");
