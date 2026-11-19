// Synth Studio's program: the source the page hands its runner for one Play,
// and the way back from the line numbers of that program to the editor's.
//
// The editor's text (or the part of it selected) runs inside a function, so
// that its last expression is a value whatever comes before it; a tail then
// renders that value as a WAV. In the browser the tail writes it as a file,
// which the worker hands the page when the run ends (worker.js's `collect`);
// the desktop app (examples/synth-studio) runs the program with
// Isolate.spawn_source, which has no file to write beside, so there the WAV's
// bytes are the program's value. The page plays the WAV itself, so the
// sound outlives the run and Stop is the page's to do.
//
// No DOM and no imports, so node can load this for tools/playground/'s test,
// which runs what it builds on the wasm.

// Repo-relative, as every path the worker is given is (it lands under /work).
// The WAV is written beside the program, which is how the tail finds it.
const DIR = "studio";
const WAV = "out.wav";
export const ENTRY = `${DIR}/main.cul`;
export const OUT = `${DIR}/${WAV}`;

const HEAD = "let __studio_value = (fn () {";

// Lines HEAD puts before the editor's first. Locations in the program are this
// much further down than in the editor.
const LINE_OFFSET = 1;

// A value that ends on its own plays to its end; one that does not (an
// oscillator, a loop) plays for `seconds`. An Array is samples already, as
// Audio's own functions answer them: one channel, or `[left, right]`.
// `out` says how the WAV leaves: "file" or "value" (above).
function tail(seconds, out) {
  const wav = `__studio_wav(__studio_value, ${Number(seconds).toFixed(3)})`;
  const last = out === "value" ? wav : `FS.write(FS.join(FS.dirname(Sys.script), "${WAV}"), ${wav})`;
  return `})()
let __studio_wav = fn (value, seconds) {
  let sound = if value is Array {
    Audio.Sound(value)
  } else if value is SynthSignal | SynthEnvelope | SynthVoices {
    let sides = try {
      Synth.render(value, channels: 2)
    } catch ValueError {
      Synth.render(value, seconds, channels: 2)
    }
    Audio.Sound(sides)
  } else {
    throw "Studio plays the value of the last expression, a Synth part or an Array of samples; this one is {type_of(value)}"
  }
  sound.data()
}
${last}
`;
}

// `selection` is `{ from, to }` in the editor's text, or null for all of it.
// A selection runs after the whole lines above it, so it can use what they
// define; the part of its first line before it is blanked rather than dropped,
// which keeps every location in it where it is in the editor. `lines` is how
// many of the editor's lines the program carries.
export function buildProgram(source, selection, seconds, out = "file") {
  let body = source;
  if (selection && selection.to > selection.from) {
    const lineStart = source.slice(0, selection.from).lastIndexOf("\n") + 1;
    // A column counts UTF-8 bytes, so the blank is as many spaces.
    const lead = " ".repeat(new TextEncoder().encode(source.slice(lineStart, selection.from)).length);
    body = source.slice(0, lineStart) + lead + source.slice(selection.from, selection.to);
  }
  return { src: `${HEAD}\n${body}\n${tail(seconds, out)}`, lines: body.split("\n").length };
}

// What a run printed, for the editor: when it failed, the error's location
// in the program put back as the editor's, and `line` that line (null when
// there is none). Only an error's own forms are read — a message's closing
// ` at LINE:COL.` and a syntax error's `<entry>:LINE:COL` — so what the
// program printed stays as it was. A location past the editor's lines is in
// the head or the tail: it says nothing about the code, so it is left out.
const LOCATION = new RegExp(
  String.raw`( at )(\d+):(\d+)(?=\.$)|(${ENTRY.replace(/[.]/g, "\\.")}:)(\d+):(\d+)`, "gm");

export function editorReport(text, failed, lines) {
  if (!failed) return { text, line: null };
  let line = null;
  const out = text.replace(LOCATION, (whole, at, al, ac, path, pl, pc) => {
    const lead = at ?? path;
    const n = Number(al ?? pl) - LINE_OFFSET;
    if (n < 1 || n > lines) return at ? "" : whole;
    if (line === null) line = n;
    return `${lead}${n}:${ac ?? pc}`;
  });
  return { text: out, line };
}

// --- the harmonics of what a Play rendered ---------------------------------
//
// A second program, run once the sound plays, reads the WAV the Play left and
// answers its pitch and the levels of its harmonics every FRAME_SECONDS:
// `{hz, harmonics}`, `hz[t]` the pitch in Hz (nil where there is none) and
// `harmonics[t]` HARMONICS levels in dBFS, DB_FLOOR at the least (nil where
// `hz[t]` is). The channels are heard as one, their mean. In the browser it
// reads the WAV beside it and writes its answer as JSON beside that; in the
// desktop app the WAV is its argument and its answer is its value, so it names
// no namespace the app is not built with (examples/synth-studio/main.cul).
const ANALYSIS = "analysis.json";
export const ANALYSIS_ENTRY = `${DIR}/analysis.cul`;
export const ANALYSIS_OUT = `${DIR}/${ANALYSIS}`;
export const FRAME_SECONDS = 0.01;
export const HARMONICS = 40;
export const DB_FLOOR = -80;
// The pitches looked for: a bass's lowest note to a soprano's highest.
const PITCH_LOW = 40;
const PITCH_HIGH = 2000;

export function buildAnalysis(out = "file") {
  const wav = out === "value" ? "Sys.argv[0]" : `FS.read(FS.join(FS.dirname(Sys.script), "${WAV}"))`;
  const analysis = `(fn (wav) {
  # The 16-bit WAV that Audio.Sound's data() writes: its samples from byte 44.
  __raise("ValueError", "Studio's analysis reads a 16-bit WAV") if wav.slice(36, 40) != "data"
  let channels = String.unpack("u16", wav, at: 22, count: 1)[0]
  let rate = String.unpack("u32", wav, at: 24, count: 1)[0]
  let pcm = String.unpack("i16", wav, at: 44)
  let mono = channels == 1
    ? pcm.map(|x| x / 32768.0)
    : iota(pcm.size() / 2).map(|i| (pcm[2 * i] + pcm[2 * i + 1]) / 65536.0)
  let pitch = Audio.pitch(mono, ${PITCH_LOW}.0, ${PITCH_HIGH}.0, rate)
  let found = Audio.harmonics(mono, pitch.hz, count: ${HARMONICS}, rate: rate)
  let floor = 10.0 ** (${DB_FLOOR}.0 / 20.0)
  let db = |a| a <= floor ? ${DB_FLOOR}.0 : Math.round(200.0 * Math.log(a) / Math.log(10.0)) / 10.0
  {
    hz: pitch.hz.map(|f| f == nil ? nil : Math.round(f * 100.0) / 100.0),
    harmonics: found.harmonics.map(|h| h == nil ? nil : h.map(db)),
  }
})(${wav})`;
  return out === "value"
    ? `${analysis}\n`
    : `FS.write(FS.join(FS.dirname(Sys.script), "${ANALYSIS}"), JSON.stringify(${analysis}))\n`;
}
