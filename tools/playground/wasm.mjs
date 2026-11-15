// The committed Playground wasm, loaded in node the way worker.js loads it in
// the page: one instance, run again for each program. Shared by the tests in
// this directory, which hold what ships rather than a fresh build (no runner
// has emsdk).

import fs from "node:fs";
import os from "node:os";
import path from "node:path";

export const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), "../..");

// The page's own scope: the glue posts output as messages and asks for a
// worker-ish global. Nothing here fetches — the wasm arrives as bytes.
export async function loadPlayground(variant = "basic") {
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

  // `file` is where the program is, as worker.js stages it (under /work).
  async function run(file, src) {
    Module.FS.mkdirTree(path.dirname(file));
    Module.FS.writeFile(file, src);
    captured = "";
    // What worker.js sets before each run for audio.h's handles to write
    // their playing flags into.
    self.__musicPlaying = {};
    self.__soundsPlaying = {};
    // run_culebra's result is a verdict, not a process status — compare it as
    // one.
    const rc = await Module.ccall("run_culebra", "number",
                                  ["string", "string", "string"],
                                  [src, file, ""], { async: true });
    return { out: captured, failed: rc !== 0 };
  }

  return { FS: Module.FS, run };
}
