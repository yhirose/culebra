// Playground worker: owns the WASM instance so the page never blocks.
// Stop = the main thread terminates this worker and spawns a fresh one.
//
// Two builds ship (see build.sh): culebra-gpu adds the WebGPU Tensor backend
// but is instantiable only where JSPI exists, so pick by feature detection —
// by capability, never by browser version (Chrome 137 shipping JSPI is exactly
// what broke version-sniffing detectors elsewhere).
const hasJSPI = typeof WebAssembly.Suspending === "function";

// The device is acquired here, in JS, and handed to wasm fully formed, which
// is what lets every C++ entry point stay synchronous — the async surface is
// this file, not the library. Without it tensorlib's WebGPU init dereferences
// an undefined device ("cannot read properties of undefined (reading
// 'queue')"), so a GPU build with no device must fall back to the CPU build.
async function acquireDevice() {
  if (!hasJSPI || !navigator.gpu) return null;
  try {
    const adapter = await navigator.gpu.requestAdapter();
    return adapter ? await adapter.requestDevice() : null;
  } catch {
    return null;
  }
}

const device = await acquireDevice();
const useGpuBuild = device !== null;

const { default: createCulebra } = await import(
  useGpuBuild ? "./culebra-gpu.js" : "./culebra-cpu.js"
);
const mod = await createCulebra(
  useGpuBuild ? { preinitializedWebGPUDevice: device } : {}
);

postMessage({ type: "ready", backend: useGpuBuild ? "gpu" : "cpu" });

onmessage = async (e) => {
  const { type, src } = e.data;
  if (type !== "run") return;
  const t0 = performance.now();
  let rc = 1;
  try {
    // JSPI makes run_culebra return a promise in the GPU build; the GPU waits
    // suspend beneath it. Nothing in the C++ call chain is written as async.
    rc = await mod.ccall("run_culebra", "number", ["string"], [src], { async: useGpuBuild });
  } catch (err) {
    postMessage({ type: "result", rc: 1, out: "internal error: " + err, ms: performance.now() - t0 });
    return;
  }
  const out = mod.UTF8ToString(mod._get_output());
  postMessage({ type: "result", rc, out, ms: performance.now() - t0 });
};
