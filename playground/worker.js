// Playground worker: owns the WASM instance so the page never blocks.
// Stop = the main thread terminates this worker and spawns a fresh one.
import createCulebra from "./culebra.js";

const mod = await createCulebra();
postMessage({ type: "ready" });

onmessage = (e) => {
  const { type, src } = e.data;
  if (type !== "run") return;
  const t0 = performance.now();
  let rc = 1;
  try {
    rc = mod.ccall("run_culebra", "number", ["string"], [src]);
  } catch (err) {
    postMessage({ type: "result", rc: 1, out: "internal error: " + err, ms: performance.now() - t0 });
    return;
  }
  const out = mod.UTF8ToString(mod._get_output());
  postMessage({ type: "result", rc, out, ms: performance.now() - t0 });
};
