// Run the committed Playground wasm and hold it to the native executor.
//
// The artifacts under site/playground/ are what GitHub Pages serves, and no
// runner has emsdk — so this checks the build that shipped rather than one it
// makes. All it needs is node and a culebra binary.
//
// Every case runs TWICE in one wasm instance, because that is what the page
// does: worker.js instantiates once and reuses it for every Run click, so
// anything the engine leaves in the Runtime outlives the run that made it.
// A single run is the shape a smoke test falls into naturally, and the one
// shape that cannot see it (docs/internals/vm.md §13.6).
//
//   node tools/playground/smoke.mjs <culebra-binary> [basic|full]

import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";

const [bin, variant = "basic"] = process.argv.slice(2);
if (!bin) {
  console.error("usage: smoke.mjs <culebra-binary> [basic|full]");
  process.exit(2);
}

const root = path.resolve(path.dirname(new URL(import.meta.url).pathname),
                          "../..");
const caseDir = path.join(root, "tools/playground/cases");
const cases = fs.readdirSync(caseDir).filter((f) => f.endsWith(".cul")).sort();
if (cases.length === 0) {
  console.error("smoke: no cases in tools/playground/cases");
  process.exit(2);
}

// The page's own scope: the glue posts output as messages and asks for a
// worker-ish global. Nothing here fetches — the wasm arrives as bytes.
let captured = "";
globalThis.self = globalThis;
globalThis.location = { href: "file:///" };
globalThis.postMessage = (m) => {
  if (m && m.type === "output") captured += m.text;
};

const jsPath = path.join(root, `site/playground/culebra-${variant}.js`);
const wasmBinary = fs.readFileSync(jsPath.replace(/\.js$/, ".wasm"));
// node reads a .js as CommonJS and the glue is an ES module; the copy is the
// cheapest way to say so without a package.json inside the served directory.
const glue = path.join(fs.mkdtempSync(path.join(os.tmpdir(), "culebra-pg-")),
                       "glue.mjs");
fs.copyFileSync(jsPath, glue);

const Module = await (await import(glue)).default({
  instantiateWasm(imports, done) {
    WebAssembly.instantiate(wasmBinary, imports).then((r) => done(r.instance));
    return {};
  },
});
Module.FS.mkdir("/work");

// stdout and stderr reach the page through one channel, so the native side is
// merged too. The cases keep the two apart — a case either prints or throws —
// so the merge cannot reorder anything.
function native(file) {
  const r = spawnSync(bin, ["--vm", file], { encoding: "utf8" });
  return { out: r.stdout + r.stderr, failed: r.status !== 0 };
}

async function wasm(name, src) {
  const p = `/work/${name}`;
  Module.FS.writeFile(p, src);
  captured = "";
  // run_culebra returns 0/1, not a process status — compare the verdict, not
  // the number (an uncaught throw exits 255 natively).
  const rc = await Module.ccall("run_culebra", "number",
                                ["string", "string", "string"],
                                [src, p, ""], { async: true });
  return { out: captured, failed: rc !== 0 };
}

let bad = 0;
for (const name of cases) {
  const file = path.join(caseDir, name);
  const src = fs.readFileSync(file, "utf8");
  const want = native(file);
  for (let run = 1; run <= 2; run++) {
    let got;
    try {
      got = await wasm(name, src);
    } catch (e) {
      console.error(`FAIL ${name} run ${run}: wasm trapped: ${e?.message}`);
      bad++;
      continue;
    }
    if (got.out !== want.out || got.failed !== want.failed) {
      console.error(`FAIL ${name} run ${run}`);
      console.error(`  native --vm (failed=${want.failed}):\n${want.out}`);
      console.error(`  wasm ${variant} (failed=${got.failed}):\n${got.out}`);
      bad++;
    }
  }
}

if (bad > 0) {
  console.error(`playground smoke: ${bad} mismatch(es)`);
  process.exit(1);
}
console.log(
  `playground smoke OK (${variant}: ${cases.length} cases, twice each)`);
