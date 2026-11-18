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
import { ENTRY, OUT, buildProgram, editorReport } from "../../playground/studio-program.js";

const [bin] = process.argv.slice(2);
if (!bin) {
  console.error("usage: studio_test.mjs <culebra-binary>");
  process.exit(2);
}

const playground = await loadPlayground();

// What a Play leaves behind: the run's verdict, its output with locations put
// back as the editor's, and the WAV (null when the run wrote none).
async function play(source, selection = null, seconds = 1) {
  const { src, lines } = buildProgram(source, selection, seconds);
  const out = `/work/${OUT}`;
  try { playground.FS.unlink(out); } catch { /* not there */ }
  const { out: text, failed } = await playground.run(`/work/${ENTRY}`, src);
  let wav = null;
  try { wav = Buffer.from(playground.FS.readFile(out)); } catch { /* not written */ }
  return { src, failed, wav, ...editorReport(text, failed, lines) };
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

// --- the desktop app plays the same WAV ---------------------------------------
//
// examples/synth-studio runs a Play with Isolate.spawn_source through its
// runner, the WAV the program's value rather than a file beside it. What the
// page gets is held to the browser's: the same WAV, and an error at the same
// line of the editor.

function desktop(source, selection = null, seconds = 1) {
  const { src, lines } = buildProgram(source, selection, seconds, "value");
  const runner = path.join(root, "examples/synth-studio/runner/runner.cul");
  const { stdout } = runNative(`import runner from '${runner}'
let run = runner.start(FS.read(Sys.argv[0]), "${ENTRY}")
mut reply = runner.answer(run)
while reply == nil {
  Time.sleep(0.01)
  reply = runner.answer(run)
}
print(JSON.stringify(reply))
`, { "program.cul": src });
  const { rc, wav, text = "" } = JSON.parse(stdout);
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
}

// The app is built `--with Synth,Audio` (examples/synth-studio/main.cul): a
// namespace the program adds around the editor's code beyond those is not in
// the binary, and every Play there fails. Only a built binary shows it, which
// this test does not build, so the program is read for the names instead.
{
  const { src } = buildProgram("", null, 1, "value");
  const named = new Set([...src.matchAll(/\b([A-Z][A-Za-z]*)\./g)].map((m) => m[1]));
  assert.deepEqual([...named].sort(), ["Audio", "Synth"]);
}

console.log("playground studio OK");
