// Generated from src/preambles/*.cul by misc/gen_preambles.sh — do not edit.
// Edit the .cul sources, then run `just gen-preambles` (CI checks sync).
#pragma once

inline constexpr const char* TIME_MODULE_SOURCE = R"=culpre=(let _time_module = fn () { class Duration { new(nanos) { this._nanos = nanos } seconds() { to_float(this._nanos) / 1000000000.0 } milliseconds() { to_float(this._nanos) / 1000000.0 } minutes() { this.seconds() / 60.0 } hours() { this.seconds() / 3600.0 } days() { this.seconds() / 86400.0 } abs() { if this._nanos < 0 { Duration.new(-this._nanos) } else { Duration.new(this._nanos) } } __add__(o) { Duration.new(this._nanos + o._nanos) } __sub__(o) { Duration.new(this._nanos - o._nanos) } __mul__(n) { Duration.new(to_long(to_float(this._nanos) * to_float(n))) } __div__(n) { Duration.new(to_long(to_float(this._nanos) / to_float(n))) } __neg__() { Duration.new(-this._nanos) } __lt__(o) { this._nanos < o._nanos } __le__(o) { this._nanos <= o._nanos } __eq__(o) { this._nanos == o._nanos } }; class Instant { new(nanos) { this._nanos = nanos } iso(utc = true) { _Time.iso_nanos(this._nanos, utc) } format(fmt, utc = false) { _Time.format_nanos(this._nanos, fmt, utc) } parts(utc = false) { _Time.parts_nanos(this._nanos, utc) } weekday(utc = false) { _Time.weekday_nanos(this._nanos, utc) } add(years = 0, months = 0, days = 0, hours = 0, minutes = 0, seconds = 0, utc = false) { Instant.new(_Time.add_nanos(this._nanos, years, months, days, hours, minutes, seconds, utc)) } start_of(unit, utc = false) { Instant.new(_Time.start_of_nanos(this._nanos, unit, utc)) } unix() { to_float(this._nanos) / 1000000000.0 } unix_nanos() { this._nanos } __add__(o) { Instant.new(this._nanos + o._nanos) } __sub__(o) { match o { d: Duration => Instant.new(this._nanos - d._nanos), i: Instant  => Duration.new(this._nanos - i._nanos) } } __lt__(o) { this._nanos < o._nanos } __le__(o) { this._nanos <= o._nanos } __eq__(o) { this._nanos == o._nanos } }; { now: fn() { Instant.new(_Time.now_nanos()) }, monotonic: fn() { _Time.monotonic() }, sleep: fn(secs) { _Time.sleep(secs) }, from_iso: fn(s) { Instant.new(_Time.from_iso_nanos(s)) }, from_unix: fn(secs) { Instant.new(to_long(to_float(secs) * 1000000000.0)) }, from_parts: fn(p, utc = false) { Instant.new(_Time.from_parts_nanos(p, utc)) }, parse: fn(s, fmt) { Instant.new(_Time.parse_nanos(s, fmt)) }, seconds: fn(n) { Duration.new(to_long(to_float(n) * 1000000000.0)) }, milliseconds: fn(n) { Duration.new(to_long(to_float(n) * 1000000.0)) }, minutes: fn(n) { Duration.new(to_long(to_float(n) * 60000000000.0)) }, hours: fn(n) { Duration.new(to_long(to_float(n) * 3600000000000.0)) }, days: fn(n) { Duration.new(to_long(to_float(n) * 86400000000000.0)) }, Instant: Instant, Duration: Duration } }; let Time = _time_module()
)=culpre=";

inline constexpr const char* TERM_MODULE_SOURCE = R"=culpre=(
let _term_module = fn () {
  # Input is a single event model: poll() returns one of these Objects (or
  # nil for no input). `kind` discriminates; modifiers are booleans.
  #   { kind: "key",    key, ctrl, shift, alt }      # key = name or character
  #   { kind: "mouse",  event, button, x, y, ctrl, shift, alt }
  #   { kind: "resize", cols, rows }
  let _evkey = fn (name, ctrl, shift, alt) { { kind: "key", key: name, ctrl: ctrl, shift: shift, alt: alt } }
  # An xterm modifier number: (m - 1) is a bitmask 1=shift 2=alt 4=ctrl.
  let _mods = fn (m) { let k = m - 1; ((k & 1) != 0, (k & 2) != 0, (k & 4) != 0) }
  let _csi_keys = { "A": "up", "B": "down", "C": "right", "D": "left",
                    "H": "home", "F": "end", "P": "f1", "Q": "f2", "R": "f3", "S": "f4" }
  let _tilde_keys = { "1": "home", "2": "insert", "3": "delete", "4": "end",
                      "5": "pageup", "6": "pagedown", "11": "f1", "12": "f2",
                      "13": "f3", "14": "f4", "15": "f5", "17": "f6", "18": "f7",
                      "19": "f8", "20": "f9", "21": "f10", "23": "f11", "24": "f12" }
  let _ctrl_letters = "abcdefghijklmnopqrstuvwxyz"

  # Parse an SGR mouse report "\x1b[<b;x;yM/m" into a mouse Object (0-based).
  let _parse_mouse = fn (raw) {
    let n = raw.size()
    let last = raw[n - 1..n]
    let parts = raw[3..n - 1].split(";")   # drop "\x1b[<" and the final M/m
    if parts.size() != 3 { return nil }
    let b = to_long(parts[0])
    let x = to_long(parts[1]) - 1
    let y = to_long(parts[2]) - 1
    mut button = "none"
    mut event = if last == "M" { "press" } else { "release" }
    if (b & 64) != 0 {
      button = if (b & 1) == 0 { "wheel_up" } else { "wheel_down" }
      event = "scroll"
    } else {
      let low = b & 3
      button = if low == 0 { "left" } else { if low == 1 { "middle" } else { if low == 2 { "right" } else { "none" } } }
      if (b & 32) != 0 { event = "drag" }
    }
    { kind: "mouse", event: event, button: button, x: x, y: y,
      shift: (b & 4) != 0, alt: (b & 8) != 0, ctrl: (b & 16) != 0 }
  }

  # Parse one raw report into an event Object, or nil.
  let _parse_event = fn (raw) {
    let n = raw.size()
    if n == 0 { return nil }
    if n >= 3 && raw[0..3] == "\x1b[<" { return _parse_mouse(raw) }
    if raw[0..1] == "\x1b" {
      if n == 1 { return _evkey("escape", false, false, false) }
      let second = raw[1..2]
      if second == "[" || second == "O" {
        let body = raw[2..n]                            # after "\x1b[" / "\x1bO"
        let bn = body.size()
        let final = body[bn - 1..bn]
        mut shift = false
        mut alt = false
        mut ctrl = false
        mut numpart = body[0..bn - 1]                   # params before final
        if numpart.size() > 0 {
          let ps = numpart.split(";")
          numpart = ps[0]
          if ps.size() == 2 { let m = _mods(to_long(ps[1])); shift = m[0]; alt = m[1]; ctrl = m[2] }
        }
        let name = if final == "~" { _tilde_keys.get(numpart, "") } else { _csi_keys.get(final, "") }
        if name != "" { return _evkey(name, ctrl, shift, alt) }
        return nil                                       # unrecognized sequence
      }
      if n == 2 { return _evkey(raw[1..2], false, false, true) }   # ESC+char = alt+char
      return nil
    }
    if n == 1 {
      if raw == "\r" || raw == "\n" { return _evkey("enter", false, false, false) }
      if raw == "\t" { return _evkey("tab", false, false, false) }
      if raw == "\x7f" || raw == "\x08" { return _evkey("backspace", false, false, false) }
      let cp = raw.code_points().collect()[0]
      if cp >= 1 && cp <= 26 { return _evkey(_ctrl_letters[cp - 1..cp], true, false, false) }   # ctrl+letter
      return _evkey(raw, false, false, false)            # printable
    }
    _evkey(raw, false, false, false)                     # multi-byte character
  }

  # A pending resize wins; otherwise parse one input report. Returns nil for
  # no input.
  let _poll = fn (timeout) {
    if _Term.resized() { return { kind: "resize", cols: _Term.cols(), rows: _Term.rows() } }
    _parse_event(_Term.read_key(timeout))
  }
  # Colour capability (0 none / 1 16 / 2 256 / 3 truecolour), auto-detected
  # and overridable via Term.set_level. Colours downsample to this level.
  mut _level = _Term.color_level()
  let _rgb256 = fn (r, g, b) {
    if r == g && g == b {
      if r < 8 { 16 } else { if r > 248 { 231 } else { 232 + (r - 8) * 24 / 247 } }
    } else { 16 + 36 * (r * 5 / 255) + 6 * (g * 5 / 255) + (b * 5 / 255) }
  }
  let _rgb16 = fn (r, g, b) {
    let bright = if r > 170 || g > 170 || b > 170 { 8 } else { 0 }
    bright + (if r > 110 { 1 } else { 0 }) + (if g > 110 { 2 } else { 0 }) + (if b > 110 { 4 } else { 0 })
  }
  let _idx_rgb = fn (n) {
    if n < 232 {
      let i = n - 16
      let conv = fn (c) { if c == 0 { 0 } else { 55 + c * 40 } }
      (conv(i / 36), conv((i % 36) / 6), conv(i % 6))
    } else { let g = 8 + (n - 232) * 10; (g, g, g) }
  }
  # SGR parameter fragments (no escape wrapper), already downsampled to the
  # active level — "" means "no colour at this level". `Term.style` joins
  # these with attributes; the wrapping helpers below add `\x1b[..m`/reset.
  let _16fg = fn (i) { to_string(if i < 8 { 30 + i } else { 90 + i - 8 }) }
  let _16bg = fn (i) { to_string(if i < 8 { 40 + i } else { 100 + i - 8 }) }
  let _fg_params = fn (n) {
    if _level >= 2 { "38;5;" + to_string(n) }
    else { if _level == 1 { if n < 16 { _16fg(n) } else { let c = _idx_rgb(n); _16fg(_rgb16(c[0], c[1], c[2])) } } else { "" } }
  }
  let _bg_params = fn (n) {
    if _level >= 2 { "48;5;" + to_string(n) }
    else { if _level == 1 { if n < 16 { _16bg(n) } else { let c = _idx_rgb(n); _16bg(_rgb16(c[0], c[1], c[2])) } } else { "" } }
  }
  let _rgbfg_params = fn (r, g, b) {
    if _level >= 3 { "38;2;" + to_string(r) + ";" + to_string(g) + ";" + to_string(b) }
    else { if _level == 2 { "38;5;" + to_string(_rgb256(r, g, b)) } else { if _level == 1 { _16fg(_rgb16(r, g, b)) } else { "" } } }
  }
  let _rgbbg_params = fn (r, g, b) {
    if _level >= 3 { "48;2;" + to_string(r) + ";" + to_string(g) + ";" + to_string(b) }
    else { if _level == 2 { "48;5;" + to_string(_rgb256(r, g, b)) } else { if _level == 1 { _16bg(_rgb16(r, g, b)) } else { "" } } }
  }
  # Wrap text in `\x1b[<params>m ... \x1b[<reset>m` (passthrough when empty).
  let _wrap = fn (s, params, reset) { if params == "" { s } else { "\x1b[" + params + "m" + s + "\x1b[" + reset + "m" } }
  let _named = fn (s, i) { if _level == 0 { s } else { _wrap(s, _16fg(i), "39") } }
  let _attr = fn (s, on, off) { if _level == 0 { s } else { "\x1b[" + on + "m" + s + "\x1b[" + off + "m" } }
  # Build an SGR parameter string for a cell style. fg/bg take a 256-colour
  # index (Long) or an (r,g,b) tuple; attrs are booleans. "" at level 0.
  let _style = fn (fg = nil, bg = nil, bold = false, dim = false, underline = false, reverse = false) {
    if _level == 0 { return "" }
    mut parts = []
    if bold { parts.push("1") }
    if dim { parts.push("2") }
    if underline { parts.push("4") }
    if reverse { parts.push("7") }
    if fg != nil {
      let p = if type_of(fg) == "Tuple" || type_of(fg) == "Array" { _rgbfg_params(fg[0], fg[1], fg[2]) } else { _fg_params(fg) }
      if p != "" { parts.push(p) }
    }
    if bg != nil {
      let p = if type_of(bg) == "Tuple" || type_of(bg) == "Array" { _rgbbg_params(bg[0], bg[1], bg[2]) } else { _bg_params(bg) }
      if p != "" { parts.push(p) }
    }
    parts.join(";")
  }
  # A double-buffered grid of cells (glyph + optional SGR style). clear()/
  # set()/put() build the back buffer; flush() emits only the cells that
  # differ from the last frame (cursor-move + minimal SGR transition + glyph),
  # so updates do not flicker. Styles come from `Term.style(...)`; wide glyphs
  # occupy two cells. Glyph and style are kept in parallel arrays so reuse
  # stays alloc-free (see clear()).
  class Screen {
    new() { this._w = 0; this._h = 0; this._back = []; this._front = []; this._bstyle = []; this._fstyle = [] }
    cols() { _Term.cols() }
    rows() { _Term.rows() }
    size() { (_Term.cols(), _Term.rows()) }
    clear() {
      let w = _Term.cols()
      let h = _Term.rows()
      let n = w * h
      this._w = w
      this._h = h
      # Reuse the buffers on the common path (size unchanged); only reallocate
      # when the terminal was resized.
      if this._back.size() == n {
        mut i = 0
        while i < n { this._back[i] = " "; this._bstyle[i] = ""; i = i + 1 }
      } else {
        this._back = []
        this._bstyle = []
        for _ in 0..n { this._back.push(" "); this._bstyle.push("") }
      }
      this
    }
    set(x, y, g, style = "") {
      let gs = to_string(g)   # graphemes()/slices yield StringView
      if x >= 0 && x < this._w && y >= 0 && y < this._h {
        let idx = y * this._w + x
        this._back[idx] = gs
        this._bstyle[idx] = style
        if _Term.width(gs) == 2 && x + 1 < this._w { this._back[idx + 1] = ""; this._bstyle[idx + 1] = style }
      }
      this
    }
    put(x, y, s, style = "") {
      mut cx = x
      for g in s.graphemes() {
        this.set(cx, y, to_string(g), style)
        # set() marks the next cell "" for a wide glyph; reuse that instead of
        # measuring the width again.
        let wide = cx + 1 < this._w && this._back[cx + 1] == ""
        cx = cx + (if wide { 2 } else { 1 })
      }
      this
    }
    # Minimal escape string to turn the displayed frame into the built one;
    # updates the front buffer. Empty when nothing changed. Returned (not
    # printed) so it is testable; flush() prints it.
    render() {
      let n = this._w * this._h
      mut out = ""
      if this._front.size() != n {
        this._front = []
        this._fstyle = []
        for _ in 0..n { this._front.push("\x00"); this._fstyle.push("") }   # force a full repaint
        out = "\x1b[2J"                                                     # wipe stale content
      }
      mut cy = -1
      mut cx = -1
      mut pen = ""   # SGR currently applied at the terminal ("" = default)
      mut y = 0
      while y < this._h {
        mut x = 0
        while x < this._w {
          let idx = y * this._w + x
          let back = this._back[idx]
          if back == "" {
            this._front[idx] = ""
            this._fstyle[idx] = this._bstyle[idx]
          } else {
            let st = this._bstyle[idx]
            if back != this._front[idx] || st != this._fstyle[idx] {
              if cy != y || cx != x { out = out + "\x1b[" + to_string(y + 1) + ";" + to_string(x + 1) + "H" }
              if st != pen {
                out = out + "\x1b[0m"
                if st != "" { out = out + "\x1b[" + st + "m" }
                pen = st
              }
              out = out + back
              this._front[idx] = back
              this._fstyle[idx] = st
              # A wide glyph is the one whose continuation cell set() blanked.
              let wide = x + 1 < this._w && this._back[idx + 1] == ""
              cy = y
              if wide { cx = x + 2; this._front[idx + 1] = ""; this._fstyle[idx + 1] = st } else { cx = x + 1 }
            }
          }
          x = x + 1
        }
        y = y + 1
      }
      if pen != "" { out = out + "\x1b[0m" }   # leave the terminal at default
      out
    }
    flush() { IO.print(this.render()); _Term.flush(); this }
    poll(timeout) { _poll(timeout) }
  }
  {
    cols: fn () { _Term.cols() },
    rows: fn () { _Term.rows() },
    size: fn () { (_Term.cols(), _Term.rows()) },
    clear: fn () { "\x1b[2J\x1b[H" },
    move: fn (x, y) { "\x1b[" + to_string(y + 1) + ";" + to_string(x + 1) + "H" },
    hide: fn () { "\x1b[?25l" },
    show: fn () { "\x1b[?25h" },
    flush: fn () { _Term.flush() },
    fg: fn (s, n) { _wrap(s, _fg_params(n), "39") },
    bg: fn (s, n) { _wrap(s, _bg_params(n), "49") },
    rgb: fn (s, r, g, b) { _wrap(s, _rgbfg_params(r, g, b), "39") },
    style: _style,
    bold: fn (s) { _attr(s, "1", "22") },
    dim: fn (s) { _attr(s, "2", "22") },
    underline: fn (s) { _attr(s, "4", "24") },
    reverse: fn (s) { _attr(s, "7", "27") },
    black: fn (s) { _named(s, 0) },
    red: fn (s) { _named(s, 1) },
    green: fn (s) { _named(s, 2) },
    yellow: fn (s) { _named(s, 3) },
    blue: fn (s) { _named(s, 4) },
    magenta: fn (s) { _named(s, 5) },
    cyan: fn (s) { _named(s, 6) },
    white: fn (s) { _named(s, 7) },
    level: fn () { _level },
    set_level: fn (n) { _level = n },
    parse: fn (raw) { _parse_event(raw) },           # raw report -> Event | nil
    mouse_on: fn () { "\x1b[?1002h\x1b[?1006h" },    # button + drag, SGR coords
    mouse_off: fn () { "\x1b[?1002l\x1b[?1006l" },
    width: fn (s) { _Term.width(s) },
    resized: fn () { _Term.resized() },
    poll: fn (timeout) { _poll(timeout) },
    app: fn (body, mouse = false) {
      _Term.raw_on()
      IO.print("\x1b[?1049h\x1b[?25l\x1b[0m" + (if mouse { "\x1b[?1002h\x1b[?1006h" } else { "" }))
      _Term.flush()
      defer {
        IO.print((if mouse { "\x1b[?1002l\x1b[?1006l" } else { "" }) + "\x1b[?25h\x1b[?1049l\x1b[0m")
        _Term.flush()
        _Term.raw_off()
      }
      body(Screen.new())
    },
    Screen: Screen,
  }
}
let Term = _term_module()
)=culpre=";

inline constexpr const char* ARGS_MODULE_SOURCE = R"=culpre=(let _args_module = fn() { let _coerce = fn(raw, type, name) { if type == "String" { return raw }; if type == "Long" { return to_long(raw) }; if type == "Float" { return to_float(raw) }; if type == "Bool" { if raw == "true" || raw == "1" { return true }; if raw == "false" || raw == "0" { return false }; throw {kind: "ArgParseError", message: "argument '{name}' expects Bool, got '{raw}'"} }; throw {kind: "ArgParseError", message: "argument '{name}' has unknown type '{type}'"} }; let _find_by_name = fn(args, name) { let mut i = 0; while i < args.size() { let a = args[i]; if a.name == name { return a }; if a.has("short") && a.short == name { return a }; i += 1 }; nil }; let _is_option = fn(a) { a.has("short") || a.has("default") }; let _is_positional = fn(a) { !_is_option(a) }; let _arg_type = fn(a) { if a.has("type") { a.type } else { "String" } }; let _format_help = fn(spec) { let name = if spec.has("name") { spec.name } else { "program" }; let doc = if spec.has("doc") { spec.doc } else { "" }; let mut parts = []; if doc != "" { parts.push("{name} - {doc}\n\n") }; let mut pos_args = []; let mut opt_args = []; let mut i = 0; while i < spec.args.size() { let a = spec.args[i]; if _is_positional(a) { pos_args.push(a) } else { opt_args.push(a) }; i += 1 }; parts.push("Usage: {name}"); if opt_args.size() > 0 { parts.push(" [options]") }; let mut j = 0; while j < pos_args.size() { let a = pos_args[j]; if a.has("default") { parts.push(" [<{a.name}>]") } else if a.has("repeated") && a.repeated { parts.push(" <{a.name}>...") } else { parts.push(" <{a.name}>") }; j += 1 }; parts.push("\n"); if pos_args.size() > 0 { parts.push("\nArguments:\n"); let mut k = 0; while k < pos_args.size() { let a = pos_args[k]; let d = if a.has("doc") { a.doc } else { "" }; parts.push("  {a.name}    {d}\n"); k += 1 } }; if opt_args.size() > 0 { parts.push("\nOptions:\n"); let mut m = 0; while m < opt_args.size() { let a = opt_args[m]; let short = if a.has("short") { "-{a.short}, " } else { "    " }; let d = if a.has("doc") { a.doc } else { "" }; parts.push("  {short}--{a.name}    {d}\n"); m += 1 } }; parts.push("  -h, --help    show this help and exit\n"); parts.join("") }; let _route_subcommand = fn(argv, spec) { if !spec.has("subcommands") { return nil }; let mut i = 0; while i < argv.size() { let tok = argv[i]; if tok == "-h" || tok == "--help" { throw {kind: "ArgParseHelp", help: _format_help(spec)} }; if tok.starts_with("-") { i += 1; continue }; let mut j = 0; while j < spec.subcommands.size() { let sub = spec.subcommands[j]; if sub.name == tok { let mut rest = []; let mut k = 0; while k < argv.size() { if k != i { rest.push(argv[k]) }; k += 1 }; return {sub: sub, argv: rest} }; j += 1 }; throw {kind: "ArgParseError", message: "unknown subcommand '{tok}'"} }; throw {kind: "ArgParseError", message: "expected subcommand"} }; let _parse_impl_flat = fn(argv, spec) { let mut result = {}; let mut positionals = []; let mut i = 0; let n = argv.size(); while i < n { let tok = argv[i]; if tok == "--" { let mut j = i + 1; while j < n { positionals.push(argv[j]); j += 1 }; i = n } else if tok == "-h" || tok == "--help" { throw {kind: "ArgParseHelp", help: _format_help(spec)} } else if tok.starts_with("--") { let body = tok.slice(2, tok.size()); let mut name = body; let mut explicit_value = nil; let mut has_value = false; if body.contains("=") { let parts = body.split("="); name = parts[0]; let mut v = parts[1]; let mut pi = 2; while pi < parts.size() { v = "{v}={parts[pi]}"; pi += 1 }; explicit_value = v; has_value = true }; let spec_a = _find_by_name(spec.args, name); if spec_a == nil { throw {kind: "ArgParseError", message: "unknown option '--{name}'"} }; if _arg_type(spec_a) == "Bool" && !has_value { result[spec_a.name] = true } else { let raw = if has_value { explicit_value } else { i = i + 1; if i >= n { throw {kind: "ArgParseError", message: "option '--{name}' expects a value"} }; argv[i] }; let v = _coerce(raw, _arg_type(spec_a), spec_a.name); if spec_a.has("repeated") && spec_a.repeated { if !result.has(spec_a.name) { result[spec_a.name] = [] }; result[spec_a.name].push(v) } else { result[spec_a.name] = v } } } else if tok.starts_with("-") && tok.size() > 1 { let body = tok.slice(1, tok.size()); let mut name = body; let mut explicit_value = nil; let mut has_value = false; if body.contains("=") { let parts = body.split("="); name = parts[0]; let mut v = parts[1]; let mut pi = 2; while pi < parts.size() { v = "{v}={parts[pi]}"; pi += 1 }; explicit_value = v; has_value = true }; let spec_a = _find_by_name(spec.args, name); if spec_a == nil { throw {kind: "ArgParseError", message: "unknown option '-{name}'"} }; if _arg_type(spec_a) == "Bool" && !has_value { result[spec_a.name] = true } else { let raw = if has_value { explicit_value } else { i = i + 1; if i >= n { throw {kind: "ArgParseError", message: "option '-{name}' expects a value"} }; argv[i] }; let v = _coerce(raw, _arg_type(spec_a), spec_a.name); if spec_a.has("repeated") && spec_a.repeated { if !result.has(spec_a.name) { result[spec_a.name] = [] }; result[spec_a.name].push(v) } else { result[spec_a.name] = v } } } else { positionals.push(tok) }; i += 1 }; let mut pos_idx = 0; let mut spec_idx = 0; while spec_idx < spec.args.size() { let a = spec.args[spec_idx]; if _is_positional(a) { if a.has("repeated") && a.repeated { result[a.name] = []; while pos_idx < positionals.size() { let v = _coerce(positionals[pos_idx], _arg_type(a), a.name); result[a.name].push(v); pos_idx += 1 } } else if pos_idx < positionals.size() { result[a.name] = _coerce(positionals[pos_idx], _arg_type(a), a.name); pos_idx += 1 } }; spec_idx += 1 }; if pos_idx < positionals.size() { throw {kind: "ArgParseError", message: "unexpected positional argument '{positionals[pos_idx]}'"} }; let mut k = 0; while k < spec.args.size() { let a = spec.args[k]; if !result.has(a.name) { if a.has("default") { result[a.name] = a.default } else if a.has("repeated") && a.repeated { result[a.name] = [] } else if _arg_type(a) == "Bool" { result[a.name] = false } else { throw {kind: "ArgParseError", message: "missing required argument '{a.name}'"} } }; k += 1 }; result }; let _parse_impl = fn(argv, spec) { let routed = _route_subcommand(argv, spec); if routed != nil { let mut result = _parse_impl_flat(routed.argv, routed.sub); result.subcommand = routed.sub.name; return result }; _parse_impl_flat(argv, spec) }; { try_parse: fn(argv, spec) { _parse_impl(argv, spec) }, parse: fn(argv, spec) { try { _parse_impl(argv, spec) } catch e { if e.has("kind") && e.kind == "ArgParseHelp" { IO.puts(e.help); Sys.exit(0) }; IO.print("error: "); IO.puts(if e.has("message") { e.message } else { e }); Sys.exit(2) } }, help: fn(spec) { _format_help(spec) } } }; let Args = _args_module()
)=culpre=";

inline constexpr const char* MATCHERS_MODULE_SOURCE = R"=culpre=(let assert_true = fn(x) { if x { return nil }; throw {kind: "AssertionError", message: "assert_true failed:\n  value: {x}"} }; let assert_false = fn(x) { if !x { return nil }; throw {kind: "AssertionError", message: "assert_false failed:\n  value: {x}"} }; let assert_eq = fn(a, b) { if a == b { return nil }; throw {kind: "AssertionError", message: "assert_eq failed:\n  left:  {a}\n  right: {b}"} }; let assert_ne = fn(a, b) { if a != b { return nil }; throw {kind: "AssertionError", message: "assert_ne failed:\n  left:  {a}\n  right: {b}"} }; let assert_lt = fn(a, b) { if a < b { return nil }; throw {kind: "AssertionError", message: "assert_lt failed:\n  left:  {a}\n  right: {b}"} }; let assert_le = fn(a, b) { if a <= b { return nil }; throw {kind: "AssertionError", message: "assert_le failed:\n  left:  {a}\n  right: {b}"} }; let assert_gt = fn(a, b) { if a > b { return nil }; throw {kind: "AssertionError", message: "assert_gt failed:\n  left:  {a}\n  right: {b}"} }; let assert_ge = fn(a, b) { if a >= b { return nil }; throw {kind: "AssertionError", message: "assert_ge failed:\n  left:  {a}\n  right: {b}"} }; let assert_throws = fn(kind, f) { if f.params.size() != 0 { throw {kind: "ArityError", message: "assert_throws: fn must take 0 parameters (got {f.params.size()})"} }; let mut threw = false; let mut actual_kind = ""; try { f() } catch e { threw = true; actual_kind = if type_of(e) == "Object" && e.has("kind") { e.kind } else { type_of(e) } }; if !threw { throw {kind: "AssertionError", message: "assert_throws('{kind}', fn): expected throw but fn returned normally"} }; if actual_kind != kind { throw {kind: "AssertionError", message: "assert_throws: expected kind '{kind}' but got '{actual_kind}'"} } }; let assert_close = fn(a, b, tol) { let mut diff = a - b; if diff < 0 { diff = -diff }; if diff != diff || tol != tol || diff > tol { throw {kind: "AssertionError", message: "assert_close failed:\n  a:    {a}\n  b:    {b}\n  diff: {diff} (> tol {tol})"} } }
)=culpre=";

inline constexpr const char* REGEX_MODULE_SOURCE = R"=culpre=(fn _regex_find_iter(pat, s) { let mut pos = 0; while pos <= s.size() { let r = _Regex.find_from(pat, s, pos); if r.m == nil { return }; yield r.m; pos = r.nxt } }; fn _regex_escape(s) { let metas = `\.^$|?*+()[]{}`; let mut out = ""; for c in s { if metas.contains(c) { out = out + `\` + c } else { out = out + c } }; out }; fn _regex_interp(x) { if type_of(x) == "Object" && x.has("class") && x["class"] == "Regex" { "(?:" + x._pat + ")" } else { _regex_escape("{x}") } }; let _regex_module = fn () { class Regex { new(pattern) { this._pat = pattern; _Regex.check(pattern) } test(s) { _Regex.test(this._pat, s) } find(s) { _Regex.find(this._pat, s) } match(s) { _Regex.match(this._pat, s) } find_all(s) { _Regex.find_all(this._pat, s) } find_all_str(s) { _Regex.find_all_str(this._pat, s) } find_all_index(s) { _Regex.find_all_index(this._pat, s) } count(s) { _Regex.count(this._pat, s) } find_iter(s) { _regex_find_iter(this._pat, s) } replace_all(s, repl) { if type_of(repl) != "Function" { return _Regex.replace_all(this._pat, s, repl) }; let mut out = ""; let mut last = 0; for m in _Regex.find_all(this._pat, s) { out = out + s.slice(last, m.start) + repl(m); last = m.end }; out + s.slice(last, s.size()) } split(s) { _Regex.split(this._pat, s) } }; { compile: fn(pattern, flags = "") { Regex.new(if flags == "" { pattern } else { "(?" + flags + ")" + pattern }) }, escape: _regex_escape, interp: _regex_interp, find: fn(pattern, s) { _Regex.find(pattern, s) }, match: fn(pattern, s) { _Regex.match(pattern, s) }, find_all: fn(pattern, s) { _Regex.find_all(pattern, s) }, test: fn(pattern, s) { _Regex.test(pattern, s) }, split: fn(pattern, s) { _Regex.split(pattern, s) }, replace_all: fn(pattern, s, repl) { Regex.new(pattern).replace_all(s, repl) }, Regex: Regex } }; let Regex = _regex_module()
)=culpre=";

inline constexpr const char* STRING_REPLACE_MODULE_SOURCE = R"=culpre=(let replace = fn(s, pat, repl) { if type_of(pat) == "String" { s.split(pat).join(repl) } else { pat.replace_all(s, repl) } }
)=culpre=";

inline constexpr const char* LOG_MODULE_SOURCE = R"=culpre=(let _log_module = fn () { let _levels = {debug: 0, info: 1, warn: 2, error: 3}; let mut _threshold = _levels.get(Sys.env("LOG_LEVEL"), 1); let mut _format = if Sys.env("LOG_FORMAT") == "json" { "json" } else { "text" }; let _colors = {debug: "\x1b[2m", info: "\x1b[32m", warn: "\x1b[33m", error: "\x1b[31m"}; let _emit = fn (name, num, msg, bound, fields) { if num >= _threshold { let _all = {...bound, ...fields}; let ts = _Time.iso_nanos(_Time.now_nanos(), true); if _format == "json" { IO.eprint(JSON.stringify({..._all, time: ts, level: name, msg: msg}) + "\n"); } else { let mut lvl = name; if IO.stderr_is_terminal() { lvl = _colors.get(name, "") + name + "\x1b[0m"; }; let mut line = ts + " " + lvl + " " + msg; for k, v in _all { line = line + " " + k + "=" + to_string(v); }; IO.eprint(line + "\n"); }; } }; let _methods = fn (bound) { {debug: fn (msg, fields = {}) { _emit("debug", 0, msg, bound, fields) }, info: fn (msg, fields = {}) { _emit("info", 1, msg, bound, fields) }, warn: fn (msg, fields = {}) { _emit("warn", 2, msg, bound, fields) }, error: fn (msg, fields = {}) { _emit("error", 3, msg, bound, fields) }, with: fn (more) { _methods({...bound, ...more}) }} }; let _set_level = fn (l) { let n = _levels.get(l, -1); if n < 0 { throw "Log.set_level: unknown level '" + l + "'" }; _threshold = n; }; let _set_format = fn (f) { if f != "text" { if f != "json" { throw "Log.set_format: unknown format '" + f + "'" } }; _format = f; }; {..._methods({}), set_level: _set_level, set_format: _set_format} }; let Log = _log_module()
)=culpre=";

inline constexpr const char* DESKTOP_MODULE_SOURCE = R"=culpre=(let _desktop_module = fn () {
  # A Tauri-shaped desktop facade: local HTTP server + native WebView + assets,
  # in one call.
  let run = fn(config) {
    let port = config.get("port", 8731)
    let base = "http://127.0.0.1:" + port.to_string()

    let srv = Http.server()
    if config.has("assets") { srv.static("/", config["assets"]) }
    if config.has("routes") { config["routes"](srv) }
    srv.post("/__quit", fn(req) { Webview.Window.quit(); "" })
    srv.listen_async(port, workers: config.get("workers", 4))

    let w = Webview.Window.new()
    w.set_title(config.get("title", "culebra"))
    if config.has("size") {
      let size = config["size"]
      w.set_size(size[0], size[1])
    }
    w.navigate(base + "/")
    w.run()
    srv.stop()
  }

  let quit = fn() { Webview.Window.quit() }

  { run: run, quit: quit }
};
let Desktop = _desktop_module()
)=culpre=";

inline constexpr const char* PATH_MODULE_SOURCE = R"=culpre=(# Path — a thin, immutable sugar layer over the FS.* path helpers. Every
# operation returns a fresh Path (paths are never mutated in place), matching
# FS which always takes and returns raw String paths. FS is the primitive
# layer; Path is the fluent layer that carries the path around so callers stop
# threading path strings by hand (Python os.path vs pathlib, same two tiers).
#
# `_s` is the PathLike coercion used *inside* the class: String stays as-is, a
# Path collapses to its inner string via __str__ (honored by to_string). So
# every method accepts either a String or another Path wherever a path is
# expected.
let _path_module = fn() {
  let _s = fn(o) { to_string(o) }
  class Path {
    new(p) { this._path = _s(p) }

    # --- display / conversion ---
    __str__() { this._path }         # "{p}" and to_string(p) yield the raw path
    str() { this._path }             # explicit String escape hatch
    __eq__(o) { this._path == _s(o) }

    # --- joining: `base / "sub" / "leaf"` ---
    join(o) { Path.new(FS.join(this._path, _s(o))) }
    __div__(o) { this.join(o) }

    # --- path components (String, mirroring FS.*) ---
    name() { FS.basename(this._path) }    # final component, e.g. "content.js"
    stem() { FS.stem(this._path) }        # final component without suffix
    suffix() { FS.extension(this._path) } # extension incl. dot, e.g. ".js"
    parent() { Path.new(FS.dirname(this._path)) }

    # --- queries ---
    exists() { FS.exists(this._path) }
    is_file() { FS.is_file(this._path) }
    is_dir() { FS.is_dir(this._path) }

    # --- filesystem ops (delegate to FS) ---
    read() { FS.read(this._path) }
    write(content) { FS.write(this._path, content) }
    mkdir() { FS.mkdir(this._path) }               # creates parents (FS.mkdir does)
    remove(recursive = false) { FS.remove(this._path, recursive: recursive) }
    rename(dst) { FS.rename(this._path, _s(dst)); Path.new(_s(dst)) }

    # --- normalization ---
    resolve() { Path.new(FS.abspath(this._path)) }

    # --- directory listing / globbing (return Path, so chains stay Path) ---
    list() { FS.list_dir(this._path).map(fn(e) { Path.new(FS.join(this._path, e)) }) }
    glob(pattern) { FS.glob(FS.join(this._path, pattern)).map(fn(p) { Path.new(p) }) }
    walk() { FS.walk(this._path).map(fn(p) { Path.new(p) }) }
  }
  Path
}
let Path = _path_module()
)=culpre=";

