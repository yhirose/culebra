// Playground UI. The WASM interpreter runs inside worker.js; this file owns
// the editor, toolbar, and worker lifecycle (Stop = terminate + respawn).

const $ = (id) => document.getElementById(id);
const editor = $("editor");
const output = $("output");
const runBtn = $("run");
const stopBtn = $("stop");
const clearBtn = $("clear");
const examplesSel = $("examples");
const status = $("status");

// --- examples (playground-safe: no Term/Proc/FS/argv) ---------------------

const EXAMPLES = {
  "Hello": `print("Hello, culebra!\\n")
let name = "WASM"
print("Running on {name} in your browser.\\n")
`,
  "Functions & closures": `fn fib(n) { if n < 2 { n } else { fib(n-1) + fib(n-2) } }
print("fib(20) = {fib(20)}\\n")

let xs = [1, 2, 3, 4, 5]
let squares = xs.map(|x| x * x)
print("squares      = {squares}\\n")
print("sum          = {squares.reduce(0, |a, b| a + b)}\\n")

fn make_counter() {
  mut n = 0
  fn() { n += 1; n }
}
let tick = make_counter()
tick(); tick()
print("counter says = {tick()}\\n")
`,
  "Classes & operators": `class Vec2 {
  new(x, y)  { this.x = x; this.y = y }
  __add__(o) { Vec2.new(this.x + o.x, this.y + o.y) }
  __mul__(k) { Vec2.new(this.x * k, this.y * k) }
  show()     { "({this.x}, {this.y})" }
}

let a = Vec2.new(1, 2)
let b = Vec2.new(3, 4)
print("a + b   = {(a + b).show()}\\n")
print("a * 10  = {(a * 10).show()}\\n")
`,
  "Generators": `fn countdown(start) {
  mut i = start
  while i > 0 { yield i; i -= 1 }
}
for v in countdown(3) { print("{v}...\\n") }

fn chunk(arr, n) {
  mut buf = []
  for v in arr {
    buf.push(v)
    if buf.size() >= n { yield buf; buf = [] }
  }
  if buf.size() > 0 { yield buf }
}
print("chunks = {chunk([1, 2, 3, 4, 5], 2).collect()}\\n")
`,
  "Tensors": `let t = Tensor.from([[1.0, 2.0], [3.0, 4.0]])
print("tensor : {t.to_string()}\\n")
print("shape  : {t.shape()}\\n")
# GPU backends aren't compiled on wasm — everything runs on the CPU path.
`,
  "Error handling": `fn risky(n) {
  if n < 0 { throw {kind: "RangeError", message: "n must be >= 0, got {n}"} }
  n * 2
}

try {
  print("risky(21) = {risky(21)}\\n")
  risky(-1)
} catch e {
  print("caught {e.kind}: {e.message}\\n")
}
`,
};

for (const name of Object.keys(EXAMPLES)) {
  const opt = document.createElement("option");
  opt.value = name;
  opt.textContent = name;
  examplesSel.appendChild(opt);
}

// --- worker lifecycle -----------------------------------------------------

let worker = null;
let running = false;

function setStatus(text, isErr = false) {
  status.textContent = text;
  status.classList.toggle("err", isErr);
}

function spawnWorker() {
  worker = new Worker("./worker.js", { type: "module" });
  worker.onmessage = (e) => {
    const msg = e.data;
    if (msg.type === "ready") {
      runBtn.disabled = false;
      if (!running) setStatus("ready");
      return;
    }
    if (msg.type === "result") {
      running = false;
      stopBtn.disabled = true;
      runBtn.disabled = false;
      output.textContent = msg.out.length ? msg.out : "(no output)";
      output.classList.toggle("err", msg.rc !== 0);
      setStatus(msg.rc === 0 ? `done in ${Math.round(msg.ms)} ms` : "error", msg.rc !== 0);
    }
  };
  worker.onerror = (e) => {
    setStatus("worker error", true);
    output.textContent = String(e.message || e);
    output.classList.add("err");
    stopBtn.disabled = true;
    if (running) { // recover like Stop; load failures stay down
      running = false;
      worker.terminate();
      spawnWorker();
    }
  };
}

function run() {
  if (running || runBtn.disabled) return;
  running = true;
  runBtn.disabled = true;
  stopBtn.disabled = false;
  output.classList.remove("err");
  output.textContent = "";
  setStatus("running…");
  worker.postMessage({ type: "run", src: editor.value });
}

function stop() {
  if (!running) return;
  worker.terminate();
  running = false;
  stopBtn.disabled = true;
  runBtn.disabled = true; // until the fresh worker reports ready
  setStatus("stopped — reloading…");
  output.textContent += "\n[stopped]";
  spawnWorker();
}

// --- toolbar wiring -------------------------------------------------------

runBtn.addEventListener("click", run);
stopBtn.addEventListener("click", stop);
clearBtn.addEventListener("click", () => {
  output.textContent = "";
  output.classList.remove("err");
});
examplesSel.addEventListener("change", () => {
  const name = examplesSel.value;
  if (!name) return;
  editor.value = EXAMPLES[name];
  examplesSel.value = "";
  editor.focus();
});

document.addEventListener("keydown", (e) => {
  if ((e.metaKey || e.ctrlKey) && e.key === "Enter") {
    e.preventDefault();
    run();
  }
});

// Two-space indent on Tab inside the editor.
editor.addEventListener("keydown", (e) => {
  if (e.key === "Tab") {
    e.preventDefault();
    const { selectionStart: s, selectionEnd: t, value } = editor;
    editor.value = value.slice(0, s) + "  " + value.slice(t);
    editor.selectionStart = editor.selectionEnd = s + 2;
  }
});

// --- boot -----------------------------------------------------------------

editor.value = EXAMPLES["Hello"];
setStatus("loading…");
spawnWorker();
