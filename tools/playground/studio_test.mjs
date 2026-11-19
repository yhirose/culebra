// Hold Synth Studio's program (playground/studio-program.js) to the committed
// wasm: what a Play runs, the WAV it leaves for the page, and the way an
// error's location comes back to the editor. The WAV is compared with the one
// the native executor writes from the same program, byte for byte.
//
//   node tools/playground/studio_test.mjs <culebra-binary>

import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { root, loadPlayground } from "./wasm.mjs";
import {
  ENTRY, OUT, ANALYSIS_ENTRY, ANALYSIS_OUT, HARMONICS, DB_FLOOR,
  buildProgram, buildAnalysis, editorReport,
} from "../../playground/studio-program.js";

const [bin] = process.argv.slice(2);
if (!bin) {
  console.error("usage: studio_test.mjs <culebra-binary>");
  process.exit(2);
}

const playground = await loadPlayground();

// Runs `src` on the wasm as `entry`: what it printed, whether it failed, and
// the file it left at `out` (null when it wrote none).
async function runWasm(entry, out, src) {
  try { playground.FS.unlink(`/work/${out}`); } catch { /* not there */ }
  const { out: text, failed } = await playground.run(`/work/${entry}`, src);
  let bytes = null;
  try { bytes = Buffer.from(playground.FS.readFile(`/work/${out}`)); } catch { /* not written */ }
  return { text, failed, bytes };
}

// What a Play leaves behind: the run's verdict, its output with locations put
// back as the editor's, and the WAV (null when the run wrote none).
async function play(source, selection = null, seconds = 1) {
  const { src, lines } = buildProgram(source, selection, seconds);
  const { text, failed, bytes } = await runWasm(ENTRY, OUT, src);
  return { src, failed, wav: bytes, ...editorReport(text, failed, lines) };
}

// Runs `main` on the native executor in a fresh directory, beside `files`
// (name to text), whose paths are its arguments.
function runNative(main, files = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "culebra-studio-"));
  for (const [name, text] of Object.entries(files)) fs.writeFileSync(path.join(dir, name), text);
  fs.writeFileSync(path.join(dir, "main.cul"), main);
  const r = spawnSync(bin, ["--vm", path.join(dir, "main.cul"),
                            ...Object.keys(files).map((f) => path.join(dir, f))],
                      // A runner that never answers fails the test rather than hang it.
                      { encoding: "utf8", timeout: 60_000,
                        env: { ...process.env, CULEBRA_AUDIO: "off" } });
  assert.equal(r.status, 0, r.stdout + r.stderr);
  return { dir, stdout: r.stdout };
}

function native(src) {
  return fs.readFileSync(path.join(runNative(src).dir, "out.wav"));
}

// [channels, frames] from a 16-bit WAV's fmt and data chunks.
function shape(wav) {
  const channels = wav.readUInt16LE(22);
  const data = wav.indexOf("data");
  return [channels, wav.readUInt32LE(data + 4) / (channels * 2)];
}

// The loudest sample of a 16-bit WAV, from 0 to 1.
function peak(wav) {
  const data = wav.indexOf("data") + 8;
  let m = 0;
  for (let i = data; i + 1 < wav.length; i += 2) m = Math.max(m, Math.abs(wav.readInt16LE(i)));
  return m / 32768;
}

const select = (source, text, nth = 0) => {
  let from = -1;
  for (let i = 0; i <= nth; i++) from = source.indexOf(text, from + 1);
  assert.ok(from >= 0, `no ${text} in the source`);
  return { from, to: from + text.length };
};

// --- a sound that ends on its own plays to its end -------------------------

const pluck = "let {noise, perc} = Synth\nnoise() * perc(0.001, 0.01) |> pluck(220.0, ring: 0.5)\n";
const plucked = await play(pluck, null, 4);
assert.equal(plucked.failed, false, plucked.text);
assert.deepEqual(shape(plucked.wav), [2, 22535]);
assert.ok(plucked.wav.equals(native(plucked.src)), "the wasm's WAV is not the native one");

// --- one that does not plays for the seconds asked -------------------------

const saw = await play("Synth.saw(110.0)", null, 0.25);
assert.equal(saw.failed, false, saw.text);
assert.deepEqual(shape(saw.wav), [2, 11025]);
assert.ok(saw.wav.equals(native(saw.src)), "the wasm's WAV is not the native one");

// --- a selection runs after the lines above it -----------------------------

const pad = "let {saw} = Synth\nlet pad = saw(110.0) * 0.2\npad.lowpass(800.0)\n";
{
  // Part of a line, without what the line goes on to do with it.
  const r = await play(pad, select(pad, "saw(110.0)"), 0.25);
  assert.equal(r.failed, false, r.text);
  assert.ok(r.wav.equals(saw.wav), "the selection did not play saw(110.0) alone");
  // A name the lines above it define.
  const alone = await play("Synth.saw(110.0) * 0.2", null, 0.25);
  const named = await play(pad, select(pad, "pad", 1), 0.25);
  assert.equal(named.failed, false, named.text);
  assert.ok(named.wav.equals(alone.wav), "the selection did not play `pad`");
}

// --- an error points at the editor's line ----------------------------------

const broken = "let {saw} = Synth\n\nlet y = Math.abss(1)\nsaw(110.0)\n";
{
  const r = await play(broken);
  assert.equal(r.failed, true);
  assert.equal(r.wav, null);
  assert.equal(r.line, 3, r.text);
  const at = r.text.match(/ at (3:\d+)/)?.[1];
  assert.ok(at, r.text);
  // A selection from the middle of that line reports the same line and column.
  const sel = await play(broken, select(broken, "Math.abss(1)\nsaw"));
  assert.equal(sel.line, 3, sel.text);
  assert.equal(sel.text.match(/ at (\d+:\d+)/)?.[1], at, sel.text);
}
{
  const r = await play("let {saw} = Synth\nsaw(110.0))\n");
  assert.equal(r.failed, true);
  assert.equal(r.line, 2, r.text);
}
{
  // All of a text that starts with an empty line.
  const src = "\nlet y = Math.abss(1)\nSynth.saw(110.0)\n";
  const r = await play(src, { from: 0, to: src.length });
  assert.equal(r.line, 2, r.text);
}
{
  // A column counts bytes: what the selection leaves out of its first line
  // has to take as many as it did.
  const src = 'let y = ["ああ", Math.abss(1)]\nSynth.saw(110.0)\n';
  const whole = await play(src);
  const sel = await play(src, select(src, "Math.abss(1)"));
  assert.equal(sel.text.match(/ at (\d+:\d+)/)?.[1], whole.text.match(/ at (\d+:\d+)/)?.[1], sel.text);
}
{
  // What a program prints is its own, even where it reads like a location.
  const said = "meet at 12:30\nx.cul:2:1\n";
  // A run that worked has no error, however its output reads.
  const ok = await play('println("meet at 12:30")\nprintln("ends at 2:1.")\nSynth.saw(110.0)\n', null, 0.1);
  assert.equal(ok.text, "meet at 12:30\nends at 2:1.\n");
  assert.equal(ok.line, null);
  const failed = await play('println("meet at 12:30")\nprintln("x.cul:2:1")\nMath.abss(1)\n');
  assert.ok(failed.text.startsWith(said), failed.text);
  assert.equal(failed.line, 3, failed.text);
}

// --- a value that is not a sound says so, and where is not the editor's ---

{
  const r = await play("let x = 3\nx\n");
  assert.equal(r.failed, true);
  assert.match(r.text, /Studio plays the value of the last expression.*this one is Long/);
  assert.equal(r.line, null, r.text);
  assert.doesNotMatch(r.text, / at \d+:\d+/);
}

// --- every sample the page offers plays, and none clips -----------------------

{
  const dir = path.join(root, "playground/studio-samples");
  const catalog = JSON.parse(fs.readFileSync(path.join(dir, "index.json"), "utf8"));
  assert.equal(catalog.filter((s) => s.opening).length, 1, "one sample opens the page");
  assert.deepEqual(fs.readdirSync(dir).filter((f) => f.endsWith(".cul")).sort(),
                   catalog.map((s) => s.file).sort());
  for (const { title, file } of catalog) {
    const r = await play(fs.readFileSync(path.join(dir, file), "utf8"), null, 0.5);
    assert.equal(r.failed, false, `${title}: ${r.text}`);
    assert.ok(peak(r.wav) < 0.99, `${title} clips`);
  }
}

// --- an Array is samples already --------------------------------------------

{
  const r = await play("[0.0, 0.5, -0.5]");
  assert.equal(r.failed, false, r.text);
  assert.deepEqual(shape(r.wav), [1, 3]);
}

// --- the harmonics of what a Play left ------------------------------------------
//
// A second run reads the WAV the Play left beside it. Its answer is held to the
// native executor's on the same WAV (the desktop app's, below) within what the
// rounding leaves: the pitch's last digits differ between the wasm's libm and
// the native one's.

async function analyse(source, seconds) {
  const p = await play(source, null, seconds);
  assert.equal(p.failed, false, p.text);
  const { text, failed, bytes } = await runWasm(ANALYSIS_ENTRY, ANALYSIS_OUT, buildAnalysis());
  assert.equal(failed, false, text);
  return { wav: p.wav, ...JSON.parse(bytes.toString()) };
}

// The same frames pitched, the pitches to 0.01 Hz and the levels to 0.1 dB.
function assertClose(a, b, what) {
  assert.equal(a.hz.length, b.hz.length, what);
  a.hz.forEach((hz, t) => {
    assert.equal(hz === null, b.hz[t] === null, `${what}: frame ${t} pitched in one only`);
    if (hz === null) return assert.equal(a.harmonics[t], null, what);
    assert.ok(Math.abs(hz - b.hz[t]) <= 0.0101, `${what}: frame ${t} at ${hz} and ${b.hz[t]} Hz`);
    a.harmonics[t].forEach((db, k) => assert.ok(Math.abs(db - b.harmonics[t][k]) <= 0.101,
                                                 `${what}: frame ${t}, harmonic ${k + 1}`));
  });
}

const sawAnalysis = await analyse("Synth.saw(220.0) * 0.5", 0.5);
{
  const a = sawAnalysis;
  // A frame every 10 ms.
  assert.equal(a.hz.length, 50);
  const mid = 25;
  assert.ok(Math.abs(a.hz[mid] - 220) < 0.5, `pitch ${a.hz[mid]}`);
  const h = a.harmonics[mid];
  assert.equal(h.length, HARMONICS);
  // A saw's k-th harmonic is 1/k of its first: 6.0 and 9.5 dB under it.
  assert.ok(Math.abs(h[1] - h[0] + 6.02) < 0.3, `harmonic 2 at ${h[1] - h[0]} dB`);
  assert.ok(Math.abs(h[2] - h[0] + 9.54) < 0.3, `harmonic 3 at ${h[2] - h[0]} dB`);
}
{
  // Harmonics past 0.45 of the rate read nothing, which is DB_FLOOR rather
  // than -inf (which JSON cannot carry).
  const a = await analyse("Synth.saw(1500.0) * 0.5", 0.1);
  assert.equal(a.harmonics[5][39], DB_FLOOR);
}
{
  // Noise has no pitch, nor harmonics.
  const a = await analyse("Synth.noise() * 0.3", 0.1);
  assert.ok(a.hz.every((hz) => hz === null), String(a.hz));
  assert.ok(a.harmonics.every((h) => h === null));
}
{
  // An Array's one channel is read as it is.
  const a = await analyse("iota(4410).map(|i| 0.5 * Math.sin(i * 2.0 * Math.pi * 330.0 / 44100.0))", 1);
  assert.deepEqual(shape(a.wav), [1, 4410]);
  assert.ok(Math.abs(a.hz[5] - 330) < 0.5, `pitch ${a.hz[5]}`);
}

// --- the desktop app plays the same WAV ---------------------------------------
//
// examples/synth-studio runs a Play with Isolate.spawn_source through its
// runner, the WAV the program's value rather than a file beside it. What the
// page gets is held to the browser's: the same WAV, and an error at the same
// line of the editor; and the analysis of a WAV, the browser's.

// The runner's answer to `src` run as `entry`, given `wav` when there is one.
function runner(src, entry, wav = null) {
  const lib = path.join(root, "examples/synth-studio/runner/runner.cul");
  const { stdout } = runNative(`import runner from '${lib}'
let run = runner.start(FS.read(Sys.argv[0]), "${entry}", ${wav ? "FS.read(Sys.argv[1])" : "nil"})
mut done = runner.answer(run)
while done == nil {
  Time.sleep(0.01)
  done = runner.answer(run)
}
print(JSON.stringify(done[0]))
`, wav ? { "program.cul": src, "out.wav": wav } : { "program.cul": src });
  return JSON.parse(stdout);
}

function desktop(source, selection = null, seconds = 1) {
  const { src, lines } = buildProgram(source, selection, seconds, "value");
  const { rc, wav, text = "" } = runner(src, ENTRY);
  return { failed: rc !== 0, wav: wav ? Buffer.from(wav, "base64") : null,
           ...editorReport(text, rc !== 0, lines) };
}

{
  const r = desktop(pluck, null, 4);
  assert.equal(r.failed, false, r.text);
  assert.ok(r.wav.equals(plucked.wav), "the desktop app's WAV is not the browser's");
  const sel = desktop(pad, select(pad, "saw(110.0)"), 0.25);
  assert.ok(sel.wav.equals(saw.wav), "the desktop app's selection did not play saw(110.0) alone");
  const failed = desktop(broken, select(broken, "Math.abss(1)\nsaw"));
  assert.equal(failed.wav, null);
  assert.equal(failed.line, 3, failed.text);
  assert.equal(desktop("let {saw} = Synth\nsaw(110.0))\n").line, 2);
  const notSound = desktop("let x = 3\nx\n");
  assert.match(notSound.text, /Studio plays the value of the last expression.*this one is Long/);
  assert.equal(notSound.line, null, notSound.text);
  const analysed = runner(buildAnalysis("value"), ANALYSIS_ENTRY, sawAnalysis.wav);
  assert.equal(analysed.rc, 0, analysed.text);
  assertClose(analysed.value, sawAnalysis, "the desktop app's analysis");
}

// The app is built with the `--with` its main.cul names: a namespace the
// programs name around the editor's code beyond those is not in the binary,
// and every Play (or analysis) there fails. Only a built binary shows it,
// which this test does not build, so the programs are read for the names.
{
  const main = fs.readFileSync(path.join(root, "examples/synth-studio/main.cul"), "utf8");
  const linked = main.match(/--with ([A-Za-z,]+)/)[1].split(",");
  const named = (src) => [...new Set([...src.matchAll(/\b([A-Z][A-Za-z]*)\./g)].map((m) => m[1]))];
  for (const src of [buildProgram("", null, 1, "value").src, buildAnalysis("value")]) {
    const missing = named(src).filter((ns) => !linked.includes(ns));
    assert.deepEqual(missing, [], `main.cul's --with leaves out ${missing}`);
  }
}

console.log("playground studio OK");
