// Generated from src/preambles/*.cul by misc/gen_preambles.sh — do not edit.
// Edit the .cul sources, then run `just gen-preambles` (CI checks sync).
#pragma once

inline constexpr const char* TIME_MODULE_SOURCE = R"=culpre=(let _time_module = fn () {
  let _tname = fn (o) {
    let t = type_of(o)
    if t == "Object" && o.has("class") {
      o.class
    } else {
      t
    }
  }
  let _type_error = fn (want, got) {
    throw {
      kind: "TypeError",
      message: "type error: expected {want}, got {_tname(got)}",
    }
  }
  class Duration {
    new(nanos) {
      self._nanos = nanos
    }
    seconds() {
      to_float(self._nanos) / 1000000000.0
    }
    milliseconds() {
      to_float(self._nanos) / 1000000.0
    }
    minutes() {
      self.seconds() / 60.0
    }
    hours() {
      self.seconds() / 3600.0
    }
    days() {
      self.seconds() / 86400.0
    }
    abs() {
      if self._nanos < 0 {
        Duration.new(-self._nanos)
      } else {
        Duration.new(self._nanos)
      }
    }
    __add__(o) {
      let n = match o {
        d: Duration => d._nanos,
      }
      _type_error("Duration", o) if n == nil
      Duration.new(self._nanos + n)
    }
    __sub__(o) {
      let n = match o {
        d: Duration => d._nanos,
      }
      _type_error("Duration", o) if n == nil
      Duration.new(self._nanos - n)
    }
    __mul__(n) {
      Duration.new(to_long(to_float(self._nanos) * to_float(n)))
    }
    __div__(n) {
      Duration.new(to_long(to_float(self._nanos) / to_float(n)))
    }
    __neg__() {
      Duration.new(-self._nanos)
    }
    __lt__(o) {
      let n = match o {
        d: Duration => d._nanos,
      }
      _type_error("Duration", o) if n == nil
      self._nanos < n
    }
    __le__(o) {
      let n = match o {
        d: Duration => d._nanos,
      }
      _type_error("Duration", o) if n == nil
      self._nanos <= n
    }
    __eq__(o) {
      let n = match o {
        d: Duration => d._nanos,
      }
      n != nil && self._nanos == n
    }
  }
  class Instant {
    new(nanos) {
      self._nanos = nanos
    }
    iso(utc = true) {
      _Time.iso_nanos(self._nanos, utc)
    }
    format(fmt, utc = false) {
      _Time.format_nanos(self._nanos, fmt, utc)
    }
    parts(utc = false) {
      _Time.parts_nanos(self._nanos, utc)
    }
    weekday(utc = false) {
      _Time.weekday_nanos(self._nanos, utc)
    }
    add(
      years = 0,
      months = 0,
      days = 0,
      hours = 0,
      minutes = 0,
      seconds = 0,
      utc = false,
    ) {
      Instant.new(_Time.add_nanos(
        self._nanos,
        years,
        months,
        days,
        hours,
        minutes,
        seconds,
        utc,
      ))
    }
    start_of(unit, utc = false) {
      Instant.new(_Time.start_of_nanos(self._nanos, unit, utc))
    }
    unix() {
      to_float(self._nanos) / 1000000000.0
    }
    unix_nanos() {
      self._nanos
    }
    __add__(o) {
      let n = match o {
        d: Duration => d._nanos,
      }
      _type_error("Duration", o) if n == nil
      Instant.new(self._nanos + n)
    }
    __sub__(o) {
      let r = match o {
        d: Duration => Instant.new(self._nanos - d._nanos),
        i: Instant => Duration.new(self._nanos - i._nanos),
      }
      _type_error("Duration or Instant", o) if r == nil
      r
    }
    __lt__(o) {
      let n = match o {
        i: Instant => i._nanos,
      }
      _type_error("Instant", o) if n == nil
      self._nanos < n
    }
    __le__(o) {
      let n = match o {
        i: Instant => i._nanos,
      }
      _type_error("Instant", o) if n == nil
      self._nanos <= n
    }
    __eq__(o) {
      let n = match o {
        i: Instant => i._nanos,
      }
      n != nil && self._nanos == n
    }
  }
  {
    now: fn () {
      Instant.new(_Time.now_nanos())
    },
    monotonic: fn () {
      _Time.monotonic()
    },
    sleep: fn (secs) {
      _Time.sleep(secs)
    },
    from_iso: fn (s) {
      Instant.new(_Time.from_iso_nanos(s))
    },
    from_unix: fn (secs) {
      Instant.new(to_long(to_float(secs) * 1000000000.0))
    },
    from_parts: fn (p, utc = false) {
      Instant.new(_Time.from_parts_nanos(p, utc))
    },
    parse: fn (s, fmt) {
      Instant.new(_Time.parse_nanos(s, fmt))
    },
    seconds: fn (n) {
      Duration.new(to_long(to_float(n) * 1000000000.0))
    },
    milliseconds: fn (n) {
      Duration.new(to_long(to_float(n) * 1000000.0))
    },
    minutes: fn (n) {
      Duration.new(to_long(to_float(n) * 60000000000.0))
    },
    hours: fn (n) {
      Duration.new(to_long(to_float(n) * 3600000000000.0))
    },
    days: fn (n) {
      Duration.new(to_long(to_float(n) * 86400000000000.0))
    },
    Instant: Instant,
    Duration: Duration,
  }
}
let Time = _time_module()
)=culpre=";

inline constexpr const char* TERM_MODULE_SOURCE = R"=culpre=(let _term_module = fn () {
  # Input is a single event model: poll() returns one of these Objects (or
  # nil for no input). `kind` discriminates; modifiers are booleans.
  #   { kind: "key",    key, ctrl, shift, alt }      # key = name or character
  #   { kind: "mouse",  event, button, x, y, ctrl, shift, alt }
  #   { kind: "resize", cols, rows }
  let _evkey = fn (name, ctrl, shift, alt) {
    {kind: "key", key: name, ctrl: ctrl, shift: shift, alt: alt}
  }
  # An xterm modifier number: (m - 1) is a bitmask 1=shift 2=alt 4=ctrl.
  let _mods = fn (m) {
    let k = m - 1
    (k & 1 != 0, k & 2 != 0, k & 4 != 0)
  }
  let _csi_keys = {
    "A": "up",
    "B": "down",
    "C": "right",
    "D": "left",
    "H": "home",
    "F": "end",
    "P": "f1",
    "Q": "f2",
    "R": "f3",
    "S": "f4",
  }
  let _tilde_keys = {
    "1": "home",
    "2": "insert",
    "3": "delete",
    "4": "end",
    "5": "pageup",
    "6": "pagedown",
    "11": "f1",
    "12": "f2",
    "13": "f3",
    "14": "f4",
    "15": "f5",
    "17": "f6",
    "18": "f7",
    "19": "f8",
    "20": "f9",
    "21": "f10",
    "23": "f11",
    "24": "f12",
  }
  let _ctrl_letters = "abcdefghijklmnopqrstuvwxyz"

  # Parse an SGR mouse report "\x1b[<b;x;yM/m" into a mouse Object (0-based).
  let _parse_mouse = fn (raw) {
    let n = raw.size()
    let last = raw[n - 1..n]
    let parts = raw[3..n - 1].split(";")  # drop "\x1b[<" and the final M/m
    return nil if parts.size() != 3
    let b = to_long(parts[0])
    let x = to_long(parts[1]) - 1
    let y = to_long(parts[2]) - 1
    mut button = "none"
    mut event = if last == "M" {
      "press"
    } else {
      "release"
    }
    if b & 64 != 0 {
      button = if b & 1 == 0 {
        "wheel_up"
      } else {
        "wheel_down"
      }
      event = "scroll"
    } else {
      let low = b & 3
      button = if low == 0 {
        "left"
      } else {
        if low == 1 {
          "middle"
        } else {
          if low == 2 {
            "right"
          } else {
            "none"
          }
        }
      }
      event = "drag" if b & 32 != 0
    }
    {
      kind: "mouse",
      event: event,
      button: button,
      x: x,
      y: y,
      shift: b & 4 != 0,
      alt: b & 8 != 0,
      ctrl: b & 16 != 0,
    }
  }

  # Parse one raw report into an event Object, or nil.
  let _parse_event = fn (raw) {
    let n = raw.size()
    return nil if n == 0
    return _parse_mouse(raw) if n >= 3 && raw[0..3] == "\x1b[<"
    if raw[0..1] == "\x1b" {
      return _evkey("escape", false, false, false) if n == 1
      let second = raw[1..2]
      if second == "[" || second == "O" {
        let body = raw[2..n]  # after "\x1b[" / "\x1bO"
        let bn = body.size()
        let final = body[bn - 1..bn]
        mut shift = false
        mut alt = false
        mut ctrl = false
        mut numpart = body[0..bn - 1]  # params before final
        if numpart.size() > 0 {
          let ps = numpart.split(";")
          numpart = ps[0]
          if ps.size() == 2 {
            let m = _mods(to_long(ps[1]))
            shift = m[0]
            alt = m[1]
            ctrl = m[2]
          }
        }
        let name = if final == "~" {
          _tilde_keys.get(numpart, "")
        } else {
          _csi_keys.get(final, "")
        }
        return _evkey(name, ctrl, shift, alt) if name != ""
        return nil  # unrecognized sequence
      }
      if n == 2 {
        return _evkey(raw[1..2], false, false, true)
      }  # ESC+char = alt+char
      return nil
    }
    if n == 1 {
      return _evkey("enter", false, false, false) if raw == "\r" || raw == "\n"
      return _evkey("tab", false, false, false) if raw == "\t"
      if raw == "\x7f" || raw == "\x08" {
        return _evkey("backspace", false, false, false)
      }
      let cp = raw.code_points().collect()[0]
      if cp >= 1 && cp <= 26 {
        return _evkey(_ctrl_letters[cp - 1..cp], true, false, false)
      }                                        # ctrl+letter
      return _evkey(raw, false, false, false)  # printable
    }
    _evkey(raw, false, false, false)  # multi-byte character
  }

  # A pending resize wins; otherwise parse one input report. Returns nil for
  # no input.
  let _poll = fn (timeout) {
    if _Term.resized() {
      return {kind: "resize", cols: _Term.cols(), rows: _Term.rows()}
    }
    _parse_event(_Term.read_key(timeout))
  }
  # Colour capability (0 none / 1 16 / 2 256 / 3 truecolour), auto-detected
  # and overridable via Term.set_level. Colours downsample to this level.
  mut _level = _Term.color_level()
  let _rgb256 = fn (r, g, b) {
    if r == g && g == b {
      if r < 8 {
        16
      } else {
        if r > 248 {
          231
        } else {
          232 + (r - 8) * 24 / 247
        }
      }
    } else {
      16 + 36 * (r * 5 / 255) + 6 * (g * 5 / 255) + b * 5 / 255
    }
  }
  let _rgb16 = fn (r, g, b) {
    let bright = if r > 170 || g > 170 || b > 170 {
      8
    } else {
      0
    }
    bright + if r > 110 {
      1
    } else {
      0
    } + if g > 110 {
      2
    } else {
      0
    } + if b > 110 {
      4
    } else {
      0
    }
  }
  let _idx_rgb = fn (n) {
    if n < 232 {
      let i = n - 16
      let conv = fn (c) {
        if c == 0 {
          0
        } else {
          55 + c * 40
        }
      }
      (conv(i / 36), conv((i % 36) / 6), conv(i % 6))
    } else {
      let g = 8 + (n - 232) * 10
      (g, g, g)
    }
  }
  # SGR parameter fragments (no escape wrapper), already downsampled to the
  # active level — "" means "no colour at this level". `Term.style` joins
  # these with attributes; the wrapping helpers below add `\x1b[..m`/reset.
  # Foreground and background differ only by ground: the 16-colour bases are
  # 30/90 vs 40/100 (each +10) and the extended-colour introducer is 38 vs 48.
  # Both grounds share one downsample ladder so a fix to it can't reach only one.
  let _16sgr = fn (i, base) {
    to_string(if i < 8 {
      base + i
    } else {
      base + 60 + i - 8
    })
  }
  let _16fg = fn (i) {
    _16sgr(i, 30)
  }
  let _idx_params = fn (n, base, ext) {
    if _level >= 2 {
      ext + ";5;" + to_string(n)
    } else {
      if _level == 1 {
        if n < 16 {
          _16sgr(n, base)
        } else {
          let c = _idx_rgb(n)
          _16sgr(_rgb16(c[0], c[1], c[2]), base)
        }
      } else {
        ""
      }
    }
  }
  let _rgb_params = fn (r, g, b, base, ext) {
    if _level >= 3 {
      ext + ";2;" + to_string(r) + ";" + to_string(g) + ";" + to_string(b)
    } else {
      if _level == 2 {
        ext + ";5;" + to_string(_rgb256(r, g, b))
      } else {
        if _level == 1 {
          _16sgr(_rgb16(r, g, b), base)
        } else {
          ""
        }
      }
    }
  }
  let _fg_params = fn (n) {
    _idx_params(n, 30, "38")
  }
  let _bg_params = fn (n) {
    _idx_params(n, 40, "48")
  }
  let _rgbfg_params = fn (r, g, b) {
    _rgb_params(r, g, b, 30, "38")
  }
  let _rgbbg_params = fn (r, g, b) {
    _rgb_params(r, g, b, 40, "48")
  }
  # Wrap text in `\x1b[<params>m ... \x1b[<reset>m` (passthrough when empty).
  let _wrap = fn (s, params, reset) {
    if params == "" {
      s
    } else {
      "\x1b[" + params + "m" + s + "\x1b[" + reset + "m"
    }
  }
  let _named = fn (s, i) {
    if _level == 0 {
      s
    } else {
      _wrap(s, _16fg(i), "39")
    }
  }
  let _attr = fn (s, on, off) {
    if _level == 0 {
      s
    } else {
      "\x1b[" + on + "m" + s + "\x1b[" + off + "m"
    }
  }
  # Build an SGR parameter string for a cell style. fg/bg take a 256-colour
  # index (Long) or an (r,g,b) tuple; attrs are booleans. "" at level 0.
  let _style = fn (
    fg = nil,
    bg = nil,
    bold = false,
    dim = false,
    underline = false,
    reverse = false,
  ) {
    return "" if _level == 0
    mut parts = []
    parts.push("1") if bold
    parts.push("2") if dim
    parts.push("4") if underline
    parts.push("7") if reverse
    if fg != nil {
      let p = if type_of(fg) == "Tuple" || type_of(fg) == "Array" {
        _rgbfg_params(fg[0], fg[1], fg[2])
      } else {
        _fg_params(fg)
      }
      parts.push(p) if p != ""
    }
    if bg != nil {
      let p = if type_of(bg) == "Tuple" || type_of(bg) == "Array" {
        _rgbbg_params(bg[0], bg[1], bg[2])
      } else {
        _bg_params(bg)
      }
      parts.push(p) if p != ""
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
    new() {
      self._w = 0
      self._h = 0
      self._fw = 0
      self._back = []
      self._front = []
      self._bstyle = []
      self._fstyle = []
    }
    cols() {
      _Term.cols()
    }
    rows() {
      _Term.rows()
    }
    size() {
      (_Term.cols(), _Term.rows())
    }
    clear() {
      let w = _Term.cols()
      let h = _Term.rows()
      let n = w * h
      self._w = w
      self._h = h
      # Reuse the buffers on the common path (size unchanged); only reallocate
      # when the terminal was resized.
      if self._back.size() == n {
        for i in 0..n {
          self._back[i] = " "
          self._bstyle[i] = ""
        }
      } else {
        self._back = []
        self._bstyle = []
        for _ in 0..n {
          self._back.push(" ")
          self._bstyle.push("")
        }
      }
      self
    }
    set(x, y, g, style = "") {
      let gs = to_string(g)  # graphemes()/slices yield StringView
      if x >= 0 && x < self._w && y >= 0 && y < self._h {
        let idx = y * self._w + x
        self._back[idx] = gs
        self._bstyle[idx] = style
        if x + 1 < self._w {
          # "" marks the right half of a wide glyph, so it must be written and
          # taken back with its owner: a narrow glyph landing here frees the
          # cell the previous wide one held.
          if _Term.width(gs) == 2 {
            self._back[idx + 1] = ""
            self._bstyle[idx + 1] = style
          } else if self._back[idx + 1] == "" {
            self._back[idx + 1] = " "
            self._bstyle[idx + 1] = style
          }
        }
      }
      self
    }
    put(x, y, s, style = "") {
      mut cx = x
      for g in s.graphemes() {
        let gs = to_string(g)
        self.set(cx, y, gs, style)
        # Advance by the glyph's own display width, the same measure set() uses
        # to blank the continuation cell. Reading that cell back instead misses
        # the row offset and breaks whenever the draw was clipped.
        cx = cx + if _Term.width(gs) == 2 {
          2
        } else {
          1
        }
      }
      self
    }
    # Minimal escape string to turn the displayed frame into the built one;
    # updates the front buffer. Empty when nothing changed. Returned (not
    # printed) so it is testable; flush() prints it.
    render() {
      let n = self._w * self._h
      mut out = ""
      # A reshape that keeps the cell count (80x24 -> 48x40) moves every cell to
      # a new column, so the width has to invalidate the front buffer too.
      if self._front.size() != n || self._fw != self._w {
        self._front = []
        self._fstyle = []
        for _ in 0..n {
          self._front.push("\x00")
          self._fstyle.push("")
        }  # force a full repaint
        self._fw = self._w
        out = "\x1b[2J"  # wipe stale content
      }
      mut cy = -1
      mut cx = -1
      mut pen = ""  # SGR currently applied at the terminal ("" = default)
      for y in 0..self._h {
        for x in 0..self._w {
          let idx = y * self._w + x
          let back = self._back[idx]
          if back == "" {
            self._front[idx] = ""
            self._fstyle[idx] = self._bstyle[idx]
          } else {
            let st = self._bstyle[idx]
            # A wide glyph owns the cell to its right, so it is stale when that
            # cell shows anything but a continuation — an overlapping draw wrote
            # there last frame and only the owner can paint over it.
            let half_lost = x + 1 < self._w &&
              self._back[idx + 1] == "" &&
              self._front[idx + 1] != ""
            if back != self._front[idx] || st != self._fstyle[idx] || half_lost {
              if cy != y || cx != x {
                out = out +
                  "\x1b[" +
                  to_string(y + 1) +
                  ";" +
                  to_string(x + 1) +
                  "H"
              }
              if st != pen {
                out = out + "\x1b[0m"
                out = out + "\x1b[" + st + "m" if st != ""
                pen = st
              }
              out = out + back
              self._front[idx] = back
              self._fstyle[idx] = st
              # Advance the tracked cursor by the glyph's real display width, the
              # same measure set() uses to blank the continuation cell. Keying off
              # the neighbour cell instead under-counts when overlapping draws have
              # since filled it, drifting cx and stranding a later cell's erase.
              let wide = x + 1 < self._w && _Term.width(back) == 2
              cy = y
              if wide {
                cx = x + 2
                self._front[idx + 1] = ""
                self._fstyle[idx + 1] = st
              } else {
                cx = x + 1
              }
            }
          }
        }
      }
      if pen != "" {
        out = out + "\x1b[0m"
      }  # leave the terminal at default
      out
    }
    flush() {
      IO.print(self.render())
      _Term.flush()
      self
    }
    poll(timeout) {
      _poll(timeout)
    }
  }
  {
    cols: fn () {
      _Term.cols()
    },
    rows: fn () {
      _Term.rows()
    },
    size: fn () {
      (_Term.cols(), _Term.rows())
    },
    clear: fn () {
      "\x1b[2J\x1b[H"
    },
    move: fn (x, y) {
      "\x1b[" + to_string(y + 1) + ";" + to_string(x + 1) + "H"
    },
    hide: fn () {
      "\x1b[?25l"
    },
    show: fn () {
      "\x1b[?25h"
    },
    flush: fn () {
      _Term.flush()
    },
    fg: fn (s, n) {
      _wrap(s, _fg_params(n), "39")
    },
    bg: fn (s, n) {
      _wrap(s, _bg_params(n), "49")
    },
    rgb: fn (s, r, g, b) {
      _wrap(s, _rgbfg_params(r, g, b), "39")
    },
    style: _style,
    bold: fn (s) {
      _attr(s, "1", "22")
    },
    dim: fn (s) {
      _attr(s, "2", "22")
    },
    underline: fn (s) {
      _attr(s, "4", "24")
    },
    reverse: fn (s) {
      _attr(s, "7", "27")
    },
    black: fn (s) {
      _named(s, 0)
    },
    red: fn (s) {
      _named(s, 1)
    },
    green: fn (s) {
      _named(s, 2)
    },
    yellow: fn (s) {
      _named(s, 3)
    },
    blue: fn (s) {
      _named(s, 4)
    },
    magenta: fn (s) {
      _named(s, 5)
    },
    cyan: fn (s) {
      _named(s, 6)
    },
    white: fn (s) {
      _named(s, 7)
    },
    level: fn () {
      _level
    },
    set_level: fn (n) {
      _level = n
    },
    parse: fn (raw) {
      _parse_event(raw)
    },  # raw report -> Event | nil
    mouse_on: fn () {
      "\x1b[?1002h\x1b[?1006h"
    },  # button + drag, SGR coords
    mouse_off: fn () {
      "\x1b[?1002l\x1b[?1006l"
    },
    width: fn (s) {
      _Term.width(s)
    },
    resized: fn () {
      _Term.resized()
    },
    attach_tty: fn () {
      _Term.attach_tty()
    },
    poll: fn (timeout) {
      _poll(timeout)
    },
    app: fn (body, mouse = false) {
      _Term.raw_on()
      IO.print("\x1b[?1049h\x1b[?25l\x1b[0m" + if mouse {
        "\x1b[?1002h\x1b[?1006h"
      } else {
        ""
      })
      _Term.flush()
      defer {
        IO.print(if mouse {
          "\x1b[?1002l\x1b[?1006l"
        } else {
          ""
        } + "\x1b[?25h\x1b[?1049l\x1b[0m")
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

inline constexpr const char* CANVAS_MODULE_SOURCE = R"=culpre=(let _canvas_module = fn () {
  # Pack r,g,b,a (each 0..255) into one Long, byte order [r,g,b,a] — exactly
  # what the browser's putImageData reads, so no repacking at present(). This
  # is the colour every Canvas call takes.
  let rgba = fn (r, g, b, a = 255) {
    r + g * 256 + b * 65536 + a * 16777216
  }

  # RGB (each channel 0..255) to HSV (each of h, s, v in 0.0..1.0) — the usual
  # transform for deriving a palette from a few base colours: boost saturation,
  # narrow a light/dark pair toward each other, shift hue. Channel-scale (not
  # 0..1) input so it composes directly with rgba's own r/g/b arguments.
  let rgb_to_hsv = fn (r, g, b) {
    # Normalize to 0..1 before any ratio, matching the textbook (and every
    # other colorsys-style) computation order: a ratio computed on the raw
    # 0..255 values and one computed on 0..1 values are mathematically the
    # same, but not bit-for-bit — the /255 shouldn't be deferred, or a hue
    # that lands exactly on a sector boundary can round to the wrong side.
    let rf = r / 255.0
    let gf = g / 255.0
    let bf = b / 255.0
    let hi = Math.max(rf, gf, bf)
    let lo = Math.min(rf, gf, bf)
    return (0.0, 0.0, hi) if lo == hi
    let span = hi - lo
    let rc = (hi - rf) / span
    let gc = (hi - gf) / span
    let bc = (hi - bf) / span
    let h6 = if rf == hi {
      bc - gc
    } else if gf == hi {
      2.0 + rc - bc
    } else {
      4.0 + gc - rc
    }
    let h = h6 / 6.0
    (h - Math.floor(h), span / hi, hi)
  }

  # The inverse: HSV (each 0.0..1.0) to RGB (each channel rounded to 0..255).
  let hsv_to_rgb = fn (h, s, v) {
    if s == 0.0 {
      let g = Math.round(v * 255)
      return (g, g, g)
    }
    let sector = (h * 6.0).to_long() % 6
    let f = h * 6.0 - (h * 6.0).to_long().to_float()
    let p = v * (1.0 - s)
    let q = v * (1.0 - s * f)
    let t = v * (1.0 - s * (1.0 - f))
    let (r, g, b) = match sector {
      0 => (v, t, p),
      1 => (q, v, p),
      2 => (p, v, t),
      3 => (p, q, v),
      4 => (t, p, v),
      _ => (v, p, q),
    }
    (Math.round(r * 255), Math.round(g * 255), Math.round(b * 255))
  }

  # hsv is to hsv_to_rgb + rgba what rgba is to its own three channels: the
  # one-call way to get a packed colour when hue is what you have.
  let hsv = fn (h, s, v, a = 255) {
    let (r, g, b) = hsv_to_rgb(h, s, v)
    rgba(r, g, b, a)
  }

  # The mirror bits, shared by both blit paths.
  let _flip_bits = fn (flip_x, flip_y) {
    if flip_x {
      1
    } else {
      0
    } + if flip_y {
      2
    } else {
      0
    }
  }

  # Pack the blit transform flags. transpose swaps the X and Y axes — a
  # reflection across the main diagonal (combine it with a flip for a true 90°
  # rotation).
  let _blit_flags = fn (flip_x, flip_y, transpose) {
    _flip_bits(flip_x, flip_y) + if transpose {
      4
    } else {
      0
    }
  }

  # Flags for the scaling blit. There is no transpose, and bit 3 asks for box
  # averaging when the sprite shrinks (ignored when it doesn't).
  let _scale_flags = fn (flip_x, flip_y, smooth) {
    _flip_bits(flip_x, flip_y) + if smooth {
      8
    } else {
      0
    }
  }

  # A registered sprite. `pixels` is a flat row-major array; if `palette` is
  # given they are indices into it (WASM-4-style indexed art), otherwise they
  # are packed-RGBA Longs. Either way the pixels are normalised to RGBA and
  # uploaded once (`_Canvas.sprite_load`); draw() re-references the handle so
  # nothing is re-marshalled per frame.
  class Sprite {
    new(pixels: Array, w: Long, h: Long, palette = nil) {
      let rgba_px = if palette == nil {
        pixels
      } else {
        pixels.map(|i| palette[i])
      }
      self._id = _Canvas.sprite_load(rgba_px, w, h)
      self._w = _Canvas.sprite_width(self._id)
      self._h = _Canvas.sprite_height(self._id)
    }
    # Decode a PNG image (its bytes as a String, e.g. from `FS.read`) and
    # upload it. The size comes from the image, so there is nothing to pass.
    # Raises ValueError on anything that isn't a decodable PNG.
    new(png: String) {
      self._id = _Canvas.sprite_from_png(png)
      self._w = _Canvas.sprite_width(self._id)
      self._h = _Canvas.sprite_height(self._id)
    }
    # A blank w×h sprite in one colour — the raw material of an offscreen
    # draw target (see Canvas.draw_to).
    new(w: Long, h: Long, color = 0) {
      self._id = _Canvas.sprite_blank(w, h, color)
      self._w = w
      self._h = h
    }
    # Named form of the String constructor, for call sites where `Sprite(data)`
    # would not read as "this is a PNG".
    static from_png(data) {
      Sprite(data)
    }
    # Named form of the blank constructor, mirroring from_png.
    static blank(w, h, color = 0) {
      Sprite(w, h, color)
    }
    # Sprites free their native pixels when the last reference drops, so a
    # program that creates them per frame doesn't grow the registry. A
    # constructor that threw (bad PNG bytes) drops before _id was ever set.
    drop() {
      _Canvas.sprite_free(self._id) if self._id != nil
    }
    width() {
      self._w
    }
    height() {
      self._h
    }
    # Blit the whole sprite to (x, y). flip_x/flip_y mirror; transpose swaps
    # X/Y. Transparent (alpha 0) pixels are skipped.
    draw(x, y, flip_x = false, flip_y = false, transpose = false) {
      _Canvas.blit(
        self._id,
        x,
        y,
        0,
        0,
        self._w,
        self._h,
        _blit_flags(flip_x, flip_y, transpose),
      )
    }
    # Blit a sub-rectangle (sx, sy, sw, sh) of the sprite to (x, y) — for sheets.
    draw_sub(
      x,
      y,
      sx,
      sy,
      sw,
      sh,
      flip_x = false,
      flip_y = false,
      transpose = false,
    ) {
      _Canvas.blit(
        self._id,
        x,
        y,
        sx,
        sy,
        sw,
        sh,
        _blit_flags(flip_x, flip_y, transpose),
      )
    }
    # Blit the whole sprite into the w×h rectangle at (x, y), resampling to fit.
    # Sampling is nearest-neighbour (pixel art stays crisp); `smooth` box-averages
    # instead when the sprite shrinks. `alpha` (0..255) composites the whole blit
    # over what is already there — 255 draws it opaque.
    draw_scaled(
      x,
      y,
      w,
      h,
      flip_x = false,
      flip_y = false,
      smooth = false,
      alpha = 255,
    ) {
      _Canvas.blit_scaled(
        self._id,
        x,
        y,
        w,
        h,
        0,
        0,
        self._w,
        self._h,
        _scale_flags(flip_x, flip_y, smooth),
        alpha,
      )
    }
    # draw_scaled from a sub-rectangle (sx, sy, sw, sh) — for sheets.
    draw_sub_scaled(
      x,
      y,
      w,
      h,
      sx,
      sy,
      sw,
      sh,
      flip_x = false,
      flip_y = false,
      smooth = false,
      alpha = 255,
    ) {
      _Canvas.blit_scaled(
        self._id,
        x,
        y,
        w,
        h,
        sx,
        sy,
        sw,
        sh,
        _scale_flags(flip_x, flip_y, smooth),
        alpha,
      )
    }
    # Encode the sprite's pixels as PNG bytes — the inverse of from_png, so
    # `FS.write(path, sprite.to_png())` saves what `Sprite.from_png(FS.read(
    # path))` loads back.
    to_png() {
      _Canvas.sprite_to_png(self._id)
    }
  }

  # A parsed TTF/OTF font (its bytes as a String, e.g. from `FS.read` or
  # `Embed.dir(...).read(...)` -- the latter bakes the font into an AOT
  # binary the same way it already does for Sprite.from_png assets). Unlike
  # Sprite this has no fixed size: size is given per draw call, so one Font
  # serves every size a program uses, and the native side caches rasterized
  # glyphs per (font, codepoint, size). stb_truetype does no bounds-checking
  # against malformed input (unlike the PNG decoder behind Sprite.from_png)
  # -- only load fonts from trusted sources.
  class Font {
    new(data: String) {
      self._id = _Canvas.ttf_load(data)
    }
    # A font's native memory frees when the last reference drops, mirroring
    # Sprite. A constructor that threw (bad font bytes) drops before _id was
    # ever set.
    drop() {
      _Canvas.ttf_free(self._id) if self._id != nil
    }
    # Pixel ascent at `size`: how far the tallest glyph reaches above the
    # baseline. draw()/text_width() use this so (x, y) reads as visual
    # top-left, matching Canvas.text's convention even though the native
    # primitive places glyphs by baseline.
    ascent(size) {
      _Canvas.ttf_ascent(self._id, size)
    }
    advance(codepoint, size) {
      _Canvas.ttf_advance(self._id, codepoint, size)
    }
    # Draw `s` at (x, y) -- visual top-left -- in `color` at `size` px. Every
    # Unicode scalar value in `s` is drawn (full Unicode, unlike the built-in
    # ASCII-only bitmap font): an unmapped codepoint draws stb_truetype's
    # .notdef glyph rather than being skipped. No kerning (v1): advance is
    # the sum of each glyph's own width.
    draw(s, x, y, color, size) {
      let baseline = y + self.ascent(size)
      mut cx = x
      for cp in s.code_points() {
        cx += _Canvas.ttf_glyph(self._id, cp, cx, baseline, color, size)
      }
    }
    # draw(), but rasterized at the size the frame is actually presented at
    # and drawn over it rather than into it. The frame is scaled up with
    # nearest-neighbour pixels -- right for sprites and the bitmap font, but
    # it magnifies a glyph's antialiased edge into blocks; this keeps the edge
    # sharp at any window size or Playground pane width.
    #
    # Same arguments, same coordinates, same units as draw() -- including what
    # text_width() predicts -- so a call switches between the two by name
    # alone. Two differences to know: this layer is cleared every present(),
    # so screen text is redrawn each frame (a `run` tick does that anyway),
    # and Canvas.to_png() does not capture it (to_png reads the draw target;
    # use draw() for text that has to appear in a saved image).
    draw_screen(s, x, y, color, size) {
      let baseline = y + self.ascent(size)
      mut cx = x
      for cp in s.code_points() {
        cx += _Canvas.ttf_glyph_screen(self._id, cp, cx, baseline, color, size)
      }
    }
    # Pixel width `s` will occupy at `size` -- for right-aligning / centring.
    text_width(s, size) {
      mut w = 0
      for cp in s.code_points() {
        w += self.advance(cp, size)
      }
      w
    }
  }

  # --- built-in 8x8 bitmap font -------------------------------------------
  # The WASM-4 runtime font (ISC-licensed, aduros/wasm4), covering printable
  # ASCII 32..126. Each glyph is 8 rows of 8 pixels, one byte per row, MSB the
  # leftmost pixel, and a 0 bit is a lit pixel. It is packed here as hex and
  # unpacked once at module load into a flat byte table indexed by
  # (code - 32) * 8 + row. Advance is a fixed 8px, matching WASM-4's text().
  let _font_hex = "ffffffffffffffffc7c7c7cfcfffcfff939393ffffffffff93019393930193ffef832f83e903efff9d5b37efd9b573ff8f27278f253381ffcfcfcffffffffffff3e7cfcfcfe7f3ff9fcfe7e7e7cf9fffff93c701c793ffffffe7e781e7e7ffffffffffffffcfcf9fffffff81ffffffffffffffffffcfcffffdfbf7efdfbf7fffc7b33939399bc7ffe7c7e7e7e7e781ff8339f1c3871f01ff81f3e7c3f93983ffe3c3933301f3f3ff033f03f9f93983ffc39f3f03393983ff0139f3e7cfcfcfff873b1b87617983ff83393981f9f387ffffcfcfffcfcfffffffcfcfffcfcf9ffff3e7cf9fcfe7f3ffffff01ff01ffffff9fcfe7f3e7cf9fff830139f3c7ffc7ff837d4555417f83ffc7933939013939ff03393903393903ffc3993f3f3f99c3ff07333939393307ff013f3f033f3f01ff013f3f033f3f3fffc19f3f313999c1ff39393901393939ff81e7e7e7e7e781fff9f9f9f9f93983ff3933270f072331ff9f9f9f9f9f9f81ff39110101293939ff39190901213139ff83393939393983ff03393939033f3fff83393939213385ff03393931072331ff87333f83f93983ff81e7e7e7e7e7e7ff39393939393983ff3939391183c7efff39392901011139ff391183c7831139ff999999c3e7e7e7ff01f1e3c78f1f01ffc3cfcfcfcfcfc3ff7fbfdfeff7fbfdff87e7e7e7e7e787ffc793ffffffffffffffffffffffffff01eff7ffffffffffffffff83f9813981ff3f3f0339393983ffffff813f3f3f81fff9f98139393981ffffff8339013f83fff1e781e7e7e7e7ffffff81393981f9833f3f0339393939ffe7ffc7e7e7e781fff3ffe3f3f3f3f3873f3f3103072331ffc7e7e7e7e7e781ffffff0349494949ffffff0339393939ffffff8339393983ffffff033939033f3fffff81393981f9f9ffff918f9f9f9fffffff833f83f903ffe7e781e7e7e7e7ffffff3939393981ffffff999999c3e7ffffff4949494981ffffff3901c70139ffffff39393981f983ffff01e3c78f01fff3e7e7cfe7e7f3ffe7e7e7e7e7e7e7ff9fcfcfe7cfcf9fffffff8f45e3ffffff"
  let _hex_digits = "0123456789abcdef".graphemes().collect()
  let _font_bytes = fn () {
    let chars = _font_hex.graphemes().collect()
    mut out = []
    for i in 0..chars.size() by 2 {
      let hi = _hex_digits.index_of(chars[i])
      let lo = _hex_digits.index_of(chars[i + 1])
      out.push(hi * 16 + lo)
    }
    out
  }()
  let _font_first = 32  # first glyph code (space)
  let _font_last = 126  # last glyph code (~)
  let _char_w = 8
  # Upload the table once; drawing then costs one call per character rather
  # than one per lit pixel (a 42-character HUD line went 5.1 ms -> 0.2 ms).
  let _font_id = _Canvas.font_load(_font_bytes)

  # Draw `s` at (x, y) in `color`, left to right (8 * scale px per glyph, each
  # font pixel a scale x scale block). Characters outside the printable range
  # are skipped (advance still applies), so layout stays stable.
  let text = fn (s, x, y, color, scale = 1) {
    mut cx = x
    for ch in s.graphemes() {
      let code = ch.bytes().collect()[0]
      if code >= _font_first && code <= _font_last {
        _Canvas.glyph(_font_id, code - _font_first, cx, y, color, scale)
      }
      cx = cx + _char_w * scale
    }
  }
  # Pixel width a string will occupy — for right-aligning / centring HUD text.
  let text_width = fn (s, scale = 1) {
    s.graphemes().collect().size() * _char_w * scale
  }

  # --- input --------------------------------------------------------------
  # Button bitmask bits (also mapped by the Playground frontend). A/B are the
  # two action buttons (space / another key); D-pad is arrows.
  let LEFT = 1
  let RIGHT = 2
  let UP = 4
  let DOWN = 8
  let A = 16
  let B = 32

  # Held-button bitmask this frame, and the mouse as an Object.
  let buttons = fn () {
    _Canvas.buttons()
  }
  let mouse = fn () {
    {
      x: _Canvas.mouse_x(),
      y: _Canvas.mouse_y(),
      buttons: _Canvas.mouse_buttons(),
    }
  }

  # Arbitrary keys, in Term.read_key's vocabulary: a printable character
  # ("a", " ", "-") or a special-key name ("left", "enter", "escape", "tab",
  # "backspace", "insert", "delete", "home", "end", "pageup", "pagedown",
  # "f1".."f12"). "space" is accepted as a readable alias for " ".
  # key(name) is the held state now; key_queue() drains this frame's presses
  # (so call it from one place per frame); typed() drains the characters the
  # user typed (shift/layout/IME applied) — for name entry, not movement.
  let key = fn (name) {
    _Canvas.key(if name == "space" {
      " "
    } else {
      name
    })
  }
  let key_queue = fn () {
    mut out = []
    mut k = _Canvas.key_pop()
    while k != "" {
      out.push(k)
      k = _Canvas.key_pop()
    }
    out
  }
  let typed = fn () {
    mut out = ""
    mut c = _Canvas.char_pop()
    while c != "" {
      out = out + c
      c = _Canvas.char_pop()
    }
    out
  }

  # Edge detector: remembers last frame's bitmask so pressed(btn) is true only
  # on the frame a button goes down (the natural "flap" trigger). Call update()
  # once per frame, after reading input.
  class Input {
    new() {
      self._prev = 0
      self._cur = 0
    }
    update() {
      self._prev = self._cur
      self._cur = _Canvas.buttons()
      self
    }
    down(btn) {
      self._cur & btn != 0
    }  # held now
    pressed(btn) {
      self._cur & btn != 0 && self._prev & btn == 0
    }  # just went down
  }

  # --- audio --------------------------------------------------------------
  # Channels, matching WASM-4's APU. The two pulse channels take a duty cycle;
  # SAWTOOTH is a culebra extension (not a WASM-4 channel). These values are the
  # channel codes passed straight through to the host.
  let PULSE = 0   # pulse 1
  let PULSE2 = 1  # pulse 2
  let TRIANGLE = 2
  let NOISE = 3
  let SAWTOOTH = 4
  # Duty cycles for the pulse channels.
  let DUTY_EIGHTH = 0
  let DUTY_QUARTER = 1
  let DUTY_HALF = 2
  let DUTY_THREE_QUARTER = 3

  # Play a tone. In the simple form, tone(freq, dur) is a `dur`-frame note at
  # `freq`. The optional args expose the full WASM-4 envelope: the note slides
  # `freq` -> `end_freq` while an ADSR envelope (attack/decay/release in frames,
  # `dur` is the sustain length) shapes the amplitude from 0 up to `peak`, down
  # to the sustain `vol`, and back to 0. `wave` picks the channel and `duty` the
  # pulse shape. No-op on the headless native backend.
  let tone = fn (
    freq,
    dur,
    vol = 100,
    wave = 0,
    end_freq = nil,
    attack = 0,
    decay = 0,
    release = 0,
    peak = nil,
    duty = 2,
  ) {
    let ef = if end_freq == nil {
      freq
    } else {
      end_freq
    }
    let pk = if peak == nil {
      vol
    } else {
      peak
    }
    _Canvas.tone(freq, ef, attack, decay, dur, release, vol, pk, wave, duty)
  }

  # --- sound effects -------------------------------------------------------
  # A one-shot sample decoded once from its bytes (WAV, MP3 or Ogg — a String,
  # e.g. from FS.read) and played per call. Each Sound is one voice: play()
  # restarts it, like the underlying host samplers. Raises ValueError when the
  # bytes are none of the three formats; silent on the headless backend.
  class Sound {
    new(data: String) {
      self._id = _Canvas.sound_load(data)
    }
    play(vol = 100) {
      _Canvas.sound_play(self._id, vol)
    }
    stop() {
      _Canvas.sound_stop(self._id)
    }
    playing() {
      _Canvas.sound_playing(self._id)
    }
    # Free the decoded sample with the last reference (a constructor that
    # threw drops before _id was ever set).
    drop() {
      _Canvas.sound_free(self._id) if self._id != nil
    }
  }

  # --- music ---------------------------------------------------------------
  # Play an MP3 or Ogg Vorbis file from its bytes (a String, e.g. from
  # FS.read) — one slot, so a new call replaces whatever was playing. `vol` is
  # 0..100 on the same scale as tone; `start` is seconds into the file. The
  # stream is fed from present(), so it only advances while frames are shown.
  # Raises ValueError when the bytes are neither MP3 nor Ogg.
  let music = fn (data, loop = true, vol = 100, start = 0.0) {
    _Canvas.music_play(
      data,
      if loop {
        1
      } else {
        0
      },
      vol,
      start,
    )
  }

  # --- offscreen drawing --------------------------------------------------
  # Redirect drawing into `sprite` for the duration of `f`: the drawing calls,
  # width()/height() and get_pixel all address the sprite; present() still
  # shows the framebuffer. The previous target is restored on every exit path
  # (defer), so a throw inside `f` can't leave drawing redirected. Drawing a
  # sprite onto itself (sprite.draw inside its own draw_to) raises.
  let draw_to = fn (sprite, f) {
    let prev = _Canvas.target(sprite._id)
    defer {
      _Canvas.target(prev)
    }
    f()
  }

  # --- game loop ----------------------------------------------------------
  # Set up a w×h framebuffer and drive `tick` once per frame, presenting after
  # each. `tick()` returns false to stop (e.g. the player quit). present()
  # suspends to the browser's animation frame in the interactive build, so the
  # loop yields cooperatively. Closing the native window (`_Canvas.closing()`,
  # always false on the browser/headless backends) also stops it. Otherwise the
  # loop stops after `frames`, so a run nobody can end can't spin forever —
  # which takes both halves below: a headless run on a tty is interactive and
  # still has nothing to show and no close box.
  let run = fn (w, h, tick, frames = 600) {
    _Canvas.init(w, h)
    let interactive = IO.stdin_is_terminal() && _Canvas.windowed()
    mut i = 0
    mut running = true
    while running {
      let cont = tick()
      _Canvas.present()
      i = i + 1
      running = false if cont == false
      running = false if _Canvas.closing()
      running = false if !interactive && i >= frames
    }
  }

  # --- coordinate methods, scalar + Vector2 overloads --------------------
  # Each is a single `let`-bound fn matching on the first arg's type, not a
  # `fn name(...)` multimethod pair: measured up to ~5x combined overhead
  # from per-call dispatcher-arm scoring and from `fn name(...)`'s wider
  # closure-over-scope cost this deep in the module (2M-call JIT microbench).
  #
  # rect/circle/ellipse/triangle accept `fill` positionally or as `fill:`
  # (tests/test_canvas_module.cul), and *args can't combine with a named
  # default param (docs/language.md) — so each param list is sized to the
  # scalar form's own max arity; the shorter Vector2 form's unused trailing
  # slot catches its own positional `fill` instead (`d ?? fill` / `e ?? fill`
  # below, named for each function's own trailing param).
  let set_pixel = fn (a, b, c = nil) {
    match a {
      p: Vector2 => _Canvas.set_pixel(p.x, p.y, b),
      _ => _Canvas.set_pixel(a, b, c),
    }
  }
  let get_pixel = fn (a, b = nil) {
    match a {
      p: Vector2 => _Canvas.get_pixel(p.x, p.y),
      _ => _Canvas.get_pixel(a, b),
    }
  }
  let line = fn (a, b, c, d = nil, e = nil) {
    match a {
      p1: Vector2 => _Canvas.line(p1.x, p1.y, b.x, b.y, c),
      _ => _Canvas.line(a, b, c, d, e),
    }
  }
  let rect = fn (a, b, c, d, e = nil, fill = true) {
    match a {
      p: Vector2 => _Canvas.rect(p.x, p.y, b, c, d, if e ?? fill {
        1
      } else {
        0
      }),
      _ => _Canvas.rect(a, b, c, d, e, if fill {
        1
      } else {
        0
      }),
    }
  }
  let circle = fn (a, b, c, d = nil, fill = true) {
    match a {
      p: Vector2 => _Canvas.ellipse(p.x, p.y, b, b, c, if d ?? fill {
        1
      } else {
        0
      }),
      _ => _Canvas.ellipse(a, b, c, c, d, if fill {
        1
      } else {
        0
      }),
    }
  }
  let ellipse = fn (a, b, c, d, e = nil, fill = true) {
    match a {
      p: Vector2 => _Canvas.ellipse(p.x, p.y, b, c, d, if e ?? fill {
        1
      } else {
        0
      }),
      _ => _Canvas.ellipse(a, b, c, d, e, if fill {
        1
      } else {
        0
      }),
    }
  }
  let triangle = fn (a, b, c, d, e = nil, f = nil, g = nil, fill = true) {
    match a {
      p1: Vector2 => _Canvas.triangle(p1.x, p1.y, b.x, b.y, c.x, c.y, d, if e ?? fill {
        1
      } else {
        0
      }),
      _ => _Canvas.triangle(a, b, c, d, e, f, g, if fill {
        1
      } else {
        0
      }),
    }
  }

  {
    rgba: rgba,
    rgb_to_hsv: rgb_to_hsv,
    hsv_to_rgb: hsv_to_rgb,
    hsv: hsv,
    # Allocate (or resize) the framebuffer. `run` does this for you; call it
    # directly when you drive the frame loop yourself.
    init: fn (w, h) {
      _Canvas.init(w, h)
    },
    # --- window ---
    # Name it. Call before the loop starts; a later call renames a window
    # already up. No-op where there is no window (headless, browser).
    title: fn (name) {
      _Canvas.title(name)
    },
    clear: fn (color) {
      _Canvas.clear(color)
    },
    # set_pixel/get_pixel/rect/line/circle/ellipse/triangle: multimethod
    # dispatchers declared above (scalar + Vector2 overloads), bound here as
    # first-class Function values like any other field.
    set_pixel: set_pixel,
    get_pixel: get_pixel,
    rect: rect,
    line: line,
    circle: circle,
    ellipse: ellipse,
    triangle: triangle,
    # Polygon from a flat x0, y0, x1, y1, ... vertex list (even-odd rule; the
    # outline closes itself). Each filled row covers [xl, xr), like rect, so
    # polygons sharing an edge tile with no seam.
    polygon: fn (points, color, fill = true) {
      _Canvas.polygon(points, color, if fill {
        1
      } else {
        0
      })
    },
    present: fn () {
      _Canvas.present()
    },
    width: fn () {
      _Canvas.width()
    },
    height: fn () {
      _Canvas.height()
    },
    # PNG bytes for the current draw target — the framebuffer, or the sprite
    # a surrounding draw_to switched to, following width/height/get_pixel.
    to_png: fn () {
      _Canvas.sprite_to_png(0)
    },
    Sprite: Sprite,
    draw_to: draw_to,
    text: text,
    text_width: text_width,
    Font: Font,
    buttons: buttons,
    mouse: mouse,
    key: key,
    key_queue: key_queue,
    typed: typed,
    Input: Input,
    tone: tone,
    Sound: Sound,
    music: music,
    music_stop: fn () {
      _Canvas.music_stop()
    },
    music_pause: fn () {
      _Canvas.music_pause()
    },
    music_resume: fn () {
      _Canvas.music_resume()
    },
    music_volume: fn (vol) {
      _Canvas.music_volume(vol)
    },
    music_seek: fn (seconds) {
      _Canvas.music_seek(seconds)
    },
    music_playing: fn () {
      _Canvas.music_playing()
    },
    run: run,
    LEFT: LEFT,
    RIGHT: RIGHT,
    UP: UP,
    DOWN: DOWN,
    A: A,
    B: B,
    PULSE: PULSE,
    PULSE2: PULSE2,
    TRIANGLE: TRIANGLE,
    SAWTOOTH: SAWTOOTH,
    NOISE: NOISE,
    DUTY_EIGHTH: DUTY_EIGHTH,
    DUTY_QUARTER: DUTY_QUARTER,
    DUTY_HALF: DUTY_HALF,
    DUTY_THREE_QUARTER: DUTY_THREE_QUARTER,
  }
}
let Canvas = _canvas_module()
)=culpre=";

inline constexpr const char* ARGS_MODULE_SOURCE = R"=culpre=(let _args_module = fn () {
  let _coerce = fn (raw, type, name) {
    return raw if type == "String"
    return to_long(raw) if type == "Long"
    return to_float(raw) if type == "Float"
    if type == "Bool" {
      return true if raw == "true" || raw == "1"
      return false if raw == "false" || raw == "0"
      throw {
        kind: "ArgParseError",
        message: "argument '{name}' expects Bool, got '{raw}'",
      }
    }
    throw {
      kind: "ArgParseError",
      message: "argument '{name}' has unknown type '{type}'",
    }
  }
  let _find_by_name = fn (args, name) {
    let mut i = 0
    while i < args.size() {
      let a = args[i]
      return a if a.name == name
      return a if a.has("short") && a.short == name
      i += 1
    }
    nil
  }
  let _is_option = fn (a) {
    a.has("short") || a.has("default")
  }
  let _is_positional = fn (a) {
    !_is_option(a)
  }
  let _arg_type = fn (a) {
    if a.has("type") {
      a.type
    } else {
      "String"
    }
  }
  let _format_help = fn (spec) {
    let name = if spec.has("name") {
      spec.name
    } else {
      "program"
    }
    let doc = if spec.has("doc") {
      spec.doc
    } else {
      ""
    }
    let mut parts = []
    if doc != "" {
      parts.push("{name} - {doc}\n\n")
    }
    let mut pos_args = []
    let mut opt_args = []
    let mut i = 0
    while i < spec.args.size() {
      let a = spec.args[i]
      if _is_positional(a) {
        pos_args.push(a)
      } else {
        opt_args.push(a)
      }
      i += 1
    }
    parts.push("Usage: {name}")
    parts.push(" [options]") if opt_args.size() > 0
    let mut j = 0
    while j < pos_args.size() {
      let a = pos_args[j]
      if a.has("default") {
        parts.push(" [<{a.name}>]")
      } else if a.has("repeated") && a.repeated {
        parts.push(" <{a.name}>...")
      } else {
        parts.push(" <{a.name}>")
      }
      j += 1
    }
    parts.push("\n")
    if pos_args.size() > 0 {
      parts.push("\nArguments:\n")
      let mut k = 0
      while k < pos_args.size() {
        let a = pos_args[k]
        let d = if a.has("doc") {
          a.doc
        } else {
          ""
        }
        parts.push("  {a.name}    {d}\n")
        k += 1
      }
    }
    if opt_args.size() > 0 {
      parts.push("\nOptions:\n")
      let mut m = 0
      while m < opt_args.size() {
        let a = opt_args[m]
        let short = if a.has("short") {
          "-{a.short}, "
        } else {
          "    "
        }
        let d = if a.has("doc") {
          a.doc
        } else {
          ""
        }
        parts.push("  {short}--{a.name}    {d}\n")
        m += 1
      }
    }
    parts.push("  -h, --help    show this help and exit\n")
    parts.join("")
  }
  let _route_subcommand = fn (argv, spec) {
    return nil if !spec.has("subcommands")
    let mut i = 0
    while i < argv.size() {
      let tok = argv[i]
      if tok == "-h" || tok == "--help" {
        throw {kind: "ArgParseHelp", help: _format_help(spec)}
      }
      if tok.starts_with("-") {
        i += 1
        continue
      }
      let mut j = 0
      while j < spec.subcommands.size() {
        let sub = spec.subcommands[j]
        if sub.name == tok {
          let mut rest = []
          let mut k = 0
          while k < argv.size() {
            rest.push(argv[k]) if k != i
            k += 1
          }
          return {sub: sub, argv: rest}
        }
        j += 1
      }
      throw {kind: "ArgParseError", message: "unknown subcommand '{tok}'"}
    }
    throw {kind: "ArgParseError", message: "expected subcommand"}
  }
  let _parse_impl_flat = fn (argv, spec) {
    let mut result = {}
    let mut positionals = []
    let mut i = 0
    let n = argv.size()
    while i < n {
      let tok = argv[i]
      if tok == "--" {
        let mut j = i + 1
        while j < n {
          positionals.push(argv[j])
          j += 1
        }
        i = n
      } else if tok == "-h" || tok == "--help" {
        throw {kind: "ArgParseHelp", help: _format_help(spec)}
      } else if tok.starts_with("--") {
        let body = tok.slice(2, tok.size())
        let mut name = body
        let mut explicit_value = nil
        let mut has_value = false
        if body.contains("=") {
          let parts = body.split("=")
          name = parts[0]
          let mut v = parts[1]
          let mut pi = 2
          while pi < parts.size() {
            v = "{v}={parts[pi]}"
            pi += 1
          }
          explicit_value = v
          has_value = true
        }
        let spec_a = _find_by_name(spec.args, name)
        if spec_a == nil {
          throw {kind: "ArgParseError", message: "unknown option '--{name}'"}
        }
        if _arg_type(spec_a) == "Bool" && !has_value {
          result[spec_a.name] = true
        } else {
          let raw = if has_value {
            explicit_value
          } else {
            i = i + 1
            if i >= n {
              throw {
                kind: "ArgParseError",
                message: "option '--{name}' expects a value",
              }
            }
            argv[i]
          }
          let v = _coerce(raw, _arg_type(spec_a), spec_a.name)
          if spec_a.has("repeated") && spec_a.repeated {
            result[spec_a.name] ??= []
            result[spec_a.name].push(v)
          } else {
            result[spec_a.name] = v
          }
        }
      } else if tok.starts_with("-") && tok.size() > 1 {
        let body = tok.slice(1, tok.size())
        let mut name = body
        let mut explicit_value = nil
        let mut has_value = false
        if body.contains("=") {
          let parts = body.split("=")
          name = parts[0]
          let mut v = parts[1]
          let mut pi = 2
          while pi < parts.size() {
            v = "{v}={parts[pi]}"
            pi += 1
          }
          explicit_value = v
          has_value = true
        }
        let spec_a = _find_by_name(spec.args, name)
        if spec_a == nil {
          throw {kind: "ArgParseError", message: "unknown option '-{name}'"}
        }
        if _arg_type(spec_a) == "Bool" && !has_value {
          result[spec_a.name] = true
        } else {
          let raw = if has_value {
            explicit_value
          } else {
            i = i + 1
            if i >= n {
              throw {
                kind: "ArgParseError",
                message: "option '-{name}' expects a value",
              }
            }
            argv[i]
          }
          let v = _coerce(raw, _arg_type(spec_a), spec_a.name)
          if spec_a.has("repeated") && spec_a.repeated {
            result[spec_a.name] ??= []
            result[spec_a.name].push(v)
          } else {
            result[spec_a.name] = v
          }
        }
      } else {
        positionals.push(tok)
      }
      i += 1
    }
    let mut pos_idx = 0
    let mut spec_idx = 0
    while spec_idx < spec.args.size() {
      let a = spec.args[spec_idx]
      if _is_positional(a) {
        if a.has("repeated") && a.repeated {
          result[a.name] = []
          while pos_idx < positionals.size() {
            let v = _coerce(positionals[pos_idx], _arg_type(a), a.name)
            result[a.name].push(v)
            pos_idx += 1
          }
        } else if pos_idx < positionals.size() {
          result[a.name] = _coerce(positionals[pos_idx], _arg_type(a), a.name)
          pos_idx += 1
        }
      }
      spec_idx += 1
    }
    if pos_idx < positionals.size() {
      throw {
        kind: "ArgParseError",
        message: "unexpected positional argument '{positionals[pos_idx]}'",
      }
    }
    let mut k = 0
    while k < spec.args.size() {
      let a = spec.args[k]
      if !result.has(a.name) {
        if a.has("default") {
          result[a.name] = a.default
        } else if a.has("repeated") && a.repeated {
          result[a.name] = []
        } else if _arg_type(a) == "Bool" {
          result[a.name] = false
        } else {
          throw {
            kind: "ArgParseError",
            message: "missing required argument '{a.name}'",
          }
        }
      }
      k += 1
    }
    result
  }
  let _parse_impl = fn (argv, spec) {
    let routed = _route_subcommand(argv, spec)
    if routed != nil {
      let mut result = _parse_impl_flat(routed.argv, routed.sub)
      result.subcommand = routed.sub.name
      return result
    }
    _parse_impl_flat(argv, spec)
  }
  {
    try_parse: fn (argv, spec) {
      _parse_impl(argv, spec)
    },
    parse: fn (argv, spec) {
      try {
        _parse_impl(argv, spec)
      } catch e {
        if e.has("kind") && e.kind == "ArgParseHelp" {
          IO.print(e.help)
          Sys.exit(0)
        }
        let msg = if e.has("message") {
          e.message
        } else {
          to_string(e)
        }
        IO.eprintln("error: {msg}")
        Sys.exit(2)
      }
    },
    help: fn (spec) {
      _format_help(spec)
    },
  }
}
let Args = _args_module()
)=culpre=";

inline constexpr const char* MATCHERS_MODULE_SOURCE = R"=culpre=(let assert_true = fn (x) {
  return nil if x
  throw {kind: "AssertionError", message: "assert_true failed:\n  value: {x}"}
}
let assert_false = fn (x) {
  return nil if !x
  throw {kind: "AssertionError", message: "assert_false failed:\n  value: {x}"}
}
let assert_eq = fn (a, b) {
  return nil if a == b
  throw {
    kind: "AssertionError",
    message: "assert_eq failed:\n  left:  {a}\n  right: {b}",
  }
}
let assert_ne = fn (a, b) {
  return nil if a != b
  throw {
    kind: "AssertionError",
    message: "assert_ne failed:\n  left:  {a}\n  right: {b}",
  }
}
let assert_lt = fn (a, b) {
  return nil if a < b
  throw {
    kind: "AssertionError",
    message: "assert_lt failed:\n  left:  {a}\n  right: {b}",
  }
}
let assert_le = fn (a, b) {
  return nil if a <= b
  throw {
    kind: "AssertionError",
    message: "assert_le failed:\n  left:  {a}\n  right: {b}",
  }
}
let assert_gt = fn (a, b) {
  return nil if a > b
  throw {
    kind: "AssertionError",
    message: "assert_gt failed:\n  left:  {a}\n  right: {b}",
  }
}
let assert_ge = fn (a, b) {
  return nil if a >= b
  throw {
    kind: "AssertionError",
    message: "assert_ge failed:\n  left:  {a}\n  right: {b}",
  }
}
let assert_throws = fn (kind, f) {
  if f.params.size() != 0 {
    throw {
      kind: "ArityError",
      message: "assert_throws: fn must take 0 parameters (got {f.params.size()})",
    }
  }
  let mut threw = false
  let mut actual_kind = ""
  try {
    f()
  } catch e {
    threw = true
    actual_kind = if type_of(e) == "Object" && e.has("kind") {
      e.kind
    } else {
      type_of(e)
    }
  }
  if !threw {
    throw {
      kind: "AssertionError",
      message: "assert_throws('{kind}', fn): expected throw but fn returned normally",
    }
  }
  if actual_kind != kind {
    throw {
      kind: "AssertionError",
      message: "assert_throws: expected kind '{kind}' but got '{actual_kind}'",
    }
  }
}
let assert_close = fn (a, b, tol) {
  let mut diff = a - b
  diff = -diff if diff < 0
  if diff != diff || tol != tol || diff > tol {
    throw {
      kind: "AssertionError",
      message: "assert_close failed:\n  a:    {a}\n  b:    {b}\n  diff: {diff} (> tol {tol})",
    }
  }
}
)=culpre=";

inline constexpr const char* REGEX_MODULE_SOURCE = R"=culpre=(fn _regex_find_iter(pat, s) {
  let mut pos = 0
  while pos <= s.size() {
    let r = _Regex.find_from(pat, s, pos)
    return if r.m == nil
    yield r.m
    pos = r.nxt
  }
}
fn _regex_escape(s) {
  let metas = `\.^$|?*+()[]{}`
  let mut out = ""
  for c in s {
    if metas.contains(c) {
      out = out + `\` + c
    } else {
      out = out + c
    }
  }
  out
}
fn _regex_interp(x) {
  if type_of(x) == "Object" && x.has("class") && x["class"] == "Regex" {
    "(?:" + x._pat + ")"
  } else {
    _regex_escape("{x}")
  }
}
let _regex_module = fn () {
  class Regex {
    new(pattern) {
      self._pat = pattern
      _Regex.check(pattern)
    }
    test(s) {
      _Regex.test(self._pat, s)
    }
    find(s) {
      _Regex.find(self._pat, s)
    }
    match(s) {
      _Regex.match(self._pat, s)
    }
    find_all(s) {
      _Regex.find_all(self._pat, s)
    }
    find_all_str(s) {
      _Regex.find_all_str(self._pat, s)
    }
    find_all_index(s) {
      _Regex.find_all_index(self._pat, s)
    }
    count(s) {
      _Regex.count(self._pat, s)
    }
    find_iter(s) {
      _regex_find_iter(self._pat, s)
    }
    replace_all(s, repl) {
      if type_of(repl) != "Function" {
        return _Regex.replace_all(self._pat, s, repl)
      }
      let mut out = ""
      let mut last = 0
      for m in _Regex.find_all(self._pat, s) {
        out = out + s.slice(last, m.start) + repl(m)
        last = m.end
      }
      out + s.slice(last, s.size())
    }
    replace_first(s, repl) {
      if type_of(repl) != "Function" {
        return _Regex.replace_first(self._pat, s, repl)
      }
      let m = _Regex.find(self._pat, s)
      return s if m == nil
      s.slice(0, m.start) + repl(m) + s.slice(m.end, s.size())
    }
    split(s) {
      _Regex.split(self._pat, s)
    }
  }
  {
    compile: fn (pattern, flags = "") {
      Regex.new(if flags == "" {
        pattern
      } else {
        "(?" + flags + ")" + pattern
      })
    },
    escape: _regex_escape,
    interp: _regex_interp,
    find: fn (pattern, s) {
      _Regex.find(pattern, s)
    },
    match: fn (pattern, s) {
      _Regex.match(pattern, s)
    },
    find_all: fn (pattern, s) {
      _Regex.find_all(pattern, s)
    },
    test: fn (pattern, s) {
      _Regex.test(pattern, s)
    },
    split: fn (pattern, s) {
      _Regex.split(pattern, s)
    },
    replace_all: fn (pattern, s, repl) {
      Regex.new(pattern).replace_all(s, repl)
    },
    replace_first: fn (pattern, s, repl) {
      Regex.new(pattern).replace_first(s, repl)
    },
    Regex: Regex,
  }
}
let Regex = _regex_module()
)=culpre=";

inline constexpr const char* STRING_REPLACE_MODULE_SOURCE = R"=culpre=(let replace = fn (s, pat, repl) {
  if type_of(pat) == "String" {
    s.split(pat).join(repl)
  } else {
    pat.replace_all(s, repl)
  }
}
)=culpre=";

inline constexpr const char* LOG_MODULE_SOURCE = R"=culpre=(let _log_module = fn () {
  let _levels = {debug: 0, info: 1, warn: 2, error: 3}
  let mut _threshold = _levels.get(Sys.env("LOG_LEVEL"), 1)
  let mut _format = if Sys.env("LOG_FORMAT") == "json" {
    "json"
  } else {
    "text"
  }
  let _colors = {
    debug: "\x1b[2m",
    info: "\x1b[32m",
    warn: "\x1b[33m",
    error: "\x1b[31m",
  }
  let _emit = fn (name, num, msg, bound, fields) {
    if num >= _threshold {
      let _all = {...bound, ...fields}
      let ts = _Time.iso_nanos(_Time.now_nanos(), true)
      if _format == "json" {
        IO.eprint(JSON.stringify({..._all, time: ts, level: name, msg: msg}) +
          "\n")
      } else {
        let mut lvl = name
        if IO.stderr_is_terminal() {
          lvl = _colors.get(name, "") + name + "\x1b[0m"
        }
        let mut line = ts + " " + lvl + " " + msg
        for k, v in _all {
          line = line + " " + k + "=" + to_string(v)
        }
        IO.eprint(line + "\n")
      }
    }
  }
  let _methods = fn (bound) {
    {
      debug: fn (msg, fields = {}) {
        _emit("debug", 0, msg, bound, fields)
      },
      info: fn (msg, fields = {}) {
        _emit("info", 1, msg, bound, fields)
      },
      warn: fn (msg, fields = {}) {
        _emit("warn", 2, msg, bound, fields)
      },
      error: fn (msg, fields = {}) {
        _emit("error", 3, msg, bound, fields)
      },
      with: fn (more) {
        _methods({...bound, ...more})
      },
    }
  }
  let _set_level = fn (l) {
    let n = _levels.get(l, -1)
    throw "Log.set_level: unknown level '" + l + "'" if n < 0
    _threshold = n
  }
  let _set_format = fn (f) {
    if f != "text" {
      throw "Log.set_format: unknown format '" + f + "'" if f != "json"
    }
    _format = f
  }
  {..._methods({}), set_level: _set_level, set_format: _set_format}
}
let Log = _log_module()
)=culpre=";

inline constexpr const char* DESKTOP_MODULE_SOURCE = R"=culpre=(let _desktop_module = fn () {
  # A Tauri-shaped desktop facade: local HTTP server + native WebView + assets,
  # in one call.
  let run = fn (config) {
    let workers = config.get("workers", 4)

    let srv = Http.server()
    srv.static("/", config["assets"]) if config.has("assets")
    config["routes"](srv) if config.has("routes")
    srv.post("/__quit", fn (req) {
      Webview.Window.quit()
      ""
    })
    # The server outlives every early exit from here on — a throw out of the
    # WebView would otherwise leave its workers holding the port for the rest
    # of the process.
    defer {
      srv.stop()
    }
    let port = if config.has("port") {
      # An explicit port is a contract: if it's taken, fail loudly.
      srv.listen_async(config["port"], workers: workers)
    } else {
      try {
        srv.listen_async(8731, workers: workers)
      } catch _ {
        # Default port taken (most likely another culebra desktop app): retry
        # once on an OS-assigned free port — a failed bind leaves the handle
        # unbound and its routes recorded, so the same server serves the retry.
        # A non-bind startup error reproduces identically here and propagates.
        srv.listen_async(0, workers: workers)
      }
    }
    let base = "http://127.0.0.1:" + port.to_string()

    let w = Webview.Window.new()
    w.set_title(config.get("title", "culebra"))
    if config.has("size") {
      let size = config["size"]
      w.set_size(size[0], size[1])
    }
    w.navigate(base + "/")
    w.run()
  }

  let quit = fn () {
    Webview.Window.quit()
  }

  {run: run, quit: quit}
}
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
let _path_module = fn () {
  # A structural "is this a Path" probe: a Path is the only object carrying a
  # `_path` string field. `type_of` first so `.has` is only ever called on an
  # Object (String/Array/… don't answer `.has`).
  let _is_path = fn (o) {
    type_of(o) == "Object" && o.has("_path")
  }
  # PathLike coercion used *inside* the class: a String/StringView stays a
  # string, a Path collapses to its inner string via __str__. Anything else is
  # a TypeError — the same strictness the native FS.* layer enforces, so a
  # non-path never slips through Path.new / `/` / `rename` as stringified junk.
  let _s = fn (o) {
    let t = type_of(o)
    if t == "String" || t == "StringView" {
      o
    } else if _is_path(o) {
      to_string(o)
    } else {
      throw {
        kind: "TypeError",
        message: "type error: expected String|Path, got {t}",
      }
    }
  }
  class Path {
    new(p) {
      self._path = _s(p)
    }

    # --- display / conversion ---
    __str__() {
      self._path
    }  # "{p}" and to_string(p) yield the raw path
    str() {
      self._path
    }  # explicit String escape hatch
    # equal only to a String/StringView or another Path with the same path;
    # any other type is simply not-equal (never a TypeError).
    __eq__(o) {
      if type_of(o) == "String" || type_of(o) == "StringView" {
        self._path == o
      } else if _is_path(o) {
        self._path == to_string(o)
      } else {
        false
      }
    }
    # Order paths by their normalized inner string, so a Path array sorts and
    # `<`/`<=`/`>`/`>=` work. `_s` coerces a String/StringView/Path to the raw
    # path (and throws on anything else) — comparing against a non-path is a
    # TypeError, not a silent false, since an ordering has no meaningful answer
    # there. `<=`/`>`/`>=` fall out of `__lt__` + `__eq__` via the backends'
    # comparison derivation. Raw (not resolved) to stay consistent with __eq__.
    __lt__(o) {
      self._path < _s(o)
    }

    # --- joining: `base / "sub" / "leaf"` ---
    join(o) {
      Path.new(FS.join(self._path, _s(o)))
    }
    __div__(o) {
      self.join(o)
    }

    # --- path components (String, mirroring FS.*) ---
    # Pure, total, O(1) string derivations, so they read as properties
    # (`p.name`, `p.parent`) — no parens. `p.name()` still works too. The
    # filesystem ops below stay methods: they do I/O and can throw.
    get name() {
      FS.basename(self._path)
    }  # final component, e.g. "content.js"
    get stem() {
      FS.stem(self._path)
    }  # final component without suffix
    get suffix() {
      FS.extension(self._path)
    }  # extension incl. dot, e.g. ".js"
    get parent() {
      Path.new(FS.dirname(self._path))
    }

    # --- queries ---
    exists() {
      FS.exists(self._path)
    }
    is_file() {
      FS.is_file(self._path)
    }
    is_dir() {
      FS.is_dir(self._path)
    }

    # --- filesystem ops (delegate to FS) ---
    read() {
      FS.read(self._path)
    }
    write(content) {
      FS.write(self._path, content)
    }
    mkdir() {
      FS.mkdir(self._path)
    }  # creates parents (FS.mkdir does)
    remove(recursive = false) {
      FS.remove(self._path, recursive: recursive)
    }
    rename(dst) {
      FS.rename(self._path, _s(dst))
      Path.new(_s(dst))
    }

    # --- normalization ---
    resolve() {
      Path.new(FS.abspath(self._path))
    }

    # --- directory listing / globbing (return Path, so chains stay Path) ---
    list() {
      FS.list_dir(self._path).map(fn (e) {
        Path.new(FS.join(self._path, e))
      })
    }
    glob(pattern) {
      FS.glob(FS.join(self._path, pattern)).map(fn (p) {
        Path.new(p)
      })
    }
    walk() {
      FS.walk(self._path).map(fn (p) {
        Path.new(p)
      })
    }
  }
  Path
}
let Path = _path_module()
)=culpre=";

inline constexpr const char* VECTOR2_MODULE_SOURCE = R"=culpre=(# Vector2 — a minimal 2D float vector for graphics/game code. Elements are
# always Float: `new` accepts Long|Float and coerces via to_float, so
# `Vector2.new(1, 0)` works, but self.x/self.y are never Long. Distinct from
# the `@packable class FloatPair` used for SharedBuffer's fixed-layout demo
# (see tests/test_packable.cul) — that type is a raw fixed-layout descriptor
# with no operators; this is the general-purpose math type. Also stands in
# for a "Point" (position vs. direction is not distinguished, matching Unity
# / Godot / three.js rather than CGAL / nalgebra's stricter split).
let _vector2_module = fn () {
  class Vector2 {
    new(x: Long | Float, y: Long | Float) {
      self.x = to_float(x)
      self.y = to_float(y)
    }

    __str__() {
      "({self.x}, {self.y})"
    }

    __add__(o) {
      Vector2.new(self.x + o.x, self.y + o.y)
    }
    __sub__(o) {
      Vector2.new(self.x - o.x, self.y - o.y)
    }
    __mul__(k: Long | Float) {
      Vector2.new(self.x * k, self.y * k)
    }
    __neg__() {
      Vector2.new(-self.x, -self.y)
    }
    # Nominal, not structural: a `match` type-pattern checks the class tag
    # (docs/language.md §match), so this correctly rejects a same-shaped
    # Vector3 (also has x/y) — a plain `o.has("x") && o.has("y")` probe
    # would not. Mirrors Duration.__eq__ / Instant.__eq__ in time.cul: a
    # non-matching match yields nil, so a non-Vector2 RHS is simply
    # not-equal, never a thrown TypeError.
    __eq__(o) {
      let v = match o {
        v: Vector2 => v,
      }
      v != nil && self.x == v.x && self.y == v.y
    }

    dot(o) {
      self.x * o.x + self.y * o.y
    }
    length_squared() {
      self.dot(self)
    }
    length() {
      Math.sqrt(self.length_squared())
    }
    # A zero vector's normalized() divides by 0.0 like any other Float
    # division in culebra — raises ZeroDivisionError. No silent fallback.
    normalized() {
      let len = self.length()
      Vector2.new(self.x / len, self.y / len)
    }
    # Component-wise, not `(self - o).length()` — avoids allocating a
    # throwaway Vector2 just to measure it.
    distance_to(o) {
      let dx = self.x - o.x
      let dy = self.y - o.y
      Math.sqrt(dx * dx + dy * dy)
    }
  }
  Vector2
}
let Vector2 = _vector2_module()
)=culpre=";

inline constexpr const char* VECTOR3_MODULE_SOURCE = R"=culpre=(# Vector3 — the 3D counterpart of Vector2 (see vector2.cul for the shared
# design rationale: Float-only, no Point split, nominal __eq__). cross() is
# intentionally omitted from both: Vector2's cross is a scalar (perp dot),
# Vector3's is a vector — the asymmetry invites naming/shape drift, and no
# example in this repo needs it yet.
let _vector3_module = fn () {
  class Vector3 {
    new(x: Long | Float, y: Long | Float, z: Long | Float) {
      self.x = to_float(x)
      self.y = to_float(y)
      self.z = to_float(z)
    }

    __str__() {
      "({self.x}, {self.y}, {self.z})"
    }

    __add__(o) {
      Vector3.new(self.x + o.x, self.y + o.y, self.z + o.z)
    }
    __sub__(o) {
      Vector3.new(self.x - o.x, self.y - o.y, self.z - o.z)
    }
    __mul__(k: Long | Float) {
      Vector3.new(self.x * k, self.y * k, self.z * k)
    }
    __neg__() {
      Vector3.new(-self.x, -self.y, -self.z)
    }
    __eq__(o) {
      let v = match o {
        v: Vector3 => v,
      }
      v != nil && self.x == v.x && self.y == v.y && self.z == v.z
    }

    dot(o) {
      self.x * o.x + self.y * o.y + self.z * o.z
    }
    length_squared() {
      self.dot(self)
    }
    length() {
      Math.sqrt(self.length_squared())
    }
    normalized() {
      let len = self.length()
      Vector3.new(self.x / len, self.y / len, self.z / len)
    }
    # Component-wise, not `(self - o).length()` — avoids allocating a
    # throwaway Vector3 just to measure it.
    distance_to(o) {
      let dx = self.x - o.x
      let dy = self.y - o.y
      let dz = self.z - o.z
      Math.sqrt(dx * dx + dy * dy + dz * dz)
    }
  }
  Vector3
}
let Vector3 = _vector3_module()
)=culpre=";

inline constexpr const char* EFFECTS_MODULE_SOURCE = R"=culpre=(# Algebraic-effects runtime (thin slice). The parse-time transform
# (effects_transform.h) lowers `effect fn` / `perform` / `handle … with`
# into synthesized computation classes plus calls to `__Eff.handle`; this
# module is the dynamically-scoped handler stack and the trampoline driver
# that resumes a computation across its suspension points. Because it is
# ordinary culebra source, the interp / JIT / AOT backends run it uniformly.
#
# Every `effect fn f` lowers to a pair: `__eff_comp_f(args)` builds the
# un-driven computation object, and `f(args)` drives one at its own call
# site (`_run_comp`). Code compiled INTO a computation body delegates by
# calling the maker (static routing — no dynamic "am I inside a _step"
# flag); everything else calls `f` and gets direct dispatch.
#
# A computation object exposes `_step(rv)` returning a tag:
#   0 = DONE     — `_eff_val` holds the result
#   1 = SUSPEND  — `_eff_op` / `_eff_args` describe a `perform`
#   2 = DELEGATE — `_eff_delegate` is a sub-computation to run first
# The driver keeps an explicit stack of computation frames (bottom = the
# handled body, higher frames = delegated effect-fn calls). A `perform`
# reaching a non-tail clause captures the WHOLE frame stack as the
# continuation, so `resume` re-runs the enclosing computation across
# delegate boundaries. Each `resume` clones every
# frame (`__eff_copy`, a native shallow object copy) and drives the fresh copy,
# leaving the snapshot intact — that is what makes a continuation multi-shot.
#
# Handler clauses are classified at parse time (t on each frame entry):
#   "t" tail-resumptive — `resume(v)` exactly once, in tail position
#   "a" abort           — never resumes; its result becomes the handle result
#   "f" full-control    — anything else (multi-shot / non-tail resume)
# Tail and abort clauses also serve DIRECT dispatch (a `perform` in a plain
# fn, or an effect fn driven at its call site): tail runs with an identity
# resume — the native stack is the continuation — and abort unwinds to its
# `handle` via the native abort signal (`__eff_abort`), which user try/catch
# cannot observe. Full-control needs a captured continuation, so reaching one
# from direct dispatch is an EffectError (the native frames in between cannot
# be reified).
let _eff_module = fn () {
  # Dynamically-scoped handlers indexed by op-name: each op maps to a stack of
  # its live `{t, f, tok}` entries, innermost last. Lookup is one dict probe
  # regardless of how deeply `handle`s nest (an O(1) dispatch that stays a pure
  # preamble change — no evidence-passing / type-directed compilation, so the
  # three backends still run identical lowered source). Keys are prefixed with
  # `@` so a user op named like a dict method (`get`, `has`, …) can never
  # shadow the method we call on this dict.
  let _op_stacks = {}
  fn _key(op) {
    "@" + op
  }
  # Monotonic token distinguishing nested `handle`s, so an abort unwinds to
  # exactly the handle whose clause ran (not the innermost catcher).
  let _tok = [0]
  # Identity resume for tail clauses (direct dispatch and the driven fast
  # path) — hoisted so the per-perform hot path does not allocate a closure
  # (which also perturbs the leak oracle).
  let _id_resume = fn (v) {
    v
  }

  fn _find(op, line) {
    let st = _op_stacks.get(_key(op), nil)
    if st != nil {
      let n = st.size()
      return st[n - 1] if n > 0
    }
    # `line` is the original source line of the `perform` (carried on the
    # computation object by the transform), so the error points at the caller.
    throw {
      kind: "EffectError",
      message: "no handler for effect '{op}'",
      line: line,
    }
  }

  fn _full_control_error(op, line) {
    throw {
      kind: "EffectError",
      message: "effect '{op}' reached a full-control handler (non-tail `resume`) from outside an `effect fn`; declare the performing fn as `effect fn` so the continuation can be captured",
      line: line,
    }
  }

  # Run each abandoned frame's deferred cleanup, innermost (deepest) first.
  fn _finalize_stack(stack) {
    for i in stack.size() - 1..=0 by -1 {
      stack[i]._eff_finalize()
    }
  }

  # Run a driver loop for `stack`; if an abort *signal* unwinds through it — a
  # plain-fn `perform` or a nested-handle cross — finalize the frames it
  # abandoned (each frame's `_eff_finalize` is guarded, so one the signal
  # already finalized is not run twice). A signal carrying this guard's own
  # token is consumed: the abort clause's result becomes the result here,
  # bypassing any return clause. Anyone else's signal (or any signal when `tok`
  # is nil — `_run_comp` owns no handle) keeps unwinding to its handle.
  fn _guard_stack(stack, tok, run) {
    let pair = __eff_catch_abort(run)
    if pair[0] {
      _finalize_stack(stack)
      let sig = pair[1]
      return sig[1] if sig[0] == tok
      __eff_abort(sig)
    }
    pair[1]
  }

  # A frame's `defer`s run when it is abandoned. Normal completion and a regular
  # `throw` finalize the frame inside `_step`. An abort *clause* that returns a
  # value without resuming abandons the suspended frames while the drive is still
  # live, so they are finalized inline below — keyed on the "a" classification,
  # the only clause that provably never resumes. A regular `throw` out of a
  # "t"/"a" clause (or a no-handler lookup) also finalizes inline at each
  # dispatch site. An abort *signal* unwinding through a driver abandons its
  # frames without that: `_guard_stack` finalizes them on the way out. Both
  # backends stay symmetric.
  #
  # A full-control `resume` re-enters `_drive` recursively, so this stays a plain
  # loop with no per-call abort guard — the guard belongs on the non-recursive
  # `_handle` frame that owns the stack (see `_handle`). A driven abort clause is
  # still finalized inline here, since it returns rather than unwinding.
  fn _drive(stack, rv, ret) {
    let mut resume_val = rv
    while true {
      let comp = stack[stack.size() - 1]
      # A regular `throw` out of `_step` abandons every frame currently on
      # `stack` (delegate ancestors included) — `_step` finalizes `comp` itself,
      # but an ancestor delegate frame below it needs this to finalize too. This
      # is sound to catch unconditionally here: `_step` never captures `stack`
      # into a `resume` closure (only the full-control `hf` call below does
      # that, and only that call skips the finalize so an exception there
      # can't spuriously finalize a continuation that might be resumed again).
      let tag = try {
        comp._step(resume_val)
      } catch e {
        _finalize_stack(stack)
        throw e
      }
      if tag == 0 {
        let done_val = comp._eff_val
        if stack.size() == 1 {
          # The handled body finished: a `return` clause (if any) maps it.
          return ret(done_val) if ret != nil
          return done_val
        }
        # A delegated call finished; feed its value to the enclosing frame.
        stack.pop()
        resume_val = done_val
        continue
      }
      if tag == 1 {
        # Bind the adapter before calling it: `h.f(…)` in call form makes
        # the JIT's UFCS-candidate analysis treat `f` as a possible free
        # variable, which — since this preamble is spliced into the entry
        # module — can capture a same-named user global fn. That breaks the
        # lazy-ns builder's captureless invariant (see the dynamic-perform
        # cycle notes; the runtime now raises a loud error on this instead
        # of a silent crash, but the bind-then-call form avoids it outright).
        # A no-handler EffectError abandons the frames just like a clause
        # throw (`_find` never touches the stack, so the catch is sound).
        let h = try {
          _find(comp._eff_op, comp._eff_line)
        } catch e {
          _finalize_stack(stack)
          throw e
        }
        let hf = h.f
        # Tail clause: a lone trailing `resume(v)`, so no continuation needs
        # capturing — run it with the identity resume (as direct dispatch
        # already does) and feed its value straight back into this loop. No
        # snapshot clone, no recursion: a chain of N tail performs stays at
        # O(1) native depth instead of nesting N `_drive` frames.
        if h.t == "t" {
          # A "t" clause provably cannot save `resume` (its one appearance is
          # the trailing call), so a throw out of it abandons the suspended
          # frames for good — run their defers on the way out.
          resume_val = try {
            hf(comp._eff_args, _id_resume)
          } catch e {
            _finalize_stack(stack)
            throw e
          }
          continue
        }
        let is_abort = h.t == "a"
        let snapshot = stack  # frozen at the suspend point; forks clone it
        let resume = fn (v) {
          _drive(snapshot.map(|c| __eff_copy(c)), v, ret)
        }
        # An "a" clause never mentions `resume`, so — like the tail path — a
        # throw out of it abandons the frames: finalize. An "f" clause may
        # have stored `resume` for a later fork, so its throw leaves the
        # frames alone (finalizing would also poison the fork's copies, which
        # inherit `_eff_finalized`); a kept continuation runs its defers when
        # its fork completes.
        let result = try {
          hf(comp._eff_args, resume)
        } catch e {
          _finalize_stack(stack) if h.t != "f"
          throw e
        }
        # An abort clause never resumes: the suspended body is abandoned.
        _finalize_stack(stack) if is_abort
        return result
      }
      # DELEGATE: push the sub-computation as a new frame and run it next.
      stack.push(comp._eff_delegate)
      resume_val = nil
    }
  }

  fn _handle(comp, frame, ret) {
    # `frame` maps each handled op-name to its {t: class, f: adapter} entry
    # (one per `with` clause); `ret` is the optional `return`-clause adapter
    # (nil if none). Push each op's entry onto its per-op stack for `comp`'s
    # duration, tagging it with this handle's abort token.
    _tok[0] += 1
    let tok = _tok[0]
    let keys = []
    for op, entry in frame {
      entry["tok"] = tok
      let k = _key(op)
      _op_stacks[k] ??= []
      _op_stacks[k].push(entry)
      keys.push(k)
    }
    defer {
      for pk in keys {
        _op_stacks[pk].pop()
      }
    }
    # `_drive` mutates `stack` in place, so on an abort signal the frames left
    # here are exactly the ones the signal abandoned; `_guard_stack` finalizes
    # them and settles a signal carrying this handle's token.
    let stack = [comp]
    _guard_stack(stack, tok, fn () {
      _drive(stack, nil, ret)
    })
  }

  # A `perform` in ordinary (non-effect) code: no computation object, no
  # suspension — dispatch straight off the per-op handler stack. Tail clauses run
  # with an identity resume (the native stack is the continuation); abort
  # clauses compute their result and unwind to their handle.
  fn _perform_direct(op, args, line) {
    let h = _find(op, line)
    let hf = h.f
    _full_control_error(op, line) if h.t == "f"
    __eff_abort([h.tok, hf(args, _id_resume)]) if h.t == "a"
    hf(args, _id_resume)
  }

  # An effect-fn call from ordinary code: the caller's native frame IS the
  # continuation, so drive the computation right here. SUSPENDs dispatch
  # via _perform_direct; DELEGATE frames stack as in _drive. An abort signal
  # unwinds natively through here; `_guard_stack` finalizes the abandoned frames
  # before it keeps unwinding to the handle that owns it.
  fn _run_comp(comp) {
    let stack = [comp]
    _guard_stack(stack, nil, fn () {
      let mut resume_val = nil
      while true {
        let c = stack[stack.size() - 1]
        # Same reasoning as `_drive`: a regular throw out of `_step` abandons
        # every frame on `stack`, not just `c` (which already finalized
        # itself).
        let tag = try {
          c._step(resume_val)
        } catch e {
          _finalize_stack(stack)
          throw e
        }
        if tag == 0 {
          return c._eff_val if stack.size() == 1
          stack.pop()
          resume_val = c._eff_val
          continue
        }
        if tag == 1 {
          # Direct dispatch only ever runs "t"/"a" clauses (a full-control
          # clause raises before invoking anything), so a regular throw out of
          # it always abandons the frames — finalize. An abort *signal* is not
          # observable by `catch` and unwinds to `_guard_stack` instead.
          resume_val = try {
            _perform_direct(c._eff_op, c._eff_args, c._eff_line)
          } catch e {
            _finalize_stack(stack)
            throw e
          }
          continue
        }
        stack.push(c._eff_delegate)
        resume_val = nil
      }
    })
  }

  {handle: _handle, perform_direct: _perform_direct, run_comp: _run_comp}
}
let __Eff = _eff_module()
)=culpre=";

