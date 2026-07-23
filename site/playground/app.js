// Playground UI. The WASM interpreter runs inside worker.js; this file owns
// the editor, toolbar, worker lifecycle (Stop = terminate + respawn), and
// the Output/TUI tabs.
import { createEditor } from "./editor.js";
import { Terminal } from "https://esm.sh/@xterm/xterm@5.5.0";

const $ = (id) => document.getElementById(id);
const editor = createEditor($("editor"), "");
const output = $("output");
const runBtn = $("run");
const stopBtn = $("stop");
const clearBtn = $("clear");
const examplesSel = $("examples");
const status = $("status");
const backendEl = $("backend");

// --- TUI pane --------------------------------------------------------------
//
// Term.app's fixed layout math (see e.g. samegame.cul's offsets()) reads
// _Term.cols()/rows() once per frame, so both sides need to agree on a
// single size — no dynamic resize in this first pass. worker.js can't reach
// this file's `self` (a Worker has its own global scope), so the size is
// sent as a message once the worker is ready, for _wasm_term_cols/rows
// (term.h) to read.
const TERM_COLS = 80;
const TERM_ROWS = 24;
const term = new Terminal({ cols: TERM_COLS, rows: TERM_ROWS, cursorBlink: true });
term.open($("tui"));

// --- Canvas pane -----------------------------------------------------------
//
// A Canvas program (canvas.h / src/preambles/canvas.cul) posts RGBA frames the
// worker forwards as "frame" messages; here they become putImageData on a
// <canvas>. The framebuffer's own w/h drive the backing store, and CSS scales
// it up crisply. Input (keys/pointer) is captured on the pane and forwarded to
// the worker, which the wasm side polls — see the input block near onData.
const gameCanvas = $("game");
const gctx = gameCanvas.getContext("2d");
const canvasPane = $("canvas-pane");

// --- tabs --------------------------------------------------------------

const TABS = ["output", "tui", "canvas"];
const tabButtons = { output: $("tab-output"), tui: $("tab-tui"), canvas: $("tab-canvas") };
const panes = { output, tui: $("tui"), canvas: canvasPane };
let activeTab = "output";

function switchTab(name) {
  if (name === activeTab) return;
  activeTab = name;
  for (const k of TABS) {
    tabButtons[k].classList.toggle("active", k === name);
    tabButtons[k].setAttribute("aria-selected", k === name ? "true" : "false");
    panes[k].classList.toggle("active", k === name);
  }
  if (name === "tui") term.focus();
  if (name === "canvas") canvasPane.focus();
}
tabButtons.output.addEventListener("click", () => switchTab("output"));
tabButtons.tui.addEventListener("click", () => switchTab("tui"));
tabButtons.canvas.addEventListener("click", () => switchTab("canvas"));

// A TUI program's Term.app enters/exits the alternate screen with these two
// escapes (src/preambles/term.cul) — reliable markers for "now drawing a
// game" vs "now printing lines", so tabs switch automatically instead of the
// user having to guess which pane a script will use.
const ALT_SCREEN_ENTER = "\x1b[?1049h";
const ALT_SCREEN_EXIT = "\x1b[?1049l";
let inTui = false;
let inCanvas = false;   // a "frame" message has switched to the Canvas pane

// Leave TUI/Canvas mode and show the Output pane. The routing state (inTui /
// inCanvas) and the visible tab must move together, so run/stop/onerror share
// this.
function resetToOutput() {
  inTui = false;
  inCanvas = false;
  heldButtons = 0;
  switchTab("output");
}

function appendOutput(text) {
  if (text) output.textContent += text;
}

// Output streams in arbitrary-sized chunks (see wasm_main.cc's StreamingBuf,
// one postMessage per std::cout/cerr flush) — a chunk can contain a whole
// alt-screen marker, straddle one, or contain neither, so route byte-range
// by byte-range rather than assuming one marker per message.
function routeOutput(text) {
  let rest = text;
  while (rest.length > 0) {
    if (!inTui) {
      const i = rest.indexOf(ALT_SCREEN_ENTER);
      if (i === -1) { appendOutput(rest); return; }
      appendOutput(rest.slice(0, i));
      inTui = true;
      switchTab("tui");
      rest = rest.slice(i);
    } else {
      const i = rest.indexOf(ALT_SCREEN_EXIT);
      if (i === -1) { term.write(rest); return; }
      let end = i + ALT_SCREEN_EXIT.length;
      // term.cul's Term.app always follows the exit marker with an SGR
      // reset in the same IO.print call; consume it too so it doesn't leak
      // into the Output pane as literal "[0m" once routing switches back.
      if (rest.startsWith("\x1b[0m", end)) end += 4;
      term.write(rest.slice(0, end));
      inTui = false;
      switchTab("output");
      rest = rest.slice(end);
    }
  }
}

// --- examples (playground-safe: no Proc/argv; FS is MEMFS-backed) ---------

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
print("GPU    : {Tensor.gpu_available()}\\n")
`,
  "Tensors on the GPU": `# Where WebGPU is available, auto mode picks the device per op: small
# kernels stay on the CPU (a browser GPU dispatch has a fixed ~0.6 ms
# floor), big ones go to the GPU. Both are measured here.
fn bench(label, n) {
  let a = Tensor.randn([n, n])
  let b = Tensor.randn([n, n])
  Tensor.eval(a); Tensor.eval(b)
  mut warm = a.dot(b)
  Tensor.eval(warm)                     # compile shaders / first dispatch
  let t0 = Time.monotonic()
  mut i = 0
  while i < 5 {
    mut c = a.dot(b)
    Tensor.eval(c)
    i += 1
  }
  print("{label} {n}x{n}: {(Time.monotonic() - t0) * 200.0} ms/iter\\n")
}

print("GPU available: {Tensor.gpu_available()}\\n\\n")
for n in [64, 512] {
  Tensor.use_cpu();  bench("cpu ", n)
  Tensor.use_gpu();  bench("gpu ", n)
  Tensor.use_auto(); bench("auto", n)
  print("\\n")
}
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
  "TUI: Same Game": `# Same Game (さめがめ) — click (or arrows+space) a group of two or more
# connected same-colored tiles to clear it; tiles above fall, empty columns
# collapse left. Bigger groups score more. q quits, n starts a new game,
# s shows best scores, h shows help. Switches to the TUI tab automatically.

ROWS = 9
COLS = 14
NCOLORS = 5
COLORS = [0, 196, 46, 39, 226, 201]   # 256-colour per tile value 1..5

INTERACTIVE = IO.stdin_is_terminal()

SCORE_DIR = Sys.env("HOME") + "/.samegame"
SCORE_FILE = SCORE_DIR + "/score.json"

fn clampi(v, lo, hi) { if v < lo { lo } else { if v > hi { hi } else { v } } }

fn new_board() {
  Random.seed(to_long(Time.now().unix()))
  mut b = []
  for _ in 0..ROWS {
    mut row = []
    for _ in 0..COLS { row.push(Random.int(1, NCOLORS + 1)) }   # 1..NCOLORS
    b.push(row)
  }
  b
}

# All cells orthogonally connected to (r,c) sharing its colour.
fn group_at(b, r, c) {
  color = b[r][c]
  mut out = []
  if color == 0 { return out }
  mut seen = {}
  mut stack = [(r, c)]
  while stack.size() > 0 {
    (pr, pc) = stack.pop()
    if (pr >= 0 && pr < ROWS && pc >= 0 && pc < COLS) {
      key = to_string(pr) + "," + to_string(pc)
      if !seen.has(key) {
        mut seen[key] = true
        if b[pr][pc] == color {
          out.push((pr, pc))
          stack.push((pr - 1, pc))
          stack.push((pr + 1, pc))
          stack.push((pr, pc - 1))
          stack.push((pr, pc + 1))
        }
      }
    }
  }
  out
}

fn remove_cells(b, cells) {
  for cell in cells { b[cell[0]][cell[1]] = 0 }
}

# Tiles fall to the bottom of each column.
fn gravity(b) {
  mut c = 0
  while c < COLS {
    mut col = []
    mut r = ROWS - 1
    while r >= 0 { if b[r][c] != 0 { col.push(b[r][c]) }; r = r - 1 }
    mut r2 = ROWS - 1
    mut i = 0
    while r2 >= 0 {
      b[r2][c] = if i < col.size() { v = col[i]; i = i + 1; v } else { 0 }
      r2 = r2 - 1
    }
    c = c + 1
  }
}

# Empty columns are removed; the rest shift left.
fn collapse(b) {
  mut write = 0
  mut c = 0
  while c < COLS {
    if b[ROWS - 1][c] != 0 {
      if write != c {
        mut r = 0
        while r < ROWS { b[r][write] = b[r][c]; r = r + 1 }
      }
      write = write + 1
    }
    c = c + 1
  }
  while write < COLS {
    mut r = 0
    while r < ROWS { b[r][write] = 0; r = r + 1 }
    write = write + 1
  }
}

fn has_moves(b) {
  mut r = 0
  while r < ROWS {
    mut c = 0
    while c < COLS {
      v = b[r][c]
      if v != 0 {
        if (c + 1 < COLS && b[r][c + 1] == v) { return true }
        if (r + 1 < ROWS && b[r + 1][c] == v) { return true }
      }
      c = c + 1
    }
    r = r + 1
  }
  false
}

fn count_tiles(b) {
  mut n = 0
  for row in b { for v in row { if v != 0 { n = n + 1 } } }
  n
}

# First cell whose group is removable (used by the auto-demo).
fn find_move(b) {
  mut r = 0
  while r < ROWS {
    mut c = 0
    while c < COLS {
      if (b[r][c] != 0 && group_at(b, r, c).size() >= 2) { return (r, c) }
      c = c + 1
    }
    r = r + 1
  }
  (-1, -1)
}

fn offsets(s) { ((s.cols() - COLS * 2) / 2, (s.rows() - ROWS) / 2) }

fn draw(s, b, cur_r, cur_c, score, msg) {
  s.clear()
  (ox, oy) = offsets(s)
  s.put(ox, oy - 2, "Same Game   Score: " + to_string(score) + "   " + msg)
  mut r = 0
  while r < ROWS {
    mut c = 0
    while c < COLS {
      v = b[r][c]
      px = ox + c * 2
      py = oy + r
      cur = (INTERACTIVE && r == cur_r && c == cur_c)
      if v == 0 {
        if cur { s.put(px, py, "()") } else { s.put(px, py, "  ") }
      } else {
        if cur { s.put(px, py, "()", Term.style(fg: 15, bg: COLORS[v])) }
        else { s.put(px, py, "  ", Term.style(bg: COLORS[v])) }
      }
      c = c + 1
    }
    r = r + 1
  }
  s.flush()
}

fn load_scores() {
  if !FS.exists(SCORE_FILE) { return [] }
  try { JSON.parse(FS.read(SCORE_FILE)) } catch e { [] }
}

fn save_scores(scores) {
  FS.mkdir(SCORE_DIR)
  FS.write(SCORE_FILE, JSON.stringify(scores, indent: 2))
}

# Merges \`entry\` into \`scores\`, keeps the top 5 by score, and reports
# whether \`entry\` made the cut.
fn rank_into(scores, entry) {
  mut list = scores.slice(0, scores.size())
  list.push(entry)
  list.sort_by(fn (x) { x.score }, reverse: true)
  list = list.slice(0, 5)
  (list, list.index_of(entry) >= 0)
}

# Persists \`score\` into the top-5 file. Returns (top5, entry) — entry is
# nil when the score didn't make the cut (and nothing is written).
fn record_score(score) {
  let scores = load_scores()
  let now = Time.now()
  let entry = {score: score, date: now.format("%Y-%m-%d"), time: now.format("%H:%M:%S")}
  let (list, made_top5) = rank_into(scores, entry)
  if made_top5 { save_scores(list) }
  (list, if made_top5 { entry } else { nil })
}

fn spaces(n) {
  mut out = ""
  mut i = 0
  while i < n { out = out + " "; i = i + 1 }
  out
}

fn draw_score_box(s, scores, highlight, footer) {
  let cols = s.cols()
  let rows = s.rows()
  let w = 40
  let n = if scores.size() > 0 { scores.size() } else { 1 }
  let list_start = 3
  let footer_row = list_start + n + 1
  let h = footer_row + 2
  let bx = (cols - w) / 2
  let by = (rows - h) / 2
  let blank = spaces(w)
  let box_style = Term.style(bg: 236, fg: 255)
  mut r = 0
  while r < h { s.put(bx, by + r, blank, box_style); r = r + 1 }
  s.put(bx + 2, by + 1, "BEST 5", Term.style(bg: 236, fg: 226, bold: true))
  if scores.size() == 0 {
    s.put(bx + 2, by + list_start, "no scores yet", box_style)
  } else {
    mut i = 0
    while i < scores.size() {
      let e = scores[i]
      let row = to_string(i + 1) + ". " + to_string(e.score) + "   " + e.date + " " + e.time
      let style = if (highlight != nil && e == highlight) { Term.style(bg: 226, fg: 16, bold: true) } else { box_style }
      s.put(bx + 2, by + list_start + i, row, style)
      i = i + 1
    }
  }
  s.put(bx + 2, by + footer_row, footer, Term.style(bg: 236, fg: 244))
  s.flush()
}

# Shows the persisted top 5, plus the live in-progress score (highlighted)
# if it would currently place in the top 5. Blocks until any key is pressed.
fn show_scores(s, board, cur_r, cur_c, score, msg) {
  let scores = load_scores()
  let entry = {score: score, date: "(current)", time: ""}
  let (list, made_top5) = rank_into(scores, entry)
  let shown = if made_top5 { list } else { scores }
  let highlight = if made_top5 { entry } else { nil }
  draw(s, board, cur_r, cur_c, score, msg)
  draw_score_box(s, shown, highlight, "press any key")
  while true { let e = s.poll(0.1); if e != nil && e.kind == "key" { return } }
}

fn draw_help_box(s) {
  let cols = s.cols()
  let rows = s.rows()
  let w = 44
  let lines = [
    "Click 2+ same-colored tiles to clear them.",
    "",
    "arrows    move cursor",
    "space     clear group",
    "n         new game",
    "s         best scores",
    "h         this help",
    "q         quit",
  ]
  let list_start = 3
  let footer_row = list_start + lines.size() + 1
  let h = footer_row + 2
  let bx = (cols - w) / 2
  let by = (rows - h) / 2
  let blank = spaces(w)
  let box_style = Term.style(bg: 236, fg: 255)
  mut r = 0
  while r < h { s.put(bx, by + r, blank, box_style); r = r + 1 }
  s.put(bx + 2, by + 1, "HELP", Term.style(bg: 236, fg: 226, bold: true))
  mut i = 0
  while i < lines.size() {
    s.put(bx + 2, by + list_start + i, lines[i], box_style)
    i = i + 1
  }
  s.put(bx + 2, by + footer_row, "press any key", Term.style(bg: 236, fg: 244))
  s.flush()
}

# Shows the help overlay. Blocks until any key is pressed.
fn show_help(s, board, cur_r, cur_c, score, msg) {
  draw(s, board, cur_r, cur_c, score, msg)
  draw_help_box(s)
  wait_any_key(s)
}

fn wait_any_key(s) {
  while true {
    let e = s.poll(0.1)
    if e != nil && e.kind == "key" { return e.key }
  }
}

fn try_remove(b, r, c) {
  cells = group_at(b, r, c)
  if cells.size() < 2 { return -1 }
  remove_cells(b, cells)
  gravity(b)
  collapse(b)
  cells.size()
}

Term.app(fn (s) {
  mut board = new_board()
  mut score = 0
  mut cur_r = 0
  mut cur_c = 0
  mut msg = ""

  fn new_round() {
    board = new_board()
    score = 0
    cur_r = 0
    cur_c = 0
    msg = ""
  }

  draw(s, board, cur_r, cur_c, score, msg)

  while true {
    if !has_moves(board) {
      left = count_tiles(board)
      if left == 0 { score = score + 1000; msg = "CLEARED!  bonus +1000" }
      else { msg = "no moves — game over (" + to_string(left) + " left)" }
      msg = msg + "  —  q: quit, other key: new game"
      draw(s, board, cur_r, cur_c, score, msg)
      if !INTERACTIVE { return }
      let (best, entry) = record_score(score)
      if entry != nil {
        draw_score_box(s, best, entry, "q: quit   other key: new game")
        if wait_any_key(s) == "q" { return }
        new_round()
        draw(s, board, cur_r, cur_c, score, msg)
        continue
      }
      if wait_any_key(s) == "q" { return }
      new_round()
      draw(s, board, cur_r, cur_c, score, msg)
    }

    mut sr = -1
    mut sc = -1
    if INTERACTIVE {
      ev = s.poll(0.1)
      if ev != nil {
        if ev.kind == "key" {
          if ev.key == "q" { return }
          if ev.key == "n" { new_round() }
          if ev.key == "s" { show_scores(s, board, cur_r, cur_c, score, msg) }
          if ev.key == "h" { show_help(s, board, cur_r, cur_c, score, msg) }
          if ev.key == "left"  { cur_c = clampi(cur_c - 1, 0, COLS - 1) }
          if ev.key == "right" { cur_c = clampi(cur_c + 1, 0, COLS - 1) }
          if ev.key == "up"    { cur_r = clampi(cur_r - 1, 0, ROWS - 1) }
          if ev.key == "down"  { cur_r = clampi(cur_r + 1, 0, ROWS - 1) }
          if (ev.key == " " || ev.key == "enter") { sr = cur_r; sc = cur_c }
          draw(s, board, cur_r, cur_c, score, msg)
        } else {
          if (ev.kind == "mouse" && ev.event == "press" && ev.button == "left") {
            (ox, oy) = offsets(s)
            gc = (ev.x - ox) / 2
            gr = ev.y - oy
            if (gr >= 0 && gr < ROWS && gc >= 0 && gc < COLS) { sr = gr; sc = gc }
          }
        }
      }
    } else {
      m = find_move(board)
      sr = m[0]
      sc = m[1]
      Time.sleep(0.04)
    }

    if sr >= 0 {
      n = try_remove(board, sr, sc)
      if n >= 2 {
        score = score + (n - 2) * (n - 2)
        msg = "cleared " + to_string(n)
        draw(s, board, cur_r, cur_c, score, msg)
      }
    }
  }
}, mouse: INTERACTIVE)   # enable mouse reporting (interactive only) so clicks reach s.poll()
`,
  "Canvas: Rocci Bird": `// rocci-bird — a flappy-bird clone on the Canvas 2D library (a culebra port of
// Roc's WASM-4 example). Tap SPACE (or the up arrow, or click) to flap and
// thread the gaps. Switches to the Canvas tab automatically; best score
// persists in the (MEMFS-backed) filesystem for the session.

let W = 160
let H = 160
let GROUND_Y = 144
let BIRD_X = 44
let BIRD_SIZE = 8

let PIPE_W = 24
let GAP = 52
let SPEED = 2
let SPAWN_GAP = 96

let GRAVITY = 0.28
let FLAP = -3.6

let SKY = Canvas.rgba(112, 197, 206)
let PIPE = Canvas.rgba(92, 184, 64)
let PIPE_DK = Canvas.rgba(64, 148, 44)
let GROUND = Canvas.rgba(222, 184, 120)
let GROUND_DK = Canvas.rgba(150, 116, 70)
let INK = Canvas.rgba(34, 40, 54)
let WHITE = Canvas.rgba(255, 255, 255)

let BIRD_PAL = [
  Canvas.rgba(0, 0, 0, 0),
  Canvas.rgba(250, 208, 60),
  Canvas.rgba(240, 140, 40),
  Canvas.rgba(40, 40, 40),
  Canvas.rgba(255, 255, 255),
]
let BIRD_PX = [
  0, 0, 1, 1, 1, 1, 0, 0,
  0, 1, 1, 1, 1, 1, 1, 0,
  1, 1, 1, 3, 1, 1, 1, 2,
  1, 1, 1, 1, 1, 1, 2, 2,
  1, 4, 4, 4, 4, 1, 1, 2,
  1, 4, 4, 4, 4, 1, 1, 0,
  0, 1, 4, 4, 1, 1, 0, 0,
  0, 0, 1, 1, 1, 0, 0, 0,
]
let bird = Canvas.Sprite.new(BIRD_PX, 8, 8, BIRD_PAL)

let SAVE_DIR = "/rocci"
let SAVE_FILE = "/rocci/best.json"
let load_best = fn () {
  if !FS.exists(SAVE_FILE) { return 0 }
  try { JSON.parse(FS.read(SAVE_FILE)) } catch e { 0 }
}
let save_best = fn (n) {
  try { FS.mkdir(SAVE_DIR); FS.write(SAVE_FILE, JSON.stringify(n)) } catch e { nil }
}

let TITLE = 0
let PLAY = 1
let OVER = 2

mut state = TITLE
mut bird_y = 72.0
mut bird_v = 0.0
mut pipes = []
mut score = 0
mut best = load_best()

let demo = !IO.stdin_is_terminal()

let spawn_pipe = fn (x) {
  let gap_top = Random.int(24, GROUND_Y - GAP - 24)
  pipes.push([x, gap_top, 0])
}

let reset = fn () {
  bird_y = 72.0
  bird_v = 0.0
  score = 0
  pipes = []
  spawn_pipe(W + 20)
}

let flap = fn () { bird_v = FLAP; Canvas.tone(520, 6, 60, Canvas.PULSE) }

let overlap = fn (ax, ay, aw, ah, bx, by, bw, bh) {
  ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by
}

let hit_something = fn (byi) {
  if byi < 0 || byi + BIRD_SIZE > GROUND_Y { return true }
  mut i = 0
  while i < pipes.size() {
    let p = pipes[i]
    let px = p[0]
    let gt = p[1]
    let top_hit = overlap(BIRD_X, byi, BIRD_SIZE, BIRD_SIZE, px, 0, PIPE_W, gt)
    let bot_hit = overlap(BIRD_X, byi, BIRD_SIZE, BIRD_SIZE, px, gt + GAP, PIPE_W, GROUND_Y - (gt + GAP))
    if top_hit || bot_hit { return true }
    i = i + 1
  }
  false
}

let draw_pipe = fn (px, gap_top) {
  Canvas.rect(px, 0, PIPE_W, gap_top, PIPE)
  Canvas.rect(px, gap_top - 6, PIPE_W, 6, PIPE_DK)
  Canvas.rect(px, 0, 3, gap_top, PIPE_DK)
  let by = gap_top + GAP
  Canvas.rect(px, by, PIPE_W, GROUND_Y - by, PIPE)
  Canvas.rect(px, by, PIPE_W, 6, PIPE_DK)
  Canvas.rect(px, by, 3, GROUND_Y - by, PIPE_DK)
}

let draw_world = fn () {
  Canvas.clear(SKY)
  mut i = 0
  while i < pipes.size() {
    let p = pipes[i]
    draw_pipe(p[0], p[1])
    i = i + 1
  }
  Canvas.rect(0, GROUND_Y, W, H - GROUND_Y, GROUND)
  Canvas.rect(0, GROUND_Y, W, 4, GROUND_DK)
  bird.draw(BIRD_X, to_long(bird_y))
}

let center = fn (s, y, color) { Canvas.text(s, (W - Canvas.text_width(s)) / 2, y, color) }

let inp = Canvas.Input.new()
mut prev_pointer = 0

let wants_flap = fn () {
  let key = inp.pressed(Canvas.A) || inp.pressed(Canvas.UP)
  let pointer = Canvas.mouse().buttons
  let click = pointer != 0 && prev_pointer == 0
  prev_pointer = pointer
  if demo {
    mut target = 72
    mut i = 0
    while i < pipes.size() {
      let p = pipes[i]
      if p[0] + PIPE_W > BIRD_X { target = p[1] + GAP / 2; i = pipes.size() }
      else { i = i + 1 }
    }
    return bird_y > to_float(target) && bird_v > -1.0
  }
  key || click
}

reset()

Canvas.run(W, H, fn () {
  inp.update()

  if state == TITLE {
    draw_world()
    center("ROCCI BIRD", 44, INK)
    center("PRESS SPACE", 92, INK)
    if wants_flap() { state = PLAY; flap() }
  } else {
    if state == PLAY {
      if wants_flap() { flap() }
      bird_v = bird_v + GRAVITY
      bird_y = bird_y + bird_v

      mut i = 0
      while i < pipes.size() {
        pipes[i][0] = pipes[i][0] - SPEED
        if pipes[i][2] == 0 && pipes[i][0] + PIPE_W < BIRD_X {
          pipes[i][2] = 1
          score = score + 1
          Canvas.tone(720, 5, 55, Canvas.TRIANGLE)
        }
        i = i + 1
      }
      if pipes.size() > 0 {
        let last = pipes[pipes.size() - 1]
        if last[0] < W - SPAWN_GAP { spawn_pipe(W + 20) }
      }
      if pipes.size() > 0 && pipes[0][0] + PIPE_W < 0 { pipes = pipes[1..pipes.size()] }

      if hit_something(to_long(bird_y)) {
        Canvas.tone(160, 14, 70, Canvas.SAWTOOTH)
        state = OVER
        if score > best { best = score; save_best(best) }
      }
    }

    draw_world()
    Canvas.text(to_string(score), 4, 4, INK)

    if state == OVER {
      Canvas.rect(20, 52, W - 40, 56, INK)
      center("GAME OVER", 60, WHITE)
      center("SCORE " + to_string(score), 76, WHITE)
      center("BEST " + to_string(best), 88, WHITE)
      if wants_flap() { state = PLAY; reset(); flap() }
    }
  }

  true
})
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
      // Term.h's cols()/rows() read this from the worker's own global scope
      // (a Worker can't see app.js's `self` directly) — must arrive before
      // the program runs, so send it as soon as the module is up.
      worker.postMessage({ type: "termSize", cols: TERM_COLS, rows: TERM_ROWS });
      runBtn.disabled = false;
      // full = JSPI available: Tensor can reach WebGPU, and Term.read_key
      // can wait for a real keypress so a TUI script is actually playable.
      backendEl.textContent = msg.backend === "full" ? "Tensor: GPU" : "Tensor: CPU";
      backendEl.title = msg.backend === "full"
        ? "WebGPU + interactive TUI available — auto mode runs large tensor ops on the GPU"
        : "Basic build (this browser lacks JSPI/WebGPU) — a TUI script runs non-interactively";
      if (!running) setStatus("ready");
      return;
    }
    if (msg.type === "output") {
      routeOutput(msg.text);
      return;
    }
    if (msg.type === "frame") {
      drawFrame(msg);
      return;
    }
    if (msg.type === "tone") {
      playTone(msg.freq, msg.dur, msg.vol, msg.wave);
      return;
    }
    if (msg.type === "done") {
      running = false;
      stopRafPump();
      stopBtn.disabled = true;
      runBtn.disabled = false;
      if (!inTui && !inCanvas && output.textContent === "") output.textContent = "(no output)";
      output.classList.toggle("err", msg.rc !== 0);
      setStatus(msg.rc === 0 ? `done in ${Math.round(msg.ms)} ms` : "error", msg.rc !== 0);
    }
  };
  worker.onerror = (e) => {
    setStatus("worker error", true);
    stopRafPump();
    resetToOutput();
    appendOutput(String(e.message || e));
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
  term.reset();
  resetToOutput();
  startRafPump();   // drives Canvas present()'s frame wait; harmless otherwise
  setStatus("running…");
  worker.postMessage({ type: "run", src: editor.getValue() });
}

function stop() {
  if (!running) return;
  worker.terminate();
  running = false;
  stopRafPump();
  stopBtn.disabled = true;
  runBtn.disabled = true; // until the fresh worker reports ready
  resetToOutput();
  setStatus("stopped — reloading…");
  appendOutput("\n[stopped]");
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
  editor.setValue(EXAMPLES[name]);
  editor.focus();
});

document.addEventListener("keydown", (e) => {
  if ((e.metaKey || e.ctrlKey) && e.key === "Enter") {
    e.preventDefault();
    run();
  }
});

// xterm.js decodes real keyboard/mouse DOM events into terminal byte
// sequences itself (including SGR mouse reports, once a TUI script's
// `\x1b[?1002h\x1b[?1006h` mouse-tracking escape reaches it) and delivers
// them all through onData — one registration covers keys and clicks for
// every run, forwarded to whichever worker is currently live.
term.onData((data) => {
  if (worker) worker.postMessage({ type: "key", key: data });
});

// --- Canvas frame / input / audio -----------------------------------------

// Paint a posted RGBA frame. The framebuffer's own dimensions drive the
// backing store (so a script can pick any size); CSS scales it up crisply.
// The first frame of a run auto-switches to the Canvas tab, mirroring how the
// TUI tab reacts to the alt-screen marker.
function drawFrame(msg) {
  if (gameCanvas.width !== msg.w || gameCanvas.height !== msg.h) {
    gameCanvas.width = msg.w;
    gameCanvas.height = msg.h;
  }
  gctx.putImageData(new ImageData(new Uint8ClampedArray(msg.buf), msg.w, msg.h), 0, 0);
  if (!inCanvas) {
    inCanvas = true;
    switchTab("canvas");
  }
}

// A requestAnimationFrame heartbeat: each tick lets the worker's suspended
// present() (self.__nextFrame) resolve, pacing the game to the display's
// refresh. Started for every run (a plain script never waits on it) and
// stopped when the run ends.
let rafId = null;
function startRafPump() {
  if (rafId !== null) return;
  const pump = () => {
    if (worker) worker.postMessage({ type: "tick" });
    rafId = requestAnimationFrame(pump);
  };
  rafId = requestAnimationFrame(pump);
}
function stopRafPump() {
  if (rafId !== null) {
    cancelAnimationFrame(rafId);
    rafId = null;
  }
}

// Simple WebAudio tone for Canvas.tone — one oscillator per call with a quick
// decay envelope. A deliberately small subset of a real chiptune APU (no
// per-channel state, noise approximated by a square wave); enough for game
// blips. The context is created lazily on first use so the page needs no audio
// permission until a program actually plays something.
let audioCtx = null;
const WAVES = ["square", "triangle", "sawtooth", "square"];  // 3 = noise ≈ square
function playTone(freq, durFrames, vol, wave) {
  try {
    if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    const now = audioCtx.currentTime;
    const dur = Math.max(1, durFrames) / 60;
    const osc = audioCtx.createOscillator();
    const gain = audioCtx.createGain();
    osc.type = WAVES[wave] || "square";
    osc.frequency.value = Math.max(1, freq);
    const peak = Math.max(0, Math.min(1, vol / 100)) * 0.2;  // keep it gentle
    gain.gain.setValueAtTime(peak, now);
    gain.gain.exponentialRampToValueAtTime(0.0001, now + dur);
    osc.connect(gain).connect(audioCtx.destination);
    osc.start(now);
    osc.stop(now + dur);
  } catch {
    // Audio unavailable (autoplay policy, no device) — a game stays playable.
  }
}

// Keyboard → button bitmask (bits match src/preambles/canvas.cul: LEFT=1,
// RIGHT=2, UP=4, DOWN=8, A=16, B=32). Captured on the focused Canvas pane so
// arrows/space don't also scroll the page. The worker keeps the held mask in
// self.__canvasButtons for the wasm side to poll.
const KEY_BITS = {
  ArrowLeft: 1, ArrowRight: 2, ArrowUp: 4, ArrowDown: 8,
  " ": 16, z: 16, Z: 16, x: 32, X: 32,
};
let heldButtons = 0;
canvasPane.addEventListener("keydown", (e) => {
  const bit = KEY_BITS[e.key];
  if (bit === undefined) return;
  e.preventDefault();
  heldButtons |= bit;
  if (worker) worker.postMessage({ type: "input", buttons: heldButtons });
});
canvasPane.addEventListener("keyup", (e) => {
  const bit = KEY_BITS[e.key];
  if (bit === undefined) return;
  e.preventDefault();
  heldButtons &= ~bit;
  if (worker) worker.postMessage({ type: "input", buttons: heldButtons });
});

// Pointer → framebuffer coordinates (accounting for the CSS scale-up).
function sendMouse(e) {
  const r = gameCanvas.getBoundingClientRect();
  const x = Math.floor(((e.clientX - r.left) / r.width) * gameCanvas.width);
  const y = Math.floor(((e.clientY - r.top) / r.height) * gameCanvas.height);
  if (worker) worker.postMessage({ type: "canvasMouse", x, y, buttons: e.buttons });
}
for (const ev of ["pointermove", "pointerdown", "pointerup"]) {
  gameCanvas.addEventListener(ev, sendMouse);
}

// --- boot -----------------------------------------------------------------

editor.setValue(EXAMPLES["Hello"]);
setStatus("loading…");
spawnWorker();
