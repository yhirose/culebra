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
  new(x, y)  { self.x = x; self.y = y }
  __add__(o) { Vec2.new(self.x + o.x, self.y + o.y) }
  __mul__(k) { Vec2.new(self.x * k, self.y * k) }
  show()     { "({self.x}, {self.y})" }
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
  "Canvas: Rocci Bird": `# Rocci Bird — a faithful culebra port of Luke Boswell's WASM-4 example
# (github.com/lukewilliamboswell/roc-wasm4, examples/rocci-bird.roc; sprite art
# by Luke DeVault). The five 2BPP sprite sheets, the Lospec "Candy Cloud"
# palette, the physics, the pixel-readback collision, the ADSR tones, and the
# three-state flow (title -> game -> game over) are reproduced from the original.
#
# WASM-4 draws indexed 2BPP art through a per-draw "draw colors" remap; this
# game uses a fixed remap per asset, so each sheet is baked once into an indexed
# Canvas.Sprite (index 0 transparent for the sprites, the background colour for
# the ground). Persistence is run-lifetime: the high score survives restarts
# within a session but not a reload, matching the Playground's in-memory disk.

# --- palette ---------------------------------------------------------------
let C1 = Canvas.rgba(0xe6, 0xe6, 0xc0)   # background / lightest
let C2 = Canvas.rgba(0xb4, 0x94, 0xb7)
let C3 = Canvas.rgba(0x42, 0x43, 0x6e)
let C4 = Canvas.rgba(0x26, 0x01, 0x3f)   # darkest / text
let CLEAR = Canvas.rgba(0, 0, 0, 0)
let SPRITE_PAL = [CLEAR, C2, C3, C4]     # set_draw_colors: None, C2, C3, C4
let GROUND_PAL = [C1, C2, C3, C4]        # set_draw_colors: C1, C2, C3, C4

# --- 2BPP sprite decoding --------------------------------------------------
let _hexd = "0123456789abcdef".graphemes().collect()
let hex_bytes = fn (s) {
  let cs = s.graphemes().collect()
  mut out = []
  mut i = 0
  while i < cs.size() {
    out.push(_hexd.index_of(cs[i]) * 16 + _hexd.index_of(cs[i + 1]))
    i = i + 2
  }
  out
}
# WASM-4 2BPP: a continuous bitstream, MSB first, 4 pixels per byte, each pixel
# a palette index 0..3. All our sheet widths are byte-aligned per row.
let decode2bpp = fn (bytes, w, h) {
  mut out = []
  mut n = 0
  let total = w * h
  while n < total {
    let b = bytes[n / 4]
    out.push((b >> (6 - 2 * (n % 4))) & 3)
    n = n + 1
  }
  out
}
let load_sprite = fn (hex, w, h, pal) {
  Canvas.Sprite.new(decode2bpp(hex_bytes(hex), w, h), w, h, pal)
}

let rocci_sheet = load_sprite("0000000000000000000000000000000000000000155600001000000000000000000081000000804005568180140001800000018000028500000281400156a5a0150005a0000005a0000a9500000a85400056a9001556a5000000150000065500000655400006aa00155aaa001556aa00001695000006950000095500156aa9000556a900001694000016950000095400056a94000156a400001a940000169400000950000069500000569000001aa000001aa000002940000029400000264000000aa000000aa000002900000029000000290000000a8000000a8000002a0000002a0000002a0000000540000005400000280000002800000028000000a5000000a50000002000000020000000200000002400000024000000000000000000000000000000000000000000000000000000000000000000000000000000000000", 80, 16, SPRITE_PAL)
let ground_sprite = load_sprite("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa5555555555555555555555555555555555555555555555555555555555555555555555555555555500000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000004444444444444444444444444444444444444444444444444444444444444444444444444444444411111111111111111111111111111111111111111111111111111111111111111111111111111111555555555555555565555555555555595555555555555555555555955555555555555555555555554514518514514914914518515451492451461451461514614514924514614614551451451461514565665656595659565565655659565995595955995959559596559955959559559655595995959655959599666599659695965959659965a5659656566599965999665a56596596565966656566599959a69a5a696a5a66a5a69a9a6a6a5a6569a6a69a6969a5a69a5a96569a6a6aa69a9a96a9a6969a5a69aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 160, 13, GROUND_PAL)
let pipe_sprite = load_sprite("0aaaaaabf0255555555c26966a9aac36966a66ac36966a9aac0ffffffff003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac003659a66c003659a9ac0", 20, 160, SPRITE_PAL)
let plant_sheet = load_sprite("00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000504000000000000000000000000000000000010000000000000000000000000000000000aa0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000414000000000000000000000004000000000014002aa0002aa000000000002a00000000a00a0000800000000000000000000000000000000000000000000000000000000000000000000000000000000000000002800000000005040000011000000000000040150000000100500080080080880002a0000080800000020000800208000000000208000000000800000000000080000a000000000008004000000000000000000000000000000008200000000004140110015000000000a001505140028005145002001602081600080800aa01600a80082159602220000000000820000000002082002a00000280002200002820006111102b2b002800800a00000000000a8000206000000000051401511150000002420805141500022001441442055602859a00201602021560202008355d602aa00000000008a0000104002a880095c000820080200000a0a28914466096d5c0a0820020b000020000206000a15a00000000215a0151508000000048660158048200208505294215560219560021560805aa80805808169560aea0000280002ab800445100aea80039a0028a8200200002808a045191803efeb2828a80820c0008800020580261618020000028a800a15280000080599b02608a0880222a142050aaa800aaa8000aa80855a700855809555562bae800088000aeea01111400baba00bede0babaa002a8002a2ab81a6a7829be68aeaab83181c000800002558087181809828000aa0028080a0820200565d008020008aa20288a140217000217000027002aa27002aa002aaaa82bbaa00087002baea80666800baeb825b95cbaeae802e000aeaeea0baae0969796babaea31857000a000026700871c5721c870002c00080a0828a8a8015f54200080282e2822820a02170002170000270009c270009c000855700000000000000000000000000000002aceac000000000000000000000000ab5bea0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000003f03f00000000000000000000000003eac3c00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000f000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000", 360, 12, SPRITE_PAL)
let hiscore_sheet = load_sprite("2800a002800a002802800a002800a000002800a002800a00002800a002800a0096025809602580960960258096025800009602580960258000960258096025809ffffffffffffff60ffffffffffffff82ffffffffffffff02ffffffffffffff02c000000000000380c000000000000369c000000000000309c000000000000300c000000000000302c000000000000369c000000000000389c000000000000380c000000000000309c000000000000382c000000000000362c000000000000362c000000000000389c000000000000300c000000000000360c000000000000369c000000000000362c000000000000300c000000000000380c000000000000389c000000000000360c000000000000382c000000000000302c000000000000302c000000000000380c000000000000369c000000000000309c000000000000300c000000000000302c000000000000369c000000000000389c000000000000380c000000000000309c000000000000382c000000000000362c000000000000362c000000000000389c000000000000300c000000000000360c000000000000369ffffffffffffff62ffffffffffffff00ffffffffffffff80ffffffffffffff896025809602580960025809602580960025809602580960002580960258096002800a002800a0028000a002800a0028000a002800a00280000a002800a002800", 128, 16, SPRITE_PAL)

# --- constants (from the original) -----------------------------------------
let W = 160
let H = 160
let GRAVITY = 0.12
let JUMP = -2.2
let GAP = 40
let PLAYER_X = 70
let PLAYER_START_Y = 40
let GROUND_Y = H - 13     # 147
let PLANT_Y = H - 22      # 138
let PLANT_TYPES = 30

# --- tones (exact WASM-4 parameters) ---------------------------------------
# tone(freq, sustain, vol, wave, end_freq, attack, decay, release, peak, duty)
let flap_tone = fn () { Canvas.tone(700, 0, 10, Canvas.PULSE, 870, 10, 5, 3, 20, Canvas.DUTY_QUARTER) }
let point_tone = fn () { Canvas.tone(995, 0, 25, Canvas.PULSE2, 1000, 0, 10, 10, 75, Canvas.DUTY_HALF) }
let death_tone = fn () { Canvas.tone(170, 20, 100, Canvas.NOISE, 40, 0, 40, 0, 0, Canvas.DUTY_HALF) }

# --- animation -------------------------------------------------------------
# cells is a list of [frames, src_x]; every frame is \`sw\`x16 on \`sheet\`.
class Anim {
  new(sheet, sw, cells, fc, index, state) {
    self.sheet = sheet
    self.sw = sw
    self.cells = cells
    self.index = index
    self.state = state          # "loop" | "runonce" | "completed"
    self.last_updated = fc
  }
  reset(fc, index, state) { self.index = index; self.state = state; self.last_updated = fc }
  update(fc) {
    let fpu = self.cells[self.index][0]
    if fc - self.last_updated < fpu { return }
    let ni = if self.index + 1 == self.cells.size() { 0 } else { self.index + 1 }
    if self.state == "completed" {
      self.last_updated = fc
    } else if self.state == "loop" {
      self.index = ni; self.last_updated = fc
    } else {                    # runonce
      if ni == 0 { self.state = "completed"; self.last_updated = fc }
      else { self.index = ni; self.last_updated = fc }
    }
  }
  draw(x, y) { self.sheet.draw_sub(x, y, self.cells[self.index][1], 0, self.sw, 16) }
}

let idle_anim = fn (fc) { Anim.new(rocci_sheet, 16, [[17, 0], [6, 16], [17, 32]], fc, 0, "loop") }
let flap_anim = fn (fc) { Anim.new(rocci_sheet, 16, [[6, 16], [12, 32], [1, 0]], fc, 2, "completed") }
let fall_anim = fn (fc) { Anim.new(rocci_sheet, 16, [[10, 48], [10, 64]], fc, 0, "loop") }
let hiscore_anim = fn (fc) { Anim.new(hiscore_sheet, 32, [[5, 0], [5, 32], [5, 64], [5, 96]], fc, 0, "loop") }

# --- collision (pixel readback at the original's sample points) ------------
# A point counts as a hit when the framebuffer there is not the background
# colour C1. This must run after the world is drawn but before the player.
let BASE_POINTS = [[11, 2], [13, 3], [3, 5], [11, 6], [9, 8], [5, 9], [7, 10], [5, 12]]
let hit_at = fn (px, py) { Canvas.get_pixel(px, py) != C1 }

let collided = fn (player_y, anim_index) {
  if player_y < -1 { return hit_at(PLAYER_X + 13, 0) }
  for p in BASE_POINTS {
    if hit_at(PLAYER_X + p[0], player_y + p[1]) { return true }
  }
  if anim_index == 2 {
    if hit_at(PLAYER_X + 2, player_y + 1) { return true }
    if hit_at(PLAYER_X + 7, player_y + 1) { return true }
  } else if anim_index == 1 {
    if hit_at(PLAYER_X + 2, player_y + 2) { return true }
  }
  false
}

# --- drawing helpers -------------------------------------------------------
let draw_ground = fn (x) {
  ground_sprite.draw(x, GROUND_Y)
  ground_sprite.draw(x + W, GROUND_Y)
}
let draw_plants = fn (plants) {
  for pl in plants { plant_sheet.draw_sub(pl.x, PLANT_Y, pl.type * 12, 0, 12, 12) }
}
let draw_pipes = fn (pipes) {
  for pp in pipes {
    pipe_sprite.draw(pp.x, pp.gap_start - H, false, true)   # top pipe, flipped vertically
    pipe_sprite.draw(pp.x, pp.gap_start + GAP)              # bottom pipe
  }
}
let draw_score = fn (score, base_x, y) {
  let x = if score < 10 { base_x + 8 } else if score < 100 { base_x + 4 } else { base_x }
  Canvas.text(to_string(score), x, y, C4)
}

# --- app -------------------------------------------------------------------
class App {
  new() {
    self.interactive = IO.stdin_is_terminal()
    self.input = Canvas.Input.new()
    self.high_score = 0
    self.fc = 0
    Random.seed(self.fc)
    # Every field a method later reassigns must be declared in the constructor —
    # a property first assigned outside \`new\` is immutable in culebra. The title
    # state is set up directly here rather than via enter_title(), because \`self\`
    # is immutable while the constructor runs (methods can only read it).
    self.mode = "title"
    self.plants = self.starting_plants()
    self.pipes = []
    self.rocci = idle_anim(self.fc)
    self.hiscore = hiscore_anim(self.fc)
    self.score = 0
    self.max_score = 0
    self.player_y = 0.0
    self.player_vel = 0.0
    self.last_pipe = 0
    self.last_plant = 0
    self.last_flap = true
    self.new_high = false
    self.ground_x = 0
  }

  starting_plants() {
    mut ps = []
    mut i = 0
    while i < 15 { ps.push({ x: i * 12, type: Random.int(0, PLANT_TYPES) }); i = i + 1 }
    ps
  }

  enter_title() {
    self.mode = "title"
    self.plants = self.starting_plants()
    self.rocci = idle_anim(self.fc)
  }

  enter_game() {
    self.mode = "game"
    # The original reseeds from the frame count at the moment play begins, so
    # the run feels randomly seeded (players rarely start on the same frame).
    Random.seed(self.fc)
    self.score = 0
    self.max_score = 0
    self.player_y = PLAYER_START_Y * 1.0
    self.player_vel = JUMP
    self.pipes = []
    self.last_pipe = self.fc
    self.last_plant = if self.fc >= 4 { self.fc - 4 } else { 0 }
    self.last_flap = true
    self.rocci = flap_anim(self.fc)
    self.ground_x = 0
    flap_tone()
  }

  enter_over() {
    self.mode = "over"
    self.new_high = self.max_score > self.high_score
    if self.new_high { self.high_score = self.max_score }
    self.rocci = fall_anim(self.fc)
    self.hiscore = hiscore_anim(self.fc)
    death_tone()
  }

  # The "flap / start" action: A, up, or left-click held this frame.
  action_held() {
    let m = Canvas.mouse()
    self.input.down(Canvas.A) || self.input.down(Canvas.UP) || (m.buttons & 1) != 0
  }
  # In-game flap and title start read the action, with a simple autopilot when
  # input is not interactive so a CI / headless run still plays the game.
  flap_input() {
    if self.interactive { self.action_held() } else { self.player_vel > 0.0 && self.player_y > 70.0 }
  }
  start_input() {
    if self.interactive { self.action_held() } else { true }
  }
  restart_input() {
    if self.interactive {
      let m = Canvas.mouse()
      self.input.down(Canvas.B) || self.input.down(Canvas.RIGHT) || (m.buttons & 2) != 0
    } else { true }
  }

  tick() {
    Canvas.clear(C1)
    self.input.update()
    self.fc = self.fc + 1
    if self.mode == "title" { self.tick_title() }
    else if self.mode == "game" { self.tick_game() }
    else { self.tick_over() }
    true
  }

  idle_shift() {
    let a = self.rocci
    if a.index == 2 { 0 }
    else if a.index == 1 && self.fc - a.last_updated > 3 { 0 }
    else { 1 }
  }

  flap_allowed() {
    let a = self.rocci
    if a.index == 2 { true }
    else if a.index == 1 { self.fc - a.last_updated > 6 }
    else { false }
  }

  tick_title() {
    self.rocci.update(self.fc)
    Canvas.text("Rocci Bird!!!", 32, 12, C4)
    Canvas.text("Click to start!", 24, 72, C4)
    draw_ground(0)
    draw_plants(self.plants)
    self.rocci.draw(PLAYER_X, PLAYER_START_Y + self.idle_shift())
    if self.start_input() { self.enter_game() }
  }

  tick_game() {
    let flap = self.flap_input()
    if !self.last_flap && flap && self.flap_allowed() {
      flap_tone()
      self.player_vel = JUMP
      self.rocci.reset(self.fc, 0, "runonce")
    } else {
      self.player_vel = self.player_vel + GRAVITY
      self.rocci.update(self.fc)
    }
    self.last_flap = flap

    # pipes: spawn, move (scoring when a pipe crosses PLAYER_X - 2), cull
    if self.fc - self.last_pipe > 90 {
      self.pipes.push({ x: W, gap_start: Random.int(0, 16) * 5 + 10 })
      self.last_pipe = self.fc
    }
    mut next_pipes = []
    mut gained = 0
    for pp in self.pipes {
      let nx = pp.x - 1
      if nx == PLAYER_X - 2 { gained = gained + 1 }
      if nx >= -20 { next_pipes.push({ x: nx, gap_start: pp.gap_start }) }
    }
    self.pipes = next_pipes

    # plants: spawn, move, cull
    if self.fc - self.last_plant > 12 {
      self.plants.push({ x: W, type: Random.int(0, PLANT_TYPES) })
      self.last_plant = self.fc
    }
    mut next_plants = []
    for pl in self.plants {
      let nx = pl.x - 1
      if nx >= -12 { next_plants.push({ x: nx, type: pl.type }) }
    }
    self.plants = next_plants

    self.player_y = self.player_y + self.player_vel
    self.score = Math.min(self.score + gained, 255)
    self.max_score = Math.max(self.score, self.max_score)
    self.ground_x = (self.ground_x - 1) % W
    if gained > 0 { point_tone() }

    draw_pipes(self.pipes)
    draw_ground(self.ground_x)
    draw_plants(self.plants)

    let y_pixel = Math.min(self.player_y.to_long(), 134)
    let hit = collided(y_pixel, self.rocci.index)
    self.rocci.draw(PLAYER_X, y_pixel)
    draw_score(self.score, 68, 4)

    if hit || self.player_y >= 134.0 { self.enter_over() }
  }

  tick_over() {
    self.player_vel = self.player_vel + GRAVITY
    self.rocci.update(self.fc)
    self.hiscore.update(self.fc)
    self.player_y = Math.min(self.player_y + self.player_vel, 134.0)

    draw_pipes(self.pipes)
    draw_ground(self.ground_x)
    draw_plants(self.plants)
    self.rocci.draw(PLAYER_X, self.player_y.to_long())

    Canvas.rect(16, 52, 136, 32, C1)
    Canvas.text("Game Over!", 44, 56, C4)
    Canvas.text("Right to restart", 20, 72, C4)
    Canvas.text("Art by Luke DeVault", 4, 151, C4)
    Canvas.rect(66, 2, 28, 12, C1)
    draw_score(self.score, 68, 4)
    if self.new_high { self.hiscore.draw(64, 0) }
    Canvas.rect(54, 18, 52, 12, C1)
    Canvas.text("HS:", 57, 20, C4)
    draw_score(self.high_score, 80, 20)

    if self.restart_input() { self.enter_title() }
  }
}

let app = App.new()
Canvas.run(W, H, fn () { app.tick() })
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
      playTone(msg);
      return;
    }
    if (msg.type === "done") {
      running = false;
      stopRafPump();
      stopBtn.disabled = true;
      runBtn.disabled = false;
      if (!inTui && !inCanvas && output.textContent === "") output.textContent = "(no output)";
      output.classList.toggle("err", msg.rc !== 0);
      if (msg.rc !== 0) {
        markErrorFromOutput();
        switchTab("output");   // a crash mid-TUI/Canvas: show the message, not a frozen frame
      }
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
  ensureAudio();    // this click is a user gesture — unlock audio for any tones
  running = true;
  runBtn.disabled = true;
  stopBtn.disabled = false;
  output.classList.remove("err");
  output.textContent = "";
  editor.clearError();
  term.reset();
  resetToOutput();
  startRafPump();   // drives Canvas present()'s frame wait; harmless otherwise
  setStatus("running…");
  worker.postMessage({ type: "run", src: editor.getValue() });
}

// culebra errors end with `at LINE:COL`; surface the first such location in the
// editor so a failed run points at the offending line, not just red text.
function markErrorFromOutput() {
  const m = output.textContent.match(/\bat (\d+):(\d+)/);
  if (m) editor.setError(parseInt(m[1], 10));
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

// A requestAnimationFrame heartbeat paced to a fixed 60 Hz: each forwarded tick
// lets the worker's suspended present() (self.__nextFrame) resolve. Canvas games
// are frame-count based (WASM-4 heritage — gravity is px/frame at 60fps), so the
// tick rate must NOT follow the display's refresh; on a 120 Hz / ProMotion panel
// that ran the game at 2x. We forward at most one tick per 1/60 s of real time,
// carrying the remainder so the average holds at 60 fps on any refresh rate.
// Started for every run (a plain script never waits on it) and stopped when the
// run ends.
const FRAME_MS = 1000 / 60;
let rafId = null;
let nextTickAt = 0;
function startRafPump() {
  if (rafId !== null) return;
  nextTickAt = 0;
  const pump = (now) => {
    if (nextTickAt === 0) nextTickAt = now;
    if (now >= nextTickAt) {
      if (worker) worker.postMessage({ type: "tick" });
      nextTickAt += FRAME_MS;
      // A stall (backgrounded tab, GC pause) must not bank catch-up ticks that
      // fast-forward the game — resync to now instead of replaying the gap.
      if (now >= nextTickAt) nextTickAt = now + FRAME_MS;
    }
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

// WebAudio implementation of Canvas.tone — a small WASM-4-style APU. A note
// slides start->end frequency under an ADSR envelope on one of four channels:
// two pulse waves with a selectable duty cycle (via a cached PeriodicWave), a
// triangle, and noise (white noise through a sweeping lowpass, which reads as a
// pitched hiss). Channels are monophonic — a new note cuts the previous one on
// the same channel, like the real APU. The context is created lazily so the
// page needs no audio permission until a program actually plays something.
let audioCtx = null;
const activeVoices = [null, null, null, null, null];  // one slot per channel
const pulseWaves = {};  // duty index -> PeriodicWave, built on demand

// Browsers start an AudioContext "suspended" until a user gesture. Create it
// (once) and resume it if suspended — called both from the first click/key
// anywhere on the page and from playTone, so a program that makes noise right
// away isn't silent. A resume() on an already-running context is a no-op.
function ensureAudio() {
  try {
    if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    if (audioCtx.state === "suspended") audioCtx.resume();
  } catch {
    // Audio unavailable (no device / blocked) — a program stays playable.
  }
  return audioCtx;
}
for (const ev of ["pointerdown", "keydown", "touchstart"]) {
  window.addEventListener(ev, ensureAudio, { passive: true });
}

// A pulse wave of the given duty cycle as a PeriodicWave. Sine-series
// coefficients b_n = (2/(n*pi))(1 - cos(2*pi*n*D)); D=0.5 recovers a square.
function pulseWave(dutyIndex) {
  if (pulseWaves[dutyIndex]) return pulseWaves[dutyIndex];
  const D = [0.125, 0.25, 0.5, 0.75][dutyIndex] ?? 0.5;
  const N = 32;
  const real = new Float32Array(N + 1);
  const imag = new Float32Array(N + 1);
  for (let n = 1; n <= N; n++) {
    imag[n] = (2 / (n * Math.PI)) * (1 - Math.cos(2 * Math.PI * n * D));
  }
  const w = audioCtx.createPeriodicWave(real, imag, { disableNormalization: false });
  pulseWaves[dutyIndex] = w;
  return w;
}

// One second of white noise, reused for every noise voice.
let noiseBuffer = null;
function whiteNoise() {
  if (noiseBuffer) return noiseBuffer;
  const buf = audioCtx.createBuffer(1, audioCtx.sampleRate, audioCtx.sampleRate);
  const d = buf.getChannelData(0);
  for (let i = 0; i < d.length; i++) d[i] = Math.random() * 2 - 1;
  noiseBuffer = buf;
  return buf;
}

function playTone(m) {
  try {
    if (!ensureAudio()) return;
    const now = audioCtx.currentTime;
    const F = (frames) => Math.max(0, frames) / 60;  // frames@60fps -> seconds
    const attackT = F(m.attack), decayT = F(m.decay);
    const sustainT = F(m.sustain), releaseT = F(m.release);
    let total = attackT + decayT + sustainT + releaseT;
    if (total <= 0) total = 1 / 60;  // guarantee an audible blip
    const startF = Math.max(1, m.startFreq);
    const endF = Math.max(1, m.endFreq);
    const G = (v) => Math.max(0, Math.min(1, v / 100)) * 0.2;  // keep it gentle
    const peakG = G(m.peak), susG = G(m.vol);
    const channel = m.channel | 0;

    // Cut any note still playing on this channel (monophony).
    const prev = activeVoices[channel];
    if (prev) { try { prev.stop(now); } catch {} }

    const gain = audioCtx.createGain();
    let src;
    if (channel === 3) {  // noise: white noise through a lowpass that sweeps
      src = audioCtx.createBufferSource();
      src.buffer = whiteNoise();
      src.loop = true;
      const lp = audioCtx.createBiquadFilter();
      lp.type = "lowpass";
      lp.frequency.setValueAtTime(startF * 8, now);
      lp.frequency.linearRampToValueAtTime(endF * 8, now + total);
      src.connect(lp).connect(gain);
    } else {
      const osc = audioCtx.createOscillator();
      if (channel === 0 || channel === 1) osc.setPeriodicWave(pulseWave(m.duty | 0));
      else if (channel === 2) osc.type = "triangle";
      else osc.type = "sawtooth";  // channel 4: culebra extension
      osc.frequency.setValueAtTime(startF, now);
      osc.frequency.linearRampToValueAtTime(endF, now + total);
      osc.connect(gain);
      src = osc;
    }

    // ADSR: 0 -> peak (attack) -> sustain vol (decay) -> hold -> 0 (release).
    // Zero-length phases collapse to instant steps at the same instant.
    let t = now;
    gain.gain.setValueAtTime(0.0001, t);
    t += attackT;  gain.gain.linearRampToValueAtTime(Math.max(0.0001, peakG), t);
    t += decayT;   gain.gain.linearRampToValueAtTime(Math.max(0.0001, susG), t);
    t += sustainT; gain.gain.setValueAtTime(Math.max(0.0001, susG), t);
    t += releaseT; gain.gain.linearRampToValueAtTime(0.0001, t);

    gain.connect(audioCtx.destination);
    src.start(now);
    src.stop(now + total);
    activeVoices[channel] = src;
    src.onended = () => { if (activeVoices[channel] === src) activeVoices[channel] = null; };
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
// The right mouse button is a real input (Rocci Bird restarts on it), so
// suppress the context menu over the canvas.
gameCanvas.addEventListener("contextmenu", (e) => e.preventDefault());

// --- boot -----------------------------------------------------------------

editor.setValue(EXAMPLES["Hello"]);
setStatus("loading…");
spawnWorker();
