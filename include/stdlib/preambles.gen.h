// Generated from src/preambles/*.cul by misc/gen_preambles.sh — do not edit.
// Edit the .cul sources, then run `just gen-preambles` (CI checks sync).
#pragma once

inline constexpr const char* TIME_MODULE_SOURCE = R"=culpre=(let _time_module = fn () {
  let _type_error = fn (want, got) {
    throw {
      kind: "TypeError",
      message: "type error: expected {want}, got {type_of(got)}",
    }
  }
  # Nanoseconds are a Long, so every step checks the ±292-year range rather
  # than letting the count wrap; a Float rounds to the nearest nanosecond.
  let _out_of_range = fn () {
    throw {
      kind: "ValueError",
      message: "Time: out of range (a Long count of nanoseconds, about ±292 years)",
    }
  }
  let _long_min = -9223372036854775807 - 1
  let _add_ns = fn (a, b) {
    let r = a + b
    _out_of_range() if ((a ^ r) & (b ^ r)) < 0
    r
  }
  let _sub_ns = fn (a, b) {
    let r = a - b
    _out_of_range() if ((a ^ b) & (a ^ r)) < 0
    r
  }
  # Math.round raises for a Float with no Long; that is Time's range too.
  let _float_ns = fn (x) {
    try {
      Math.round(x)
    } catch e {
      _out_of_range()
    }
  }
  let _scale_ns = fn (n, unit) {
    match n {
      i: Long => {
        let p = i * unit
        _out_of_range() if (i == -1 && unit == _long_min) || (i != 0 && p / i != unit)
        p
      },
      f: Float => _float_ns(f * to_float(unit)),
      _ => _type_error("Long or Float", n),
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
        -self
      } else {
        Duration.new(self._nanos)
      }
    }
    __add__(o) {
      let n = match o {
        d: Duration => d._nanos,
      }
      _type_error("Duration", o) if n == nil
      Duration.new(_add_ns(self._nanos, n))
    }
    __sub__(o) {
      let n = match o {
        d: Duration => d._nanos,
      }
      _type_error("Duration", o) if n == nil
      Duration.new(_sub_ns(self._nanos, n))
    }
    __mul__(n) {
      Duration.new(_scale_ns(n, self._nanos))
    }
    __div__(n) {
      Duration.new(match n {
        i: Long => {
          _out_of_range() if i == -1 && self._nanos == _long_min
          self._nanos / i
        },
        f: Float => _float_ns(to_float(self._nanos) / f),
        _ => _type_error("Long or Float", n),
      })
    }
    __neg__() {
      Duration.new(_sub_ns(0, self._nanos))
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
      Instant.new(_add_ns(self._nanos, n))
    }
    __sub__(o) {
      let r = match o {
        d: Duration => Instant.new(_sub_ns(self._nanos, d._nanos)),
        i: Instant => Duration.new(_sub_ns(self._nanos, i._nanos)),
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
      Instant.new(_scale_ns(secs, 1000000000))
    },
    from_parts: fn (p, utc = false) {
      Instant.new(_Time.from_parts_nanos(p, utc))
    },
    parse: fn (s, fmt) {
      Instant.new(_Time.parse_nanos(s, fmt))
    },
    seconds: fn (n) {
      Duration.new(_scale_ns(n, 1000000000))
    },
    milliseconds: fn (n) {
      Duration.new(_scale_ns(n, 1000000))
    },
    minutes: fn (n) {
      Duration.new(_scale_ns(n, 60000000000))
    },
    hours: fn (n) {
      Duration.new(_scale_ns(n, 3600000000000))
    },
    days: fn (n) {
      Duration.new(_scale_ns(n, 86400000000000))
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
    set(x, y, glyph, style = "") {
      let gs = to_string(glyph)  # graphemes()/slices yield StringView
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
      self._w = _Canvas.sprite_width(self._id)
      self._h = _Canvas.sprite_height(self._id)
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
  # `Dir.embedded(...).read(...)` -- the latter bakes the font into an AOT
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
  let _text = fn (s, x, y, color, scale = 1) {
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

  # --- window / system ------------------------------------------------------
  # No-ops in the browser and headless backends: the embedding host page owns
  # fullscreen and gamepad for the browser build already (relaying the F key
  # and Gamepad API onto the Playground's canvas pane; see docs/index.html in
  # yhirose/Lunar-Lander-Culebra for the pattern), and there is no window at
  # all headless.
  let toggle_fullscreen = fn () {
    _Canvas.toggle_fullscreen()
  }
  let fullscreen = fn () {
    _Canvas.is_fullscreen()
  }
  let show_cursor = fn () {
    _Canvas.show_cursor()
  }
  let hide_cursor = fn () {
    _Canvas.hide_cursor()
  }
  let cursor_hidden = fn () {
    _Canvas.cursor_hidden()
  }
  let clipboard = fn () {
    _Canvas.clipboard_get()
  }
  let set_clipboard = fn (text) {
    _Canvas.clipboard_set(text)
  }
  # Lets the OS window be dragged larger/smaller; every Canvas window is a
  # fixed size otherwise. The framebuffer's own logical resolution does not
  # follow a resize on its own -- resized() is how a script notices one to
  # react itself (re-`init()` at a new size, reflow its own UI, or ignore it).
  let resizable = fn (enabled) {
    _Canvas.set_resizable(enabled)
  }
  let resized = fn () {
    _Canvas.window_resized()
  }
  # Closes the window, ending run()'s loop on its next frame the same way the
  # window's own close box does -- see closing() below, which the two share.
  # A no-op on the wasm and headless backends, where there is no window;
  # can_quit() says which is which, so a game can decide whether an in-game
  # "quit?" prompt makes sense before ever offering one.
  let quit = fn () {
    _Canvas.quit()
  }
  let can_quit = fn () {
    _Canvas.can_quit()
  }

  # --- timing ----------------------------------------------------------------
  # `run`'s tick is vsynced to target_fps (60 by default, matching the browser
  # loop), so most games never need these: dt() only matters to one that wants
  # to step by real elapsed seconds instead of the fixed 1/60 DT a vsynced
  # tick otherwise assumes (as this repo's own lunar_lander.cul does).
  let dt = fn () {
    _Canvas.dt()
  }
  let target_fps = fn (n) {
    _Canvas.set_target_fps(n)
  }
  let fps = fn () {
    _Canvas.fps()
  }

  # --- mouse wheel ------------------------------------------------------------
  # Vertical delta since the last frame; positive is away from the user
  # (scroll up / zoom in).
  let wheel = fn () {
    _Canvas.mouse_wheel()
  }

  # --- gamepad ---------------------------------------------------------------
  # Button/axis numbers are raylib's own GamepadButton/GamepadAxis values,
  # named here so a script never has to know them. `index` (0-3, raylib's
  # MAX_GAMEPADS) picks which pad -- 0 for the common single-controller case,
  # others for local multiplayer. pressed() is a same-frame edge the way
  # Input.pressed() is for buttons(), except raylib tracks it natively so
  # there is no update()/prev-state bookkeeping to do.
  let PAD_UP = 1
  let PAD_RIGHT = 2
  let PAD_DOWN = 3
  let PAD_LEFT = 4
  let PAD_Y = 5      # north face button (Xbox Y / PlayStation Triangle)
  let PAD_B = 6      # east face button (Xbox B / PlayStation Circle)
  let PAD_A = 7      # south face button (Xbox A / PlayStation Cross)
  let PAD_X = 8      # west face button (Xbox X / PlayStation Square)
  let PAD_LB = 9
  let PAD_LT = 10
  let PAD_RB = 11
  let PAD_RT = 12
  let PAD_SELECT = 13
  let PAD_GUIDE = 14
  let PAD_START = 15
  let PAD_L3 = 16    # left stick pressed in
  let PAD_R3 = 17    # right stick pressed in
  let AXIS_LX = 0
  let AXIS_LY = 1
  let AXIS_RX = 2
  let AXIS_RY = 3
  let AXIS_LT = 4
  let AXIS_RT = 5

  let pad_available = fn (index = 0) {
    _Canvas.pad_available(index)
  }
  let pad_axis = fn (axis, index = 0) {
    _Canvas.pad_axis(index, axis)
  }
  let pad_held = fn (button, index = 0) {
    _Canvas.pad_button(index, button)
  }
  let pad_pressed = fn (button, index = 0) {
    _Canvas.pad_pressed(index, button)
  }
  let pad_name = fn (index = 0) {
    _Canvas.pad_name(index)
  }
  # left/right motor strength 0..1, `sec` seconds. Silently does nothing on a
  # backend/pad without haptics (Xbox pads on macOS: no API drives them).
  let pad_rumble = fn (left, right, sec, index = 0) {
    _Canvas.pad_rumble(index, left, right, sec)
  }
  # Extra SDL_GameControllerDB mapping lines for a pad the bundled DB lacks
  # (a newer controller). Returns true on success.
  let pad_mappings = fn (db) {
    _Canvas.pad_mappings(db) == 1
  }

  # --- collision --------------------------------------------------------------
  # Axis-aligned rectangle / circle overlap -- the two shapes almost every
  # casual game ends up hand-rolling for hit detection. `rect` args are a
  # box's top-left and size, matching Canvas.rect's own (x, y, w, h).
  let rect_overlap = fn (x1, y1, w1, h1, x2, y2, w2, h2) {
    x1 < x2 + w2 && x2 < x1 + w1 && y1 < y2 + h2 && y2 < y1 + h1
  }
  let circle_overlap = fn (x1, y1, r1, x2, y2, r2) {
    let dx = x1 - x2
    let dy = y1 - y2
    dx * dx + dy * dy < (r1 + r2) * (r1 + r2)
  }
  let point_in_rect = fn (px, py, x, y, w, h) {
    px >= x && px < x + w && py >= y && py < y + h
  }
  let point_in_circle = fn (px, py, cx, cy, r) {
    let dx = px - cx
    let dy = py - cy
    dx * dx + dy * dy < r * r
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
    set_pixel: fn (x, y, color) {
      _Canvas.set_pixel(x, y, color)
    },
    get_pixel: fn (x, y) {
      _Canvas.get_pixel(x, y)
    },
    rect: fn (x, y, w, h, color, fill = true) {
      _Canvas.rect(x, y, w, h, color, if fill {
        1
      } else {
        0
      })
    },
    line: fn (x1, y1, x2, y2, color) {
      _Canvas.line(x1, y1, x2, y2, color)
    },
    circle: fn (cx, cy, r, color, fill = true) {
      _Canvas.ellipse(cx, cy, r, r, color, if fill {
        1
      } else {
        0
      })
    },
    ellipse: fn (cx, cy, rx, ry, color, fill = true) {
      _Canvas.ellipse(cx, cy, rx, ry, color, if fill {
        1
      } else {
        0
      })
    },
    triangle: fn (x1, y1, x2, y2, x3, y3, color, fill = true) {
      _Canvas.triangle(x1, y1, x2, y2, x3, y3, color, if fill {
        1
      } else {
        0
      })
    },
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
    text: _text,
    text_width: text_width,
    Font: Font,
    buttons: buttons,
    mouse: mouse,
    key: key,
    key_queue: key_queue,
    typed: typed,
    Input: Input,
    run: run,
    LEFT: LEFT,
    RIGHT: RIGHT,
    UP: UP,
    DOWN: DOWN,
    A: A,
    B: B,
    toggle_fullscreen: toggle_fullscreen,
    fullscreen: fullscreen,
    show_cursor: show_cursor,
    hide_cursor: hide_cursor,
    cursor_hidden: cursor_hidden,
    clipboard: clipboard,
    set_clipboard: set_clipboard,
    resizable: resizable,
    resized: resized,
    quit: quit,
    can_quit: can_quit,
    dt: dt,
    target_fps: target_fps,
    fps: fps,
    wheel: wheel,
    PAD_UP: PAD_UP,
    PAD_RIGHT: PAD_RIGHT,
    PAD_DOWN: PAD_DOWN,
    PAD_LEFT: PAD_LEFT,
    PAD_Y: PAD_Y,
    PAD_B: PAD_B,
    PAD_A: PAD_A,
    PAD_X: PAD_X,
    PAD_LB: PAD_LB,
    PAD_LT: PAD_LT,
    PAD_RB: PAD_RB,
    PAD_RT: PAD_RT,
    PAD_SELECT: PAD_SELECT,
    PAD_GUIDE: PAD_GUIDE,
    PAD_START: PAD_START,
    PAD_L3: PAD_L3,
    PAD_R3: PAD_R3,
    AXIS_LX: AXIS_LX,
    AXIS_LY: AXIS_LY,
    AXIS_RX: AXIS_RX,
    AXIS_RY: AXIS_RY,
    AXIS_LT: AXIS_LT,
    AXIS_RT: AXIS_RT,
    pad_available: pad_available,
    pad_axis: pad_axis,
    pad_held: pad_held,
    pad_pressed: pad_pressed,
    pad_name: pad_name,
    pad_rumble: pad_rumble,
    pad_mappings: pad_mappings,
    rect_overlap: rect_overlap,
    circle_overlap: circle_overlap,
    point_in_rect: point_in_rect,
    point_in_circle: point_in_circle,
  }
}
let Canvas = _canvas_module()
)=culpre=";

inline constexpr const char* AUDIO_MODULE_SOURCE = R"=culpre=(# Audio.Kauai: the Kauai language (docs/kauai/language.md), read, checked and
# played. A song is expanded to its notes, each timed in seconds, and handed to
# the runtime whole as a score (_Audio.score_*), which starts every note on
# the audio stream's own clock. Spliced in front of audio.cul, whose module
# builds it (misc/gen_preambles.sh).
let _kauai_module = fn () {
  let WHOLE = 1920  # time units in a whole note: a sixty-fourth is 30, a triplet eighth 160
  let GRACE = WHOLE / 64  # what an arpeggio rolls by, and what a grace note takes

  # `line` is where the mistake is: "file.kau:12", or "line 12" in a song given
  # as text.
  fn fail(line, msg) {
    throw {kind: "KauaiError", message: "{line}: {msg}"}
  }

  # --- lines and blocks ------------------------------------------------------

  # A `//` starts a comment only at the start of a line or after a space, so
  # one inside a word (`https://`) is not.
  fn strip_comment(s) {
    let at = s.index_of(" //")
    let cut = s.starts_with("//") ? 0 : at
    cut < 0 ? s : s.slice(0, cut).to_string()
  }

  # Where line `n` of `file` is, as an error names it.
  fn where(file, n) {
    file == nil ? "line {n}" : "{file}:{n}"
  }

  # The source as a tree: a block is {head, body, line}, a plain line is
  # {words, text, line}.
  fn blocks(src, file) {
    mut stack = [{head: [], body: [], line: where(file, 1)}]
    for i, raw in src.split("\n").enumerate() {
      let text = strip_comment(raw.to_string()).trim().to_string()
      continue if text.empty()
      let words = text.split_whitespace().map(|w| w.to_string())
      let line = where(file, i + 1)
      if text == "}" {
        fail(line, 'unmatched }') if stack.size() == 1
        let done = stack.pop()
        stack[stack.size() - 1].body.push(done)
      } else if text.ends_with('{') {
        stack.push({head: words.slice(0, words.size() - 1), body: [], line: line})
      } else {
        stack[stack.size() - 1].body.push({words: words, line: line, text: text})
      }
    }
    fail(stack[stack.size() - 1].line, 'unclosed {') if stack.size() != 1
    stack[0].body
  }

  # `s` split at the first `sep` (a pair of Strings), or nil; and `s` with
  # every `from` made `to`. The stdlib's own `split_once` and `replace` are
  # loaded only for a program that names them, so a stdlib module cannot use
  # them.
  fn halves(s, sep) {
    let i = s.index_of(sep)
    i < 0 ? nil : (s.slice(0, i).to_string(), s.slice(i + sep.size(), s.size()).to_string())
  }
  fn swap(s, from, to) {
    s.split(from).join(to)
  }

  # A name the song defines: a capitalised word.
  fn is_name(w) {
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ".contains(w.slice(0, 1).to_string())
  }

  fn name_at(words, i, line, what) {
    fail(line, "{what} needs a name") if i >= words.size()
    let w = words[i]
    fail(line, "{what} names start with a capital letter: {w}") if !is_name(w)
    w
  }

  # The words after each of `keys` in `words` (past the first `skip`), up to
  # the next key. A key with nothing after it is a flag: an empty list.
  fn options(words, skip, keys) {
    mut out = {}
    mut key = nil
    for w in words.slice(skip, words.size()) {
      if keys.contains(w) {
        key = w
        out[w] = []
      } else if key != nil {
        out[key].push(w)
      }
    }
    out
  }

  fn first_of(o, key, dflt) {
    (o.get(key, nil) ?? [dflt])[0]
  }

  # "46%" is 0.46, "34" is 34.
  fn amount(s) {
    s.ends_with("%") ? to_long(s.strip_suffix("%")) / 100.0 : to_long(s)
  }

  # The number `word` is, for option `what`.
  fn number_of(word, line, what) {
    fail(line, "{what} takes a number: {word}") if !is_number(word) || word.starts_with(".")
    word.contains(".") ? to_float(word) : to_long(word)
  }

  # An option's one number, or `dflt` when the option is not there.
  fn option_number(o, key, line, dflt) {
    return dflt if !o.has(key)
    fail(line, "{key} takes a number") if o[key].size() != 1
    number_of(o[key][0], line, key)
  }

  # `env A D R`, in ticks.
  fn env_of(o, line) {
    let env = o.get("env", nil) ?? ["0", "0", "0"]
    fail(line, "env is attack, decay and release: env 0 3 2") if env.size() != 3
    env.map(|x| number_of(x, line, "env"))
  }

  # --- pitch -----------------------------------------------------------------

  let LETTERS = "CDEFGAB"
  let PC = [0, 2, 4, 5, 7, 9, 11]

  # A note name as (letter index, semitone shift, octave or nil, octave marks).
  fn note_parts(tok, line) {
    let letter = LETTERS.index_of(tok.slice(0, 1).upper().to_string())
    fail(line, "not a note: {tok}") if letter < 0
    mut i = 1
    mut shift = 0
    while i < tok.size() {
      let ch = tok.slice(i, i + 1).to_string()
      cond {
        ch == "#" => shift += 1,
        ch == "b" => shift -= 1,
        _ => break,
      }
      i += 1
    }
    let accidentals = tok.slice(1, i).to_string()
    fail(line, "a note is sharp or flat, not both: {tok}") if accidentals.contains("#") && accidentals.contains("b")
    fail(line, "two accidentals at most: {tok}") if accidentals.size() > 2
    mut digits = ""
    mut marks = 0
    while i < tok.size() {
      let ch = tok.slice(i, i + 1).to_string()
      cond {
        "0123456789".contains(ch) => digits += ch,
        ch == "'" => marks += 1,
        ch == "," => marks -= 1,
        _ => fail(line, "not a note: {tok}"),
      }
      i += 1
    }
    (letter, shift, digits.empty() ? nil : to_long(digits), marks)
  }

  # An absolute note name ("D#3") as a MIDI number.
  fn midi(tok, line) {
    let (letter, shift, octave, _) = note_parts(tok, line)
    fail(line, "{tok} needs an octave") if octave == nil
    (octave + 1) * 12 + PC[letter] + shift
  }

  # `LO..HI`, one octave from a natural note: its lowest note.
  fn window(text, line) {
    let (lo, hi) = halves(text, "..") ?? fail(line, "a range is LO..HI")
    let low = midi(lo.to_string(), line)
    fail(line, "a range starts on a natural note: {text}") if !PC.contains(low % 12)
    fail(line, "{text} is not one octave") if midi(hi.to_string(), line) != low + 11
    low
  }

  # A melody note: with an octave number, exactly that note; without, its
  # letter's natural inside the window, then its accidental, and an octave for
  # each ' or ,. So `cb` is just below `c` and `b#` just above `b`, as on a staff.
  fn melody_note(tok, low, line) {
    let (letter, shift, octave, marks) = note_parts(tok, line)
    return (octave + 1) * 12 + PC[letter] + shift + 12 * marks if octave != nil
    low + Math.wrap(PC[letter] - low, 12) + shift + 12 * marks
  }

  # A pitch class ("Bb") from the head of a chord symbol, and the rest.
  fn split_root(s, line) {
    let letter = LETTERS.index_of(s.slice(0, 1).to_string())
    fail(line, "not a chord or key: {s}") if letter < 0
    let acc = s.size() > 1 ? s.slice(1, 2).to_string() : ""
    let shift = match acc {
      "#" => 1,
      "b" => -1,
      _ => 0,
    }
    let head = shift == 0 ? 1 : 2
    (Math.wrap(PC[letter] + shift, 12), s.slice(head, s.size()).to_string())
  }

  fn key_pc(name, line) {
    split_root(name, line)[0]
  }

  let INTERVALS = {
    m2: 1, M2: 2, m3: 3, M3: 4, P4: 5, TT: 6,
    P5: 7, m6: 8, M6: 9, m7: 10, M7: 11, P8: 12,
  }

  # --- chords ------------------------------------------------------------------

  let CLOSE = {
    "": [0, 4, 7, 12],
    m: [0, 3, 7, 12],
    "6": [0, 4, 7, 9],
    m6: [0, 3, 7, 9],
    "7": [0, 4, 7, 10],
    maj7: [0, 4, 7, 11],
    m7: [0, 3, 7, 10],
    m7b5: [0, 3, 6, 10],
    sus4: [0, 5, 7, 12],
    "7sus4": [0, 5, 7, 10],
    add9: [0, 4, 7, 14],
    "9": [0, 4, 7, 10, 14],
    maj9: [0, 4, 7, 11, 14],
    m9: [0, 3, 7, 10, 14],
    "6/9": [0, 4, 7, 9, 14],
    dim: [0, 3, 6, 12],
    dim7: [0, 3, 6, 9],
    aug: [0, 4, 8, 12],
    "7#5": [0, 4, 8, 10],
    sus2: [0, 2, 7, 12],
    mmaj7: [0, 3, 7, 11],
    madd9: [0, 3, 7, 14],
    "7b9": [0, 4, 7, 10, 13],
    "7#9": [0, 4, 7, 10, 15],
    "7#11": [0, 4, 7, 10, 18],
    "7alt": [0, 4, 10, 15, 20],
    "maj7#11": [0, 4, 7, 11, 18],
    m11: [0, 3, 7, 10, 14, 17],
    "13": [0, 4, 7, 10, 14, 21],
  }

  # The table the song being read uses: the built-in one and its voicings.
  mut qualities = CLOSE

  # Other spellings of the qualities above, as charts write them. Parentheses
  # around an alteration are left out before this: `C7(b9)` is `C7b9`.
  let SPELLINGS = {
    M7: "maj7", ma7: "maj7", "Δ": "maj7", "Δ7": "maj7",
    M9: "maj9", ma9: "maj9", "Δ9": "maj9",
    "-": "m", mi: "m", min: "m",
    "-6": "m6", "-7": "m7", mi7: "m7", min7: "m7", "-9": "m9", "-11": "m11",
    "ø": "m7b5", "ø7": "m7b5", "-7b5": "m7b5",
    mM7: "mmaj7", "-Δ7": "mmaj7", "mΔ7": "mmaj7",
    "°": "dim", o: "dim", "°7": "dim7", o7: "dim7",
    "+": "aug", "+7": "7#5", aug7: "7#5",
    sus: "sus4", "7sus": "7sus4",
    "△": "maj7", "△7": "maj7", "△9": "maj9", "m7-5": "m7b5",
    "69": "6/9",
  }

  # A chord symbol ("F#m7/A") as {root, bass, shape}, pitch classes. The bass
  # is after a `/` that a capital letter follows; `C6/9` is a quality.
  fn chord(written_sym, line) {
    return nil if written_sym == "N.C."
    let sym = written_sym.tr("♭♯", "b#")
    let cut = iota(sym.size()).find(|i| sym.slice(i, i + 1).to_string() == "/" && LETTERS.contains(sym.slice(i + 1, i + 2).to_string()))
    let name = cut == nil ? sym : sym.slice(0, cut).to_string()
    let over = cut == nil ? nil : sym.slice(cut + 1, sym.size()).to_string()
    let (root, written) = split_root(name.to_string(), line)
    let shape = quality_shape(written, written_sym, line)
    let bass = over == nil ? root : split_root(over.to_string(), line)[0]
    {root: root, bass: bass, shape: shape}
  }

  # A quality's intervals: a quality from the table and tensions in
  # parentheses, comma separated (`(9)`, `7(9,13)`, `m7(11)`, `7(-9,+11)`),
  # which add their notes, a `b5` or `#5` taking the fifth's place; else the
  # table's, with parentheses left out (`m(maj7)` is `mmaj7`).
  fn quality_shape(written, sym, line) {
    let open = written.index_of("(")
    let inside = open >= 0 && written.ends_with(")") ? written.slice(open + 1, written.size() - 1).to_string() : nil
    let base_name = open >= 0 ? written.slice(0, open).to_string() : written
    let base = qualities.get(SPELLINGS.get(base_name, base_name), nil)
    # Parentheses of numbers are tensions; others (`m(maj7)`) are left out.
    if inside == nil || base == nil || !all_of(inside, "0123456789b#-+,") {
      let plain = written.tr("()", "")
      return qualities.get(SPELLINGS.get(plain, plain), nil) ?? fail(line, "unknown chord: {sym}")
    }
    mut shape = [...base]
    mut seen = []
    for item in inside.split(",") {
      let t = item.to_string()
      let not_tension = "a tension is 9, b9, #9, 11, #11, 13 or b13, or a b5 or #5: {t} in {sym}"
      fail(line, not_tension) if t.empty()
      let acc = t.slice(0, 1).to_string()
      let shift = match acc {
        "b" | "-" => -1,
        "#" | "+" => 1,
        _ => 0,
      }
      let digits = shift == 0 ? t : t.slice(1, t.size()).to_string()
      let base_semis = match digits {
        "5" => 7,
        "9" => 14,
        "11" => 17,
        "13" => 21,
        _ => fail(line, not_tension),
      }
      fail(line, "a fifth in parentheses is b5 or #5: {t} in {sym}") if digits == "5" && shift == 0
      fail(line, not_tension) if (digits == "11" && shift < 0) || (digits == "13" && shift > 0)
      fail(line, "{t} is written twice in {sym}") if seen.contains(t)
      seen.push(t)
      if digits == "5" {
        shape = [...shape.filter(|i| i != 7), 7 + shift]
      } else {
        # On a triad the tension takes the place of the root's octave.
        shape = [...shape.filter(|i| i != 12 && i != base_semis + shift), base_semis + shift]
      }
    }
    shape.sorted()
  }

  fn transposed(c, n) {
    return nil if c == nil
    {...c, root: Math.wrap(c.root + n, 12), bass: Math.wrap(c.bass + n, 12)}
  }

  # The chord's `tok` degree over its root in semitones: 3 5 7 9 as the chord
  # has them.
  fn degree(c, tok, line) {
    let pick = fn (lo, hi, dflt) {
      c.shape.find(|i| i >= lo && i <= hi) ?? dflt
    }
    match tok {
      "3" => pick(3, 5, 4),
      "5" => pick(6, 8, 7),
      "7" => pick(9, 11, 10),
      "9" => pick(13, 15, 14),
      _ => fail(line, "not a degree: {tok}"),
    }
  }

  # The lowest `pc` at or above `floor`.
  fn at_or_above(pc, floor) {
    floor + Math.wrap(pc - floor, 12)
  }

  # A chord voiced by `v` (`plays chords [rootless] from NOTE`): its tones in
  # order, the first at or above the note, each next the lowest above the one
  # before.
  fn voiced(v, c) {
    let shape = v.rootless ? c.shape.filter(|i| i % 12 != 0) : c.shape
    mut out = []
    mut floor = v.from
    for i in shape {
      let p = at_or_above(c.root + i, floor)
      out.push(p)
      floor = p + 1
    }
    out
  }

  # --- time ----------------------------------------------------------------------

  # A groove row "1 - . 1  8 - 1 - | ..." bar by bar, for a bar of `len` units,
  # as {size, toks}: a bar of n tokens gives each token len / n units. A drum
  # row is letters, one a token, and spaces between them are ignored.
  fn grid(text, hit, line, len) {
    text.split("|").map(fn (bar_text) {
      let bt = bar_text.to_string()
      let toks = hit
        ? bt.iter().map(|ch| ch.to_string()).filter(|ch| !ch.trim().empty()).collect()
        : bt.split_whitespace().map(|w| w.to_string())
      let n = toks.size()
      fail(line, "a bar of a row needs a step") if n == 0
      fail(line, "a bar of {n} steps is too fine to play") if len % n != 0
      fail(line, "a bar of {n} steps makes each one no note value (plain, dotted or triplet): count the steps") if !is_step_value(len / n)
      {size: len / n, toks: toks}
    })
  }

  # Whether `u` units is a note value: plain (a whole note halved, and halved
  # again), dotted, or a triplet's.
  fn is_step_value(u) {
    iota(8).any(fn (k) {
      let plain = WHOLE / Math.pow(2, k)
      u == plain || u * 2 == plain * 3 || u * 3 == plain * 2
    })
  }

  # --- melody --------------------------------------------------------------------

  let NOTE_VALUES = [1, 2, 4, 8, 16, 32]
  let LEVELS = ["pp", "p", "mp", "mf", "f", "ff"]
  let DYNAMICS = {pp: 0.4, p: 0.55, mp: 0.7, mf: 0.85, f: 1.0, ff: 1.15}
  let TOUCHES = {".": "staccato", "!": "accent", "-": "tenuto", "^": "marcato", "@": "fermata"}

  # A note name with its accidentals and octave marks, and a tie after it:
  # "bb'~" as (name, tied).
  fn chord_note(t, line) {
    let tied = t.ends_with("~")
    let name = t.strip_suffix("~")
    fail(line, "not a note in a chord: {t}") if name.empty() || !"abcdefg".contains(name.slice(0, 1).to_string())
    fail(line, "not a note in a chord: {t}") if !all_of(name.slice(1, name.size()).to_string(), "#b',")
    (name, tied)
  }

  # A note token ".!bb'8.~" as {rest, names, ties, length (units or nil), tie,
  # marks}: playing marks before the name, the length and a tie after it. A
  # chord "<c e~ g>4" has several names, each of which may be tied, and a `~`
  # before it rolls it. Grace notes "{d e}" come first of all.
  fn melody_token(tok, line) {
    fail(line, "a tie is written against its note: c2~ c") if tok == "~"
    mut i = 0
    mut graces = []
    if tok.starts_with('{') {
      let close = tok.index_of('}')
      fail(line, 'a { without its }: ' + tok) if close < 0
      let inside = tok.slice(1, close).to_string().split("_").map(|x| x.to_string()).filter(|x| !x.empty())
      fail(line, "grace notes before a note: {tok}") if inside.empty()
      for t in inside {
        let (nm, td) = chord_note(t, line)
        fail(line, "a grace note is not tied: {tok}") if td
        graces.push(nm)
      }
      i = close + 1
    }
    mut marks = []
    mut arpeggio = false
    while i < tok.size() {
      let ch = tok.slice(i, i + 1).to_string()
      cond {
        TOUCHES.has(ch) => marks.push(TOUCHES[ch]),
        ch == "~" => {
          fail(line, "an arpeggio rolls a chord, its ~ right before the <: ~<c e g>") if tok.slice(i + 1, i + 2).to_string() != "<"
          arpeggio = true
        },
        _ => break,
      }
      i += 1
    }
    fail(line, "not a note: {tok}") if i >= tok.size()
    fail(line, 'grace notes come before the marks: {d}.c8') if tok.slice(i, i + 1).to_string() == '{'
    let rest = tok.slice(i, i + 1).to_string() == "r"
    # A rest takes a fermata, and no other mark.
    fail(line, "a rest takes no playing marks but a fermata: {tok}") if rest && (marks.any(|m| m != "fermata") || arpeggio)
    fail(line, "grace notes lead into a note, not a rest: {tok}") if rest && !graces.empty()
    fail(line, "an arpeggio rolls a chord: ~<c e g>") if arpeggio && tok.slice(i, i + 1).to_string() != "<"
    mut names = []
    mut ties = []
    if tok.slice(i, i + 1).to_string() == "<" {
      let close = tok.index_of(">")
      fail(line, "a < without its >: {tok}") if close < 0
      let inside = tok.slice(i + 1, close).to_string().split("_").map(|x| x.to_string()).filter(|x| !x.empty())
      fail(line, "a chord holds two notes or more: {tok}") if inside.size() < 2
      for t in inside {
        let (nm, td) = chord_note(t, line)
        names.push(nm)
        ties.push(td)
      }
      i = close + 1
    } else {
      let start = i
      i += 1
      if !rest {
        fail(line, "not a note: {tok}") if !"abcdefg".contains(tok.slice(start, start + 1).to_string())
        while i < tok.size() && "#b',".contains(tok.slice(i, i + 1).to_string()) {
          i += 1
        }
        names.push(tok.slice(start, i).to_string())
        ties.push(false)
      }
    }
    mut digits = ""
    while i < tok.size() && "0123456789".contains(tok.slice(i, i + 1).to_string()) {
      digits += tok.slice(i, i + 1).to_string()
      i += 1
    }
    mut dots = 0
    while i < tok.size() && tok.slice(i, i + 1).to_string() == "." {
      dots += 1
      i += 1
    }
    let tie = i < tok.size() && tok.slice(i, i + 1).to_string() == "~"
    i += 1 if tie
    fail(line, "not a note: {tok}") if i != tok.size()
    fail(line, "a rest cannot be tied") if rest && tie
    mut length = nil
    if !digits.empty() {
      let n = to_long(digits)
      fail(line, "{n} is not a note value (1 2 4 8 16 32)") if !NOTE_VALUES.contains(n)
      let base = WHOLE / n
      fail(line, "{tok} is too short to play") if base % (1 << dots) != 0
      mut total = base
      mut add = base
      for _ in 0..dots {
        add /= 2
        total += add
      }
      length = total
    } else {
      fail(line, "dots follow a note value: {tok}") if dots > 0
    }
    {rest: rest, names: names, ties: ties, length: length, tie: tie, marks: marks, graces: graces, arpeggio: arpeggio}
  }

  # The tokens of a bar: a chord `<c e g>` and grace notes `{d e}` are one
  # token with the note they belong to, a tuplet's count keeps its `[` (so
  # `/tempo 3[c8 d e]` is not a tempo of 3), and `]` and parentheses stand alone.
  fn lex(text) {
    mut s = ""
    mut inside = false
    for ch in text.iter() {
      let c = ch.to_string()
      inside = true if c == "<" || c == '{'
      s += inside && c == " " ? "_" : c
      inside = false if c == ">" || c == '}'
    }
    s = swap(s, "[", "[ ")
    for ch in ["]", "(", ")"] {
      s = swap(s, ch, " {ch} ")
    }
    s.split_whitespace().map(|w| w.to_string())
  }

  # Whether `s` is a count: digits, more than 0.
  fn is_count(s) {
    !s.empty() && all_of(s, "0123456789") && to_long(s) > 0
  }

  # Whether every character of `t` is one of `set`.
  fn all_of(t, set) {
    t.iter().map(|ch| ch.to_string()).collect().all(|ch| set.contains(ch))
  }

  fn is_number(t) {
    !t.empty() && all_of(t, "0123456789.")
  }

  let TEMPO_FORM = "a tempo is N, or a value and N: 4.=64"

  # A tempo as quarter notes a minute: `96`, or `4.=64` for a value's beats a
  # minute (a dotted quarter's here); nil for a word that is no tempo.
  fn tempo_of(t, line) {
    let parts = halves(t, "=")
    return is_number(t) ? to_float(t) : nil if parts == nil
    let (v, n) = (parts[0].to_string(), parts[1].to_string())
    fail(line, TEMPO_FORM) if !is_number(n) || v.empty() || !all_of(v, "0123456789.")
    let value = melody_token("c{v}", line).length ?? fail(line, TEMPO_FORM)
    to_float(n) * value / (WHOLE / 4)
  }

  # The items of a bar from `at`, up to a `]` or the end: notes and rests with
  # their written lengths (a value left out is the one before it in the bar,
  # and a bar starts on a beat), `3[ ... ]` tuplets, slurs and `\` directions.
  fn items_from(toks, at, cur, line) {
    mut items = []
    mut i = at
    mut value = cur
    while i < toks.size() && toks[i] != "]" {
      let t = toks[i]
      # `3[` counts its notes; `2:3[` gives a ratio, p notes in the time of q.
      let count = t.ends_with("[") ? t.slice(0, t.size() - 1).to_string() : nil
      let ratio = count == nil ? nil : halves(count, ":")
      cond {
        count != nil && ratio == nil && !is_count(count) => fail(line, "a tuplet says how many: 3[c8 d e], or a ratio: 2:3[c8 d]"),
        ratio != nil && !(is_count(ratio[0].to_string()) && is_count(ratio[1].to_string())) => fail(line, "a tuplet's ratio is two counts: 2:3[c8 d]"),
        count != nil => {
          let (group, next, after) = items_from(toks, i + 1, value, line)
          fail(line, "a [ without its ]") if next >= toks.size()
          if ratio == nil {
            let n = group.filter(|g| g.has("length") || g.has("group")).size()
            fail(line, "{t} holds {n}") if n != to_long(count)
            items.push({group: group, p: nil, q: nil})
          } else {
            let (p, q) = (to_long(ratio[0].to_string()), to_long(ratio[1].to_string()))
            fail(line, "{t}...] is too fine to play") if written_len(group) * q % p != 0
            items.push({group: group, p: p, q: q})
          }
          value = after
          i = next + 1
        },
        t == "(" || t == ")" => {
          items.push({slur: t})
          i += 1
        },
        t.starts_with('\') => {
          let word = t.slice(1, t.size()).to_string()
          let takes = word == "tempo" || word == "rit" || word == "accel"
          let arg = takes && i + 1 < toks.size() ? tempo_of(toks[i + 1], line) : nil
          fail(line, "\\{word} takes a tempo: \\{word} 100") if (word == "rit" || word == "accel") && arg == nil
          items.push({direct: word, arg: arg})
          i += arg == nil ? 1 : 2
        },
        _ => {
          let tok = melody_token(t, line)
          value = tok.length ?? value
          items.push({...tok, length: value})
          i += 1
        },
      }
    }
    (items, i, value)
  }

  # How long items sound: a tuplet with a ratio p:q sounds q/p of what is
  # written in it; one without fits into the longest plain note value (whole,
  # half, quarter...) no longer than that.
  fn written_len(items) {
    items.map(fn (it) {
      cond {
        it.has("group") => sounding(it),
        it.has("length") => it.length,
        _ => 0,
      }
    }).sum()
  }

  fn sounding(tuplet) {
    tuplet.p == nil ? fitted(tuplet.group) : written_len(tuplet.group) * tuplet.q / tuplet.p
  }

  fn fitted(items) {
    let w = written_len(items)
    mut target = WHOLE
    while target > w {
      target /= 2
    }
    target
  }

  # The notes, rests, slurs and directions of `items` in order, each note and
  # rest with the units it sounds for; a tuplet between {tuplet: units} and
  # {tuplet_end}, so that swing can keep its notes evenly spaced.
  fn timeline(items, scale_num, scale_den, line) {
    items.flat_map(fn (it) {
      cond {
        it.has("group") => [
          {tuplet: sounding(it) * scale_num / scale_den},
          ...timeline(it.group, scale_num * sounding(it), scale_den * written_len(it.group), line),
          {tuplet_end: true},
        ],
        it.has("length") => {
          let units = it.length * scale_num
          fail(line, "a tuplet this fine is too fine to play") if units % scale_den != 0
          [{...it, length: units / scale_den}]
        },
        _ => [it],
      }
    })
  }

  # A bar's notes (or a pickup's) as a timeline, and how long they last; a
  # first note without a value is a `beat` long.
  fn melody_notes(text, line, beat) {
    let toks = lex(text)
    let (items, end, _) = items_from(toks, 0, beat, line)
    fail(line, "a ] without its [") if end < toks.size()
    let events = timeline(items, 1, 1, line)
    (events, events.filter(|e| e.has("length")).map(|e| e.length).sum())
  }

  # A cresc or dim that ends at `pos`: the level ramps up to what it reaches,
  # the next dynamic or else one level on from where it began.
  fn settle(state, to, pos) {
    return nil if !state.swell
    let step = state.swell_dir == "cresc" ? 1 : -1
    let target = to ?? LEVELS[Math.clamp(LEVELS.index_of(state.level) + step, 0, LEVELS.size() - 1)]
    state.swell = false
    state.level = target
    state.points.push({at: pos, level: DYNAMICS[target], ramp: true})
  }

  # Writes `events` from unit `at` into `state`: its notes, each with how it
  # is played, the melody's top line as {at, pitch, len} (what the derived
  # parts follow), and its dynamics; tempo changes go into `tempos`. `state`
  # carries a tie, a slur and the dynamics from one bar to the next.
  fn write_melody(events, low, shift, line, at, tempos, song_tempo, state) {
    mut pos = at
    for it in events {
      cond {
        # The outermost tuplet's span, [start, end], goes with each note in it.
        it.has("tuplet") => {
          state.span = [pos, pos + it.tuplet] if state.tuplet_depth == 0
          state.tuplet_depth += 1
        },
        it.has("tuplet_end") => {
          state.tuplet_depth -= 1
          state.span = nil if state.tuplet_depth == 0
        },
        it.has("slur") => {
          fail(line, "a slur is open already; close it with ) first") if it.slur == "(" && state.slur
          fail(line, "a ) with no ( open") if it.slur == ")" && !state.slur
          state.slur = it.slur == "("
          state.slur_line = line
          # The last note under a slur is not joined to the one after it.
          state.last.legato = false if it.slur == ")" && state.last != nil
        },
        it.has("direct") => {
          let w = it.direct
          cond {
            DYNAMICS.has(w) => {
              settle(state, w, pos)
              state.level = w
              state.points.push({at: pos, level: DYNAMICS[w], ramp: false})
            },
            w == "cresc" || w == "dim" => {
              settle(state, nil, pos)
              state.points.push({at: pos, level: DYNAMICS[state.level], ramp: false})
              state.swell = true
              state.swell_dir = w
              state.swell_line = line
            },
            w == "!" => {
              fail(line, "a \\! ends a cresc or dim, and none is going") if !state.swell
              settle(state, nil, pos)
            },
            w == "tempo" => tempos[pos] = {bpm: it.arg ?? song_tempo, gradual: false},
            w == "rit" || w == "accel" => tempos[pos] = {bpm: it.arg, gradual: true},
            _ => fail(line, "not a direction: \\{w}"),
          }
        },
        it.rest => {
          state.holds.push((pos, it.length)) if it.marks.contains("fermata")
          fail(line, "a tie into a rest") if !state.ties.empty()
          state.open = nil
          pos += it.length
        },
        _ => {
          let pitches = it.names.map(|nm| melody_note(nm, low, line) + shift)
          for tp in state.ties.keys() {
            fail(line, "a tie joins two of the same note") if !pitches.contains(tp)
          }
          # Grace notes take a sixty-fourth each from the head of the note, and
          # an arpeggio starts each struck note a sixty-fourth after the one
          # below it; `shift` is how late a note starts against where it is written.
          let head = it.graces.size() * GRACE
          let struck = pitches.filter(|p| !state.ties.has(p)).sorted()
          fail(line, "a note held by a tie is not struck, and takes no marks") if struck.empty() && (!it.marks.empty() || it.arpeggio)
          fail(line, "grace notes lead into a note, not a tied one") if head > 0 && struck.size() != pitches.size()
          let late = |p| head + (it.arpeggio ? struck.index_of(p) * GRACE : 0)
          fail(line, "the note is too short for its grace notes or its arpeggio") if struck.any(|p| late(p) >= it.length)
          for k, nm in it.graces.enumerate() {
            let g = melody_note(nm, low, line) + shift
            state.notes.push({pitch: g, start: pos + k * GRACE, mut len: GRACE, line: line, shift: 0, grace: true, span: state.span, touch: nil})
          }
          # How the struck notes are played, which the slur's end can change.
          let touch = {
            mut legato: state.slur,
            staccato: it.marks.contains("staccato"),
            accent: it.marks.contains("accent"),
            tenuto: it.marks.contains("tenuto"),
            marcato: it.marks.contains("marcato"),
          }
          # Every note, tied ones lengthened.
          mut ties_now = {}
          for k, p in pitches.enumerate() {
            if state.ties.has(p) {
              state.notes[state.ties[p]].len += it.length
            } else {
              state.notes.push({pitch: p, start: pos + late(p), mut len: it.length - late(p), line: line, shift: late(p), grace: false, span: state.span, touch: touch})
            }
            let idx = state.ties.has(p) ? state.ties[p] : state.notes.size() - 1
            ties_now[p] = idx if it.tie || it.ties[k]
          }
          # The table keeps the top note, what the derived parts follow.
          let pitch = pitches.max()
          if state.ties.has(pitch) {
            state.open.len += it.length if state.open != nil
          } else {
            let at = pos + late(pitch)
            state.open = {at: at, pitch: pitch, mut len: pos + it.length - at}
            state.lead.push(state.open)
          }
          state.last = touch if !struck.empty()
          state.ties = ties_now
          # A fermata holds the whole band: its written time plays twice as long.
          state.holds.push((pos, it.length)) if it.marks.contains("fermata")
          pos += it.length
        },
      }
    }
  }

  # --- band --------------------------------------------------------------------

  let WAVES = {pulse: 0, pulse2: 1, triangle: 2, noise: 3, saw: 4}
  let DUTIES = {"1/8": 0, "1/4": 1, "1/2": 2, "3/4": 3}
  let VOICE_KEYS = ["plays", "duty", "vol", "env", "gap", "poly"]

  # A host instrument's own parameters, `name=value`, handed to the host as is.
  fn host_params(words) {
    words.filter(|x| x.contains("="))
      .map(|x| halves(x, "="))
      .map(|(pk, pv)| (pk, to_float(pv)))
      .to_object()
  }

  fn parse_voice(words, line) {
    let name = name_at(words, 1, line, "a voice")
    let host = words[2] == "host"
    let params = host_params(words)
    # `plays part NAME` (any number of them): the voice plays that part where
    # a section writes it.
    let plain = words.filter(|x| !x.contains("="))
    mut parts = []
    mut w = []
    mut pi = 0
    while pi < plain.size() {
      if plain[pi] == "plays" && pi + 1 < plain.size() && plain[pi + 1] == "part" {
        # The names that follow, one or more.
        parts.push(name_at(plain, pi + 2, line, "a part"))
        pi += 3
        while pi < plain.size() && is_name(plain[pi]) {
          parts.push(plain[pi])
          pi += 1
        }
      } else {
        w.push(plain[pi])
        pi += 1
      }
    }
    fail(line, "a voice plays on pulse, pulse2, triangle, saw or a host instrument") if w.size() < 3
    let o = options(w, host ? 4 : 3, VOICE_KEYS)
    let env = env_of(o, line)
    let vol = o.get("vol", nil) ?? fail(line, "a voice needs its vol: vol 20")
    fail(line, "vol takes a number: vol 20") if vol.size() != 1
    if host {
      fail(line, "a host voice's vol is a percentage: vol 40%") if !vol[0].ends_with("%")
    } else {
      fail(line, "a voice's vol is 0 to 100, as Audio.tone's: vol 20") if vol[0].ends_with("%")
    }
    let duty = first_of(o, "duty", "1/2")
    mut v = {
      name: name,
      host: host ? name_at(w, 3, line, "a host instrument") : nil,
      wave: host ? nil : (w[2] == "noise" ? nil : WAVES.get(w[2], nil)) ?? fail(line, "a voice plays on pulse, pulse2, triangle or saw, or a host instrument; not {w[2]}"),
      poly: o.has("poly"),
      duty: DUTIES.get(duty, nil) ?? fail(line, "a duty is 1/8, 1/4, 1/2 or 3/4: {duty}"),
      vol: host ? amount(vol[0]) : number_of(vol[0], line, "vol"),
      attack: env[0],
      decay: env[1],
      release: env[2],
      gap: option_number(o, "gap", line, 0),
      params: params,
      parts: parts,
      mut role: nil,
    }
    # A voice with no `plays` plays only what it echoes or harmonizes.
    let plays = o.get("plays", nil) ?? ["nothing"]
    match plays[0] {
      "nothing" => nil,
      "melody" => v.role = "melody",
      "roots" => {
        fail(line, "`plays roots in LO..HI`") if plays.size() != 3 || plays[1] != "in"
        v.role = "bass"
        v.low = window(plays[2], line)
      },
      "chords" => {
        v.role = "chords"
        v.rootless = plays.size() > 1 && plays[1] == "rootless"
        let rest = v.rootless ? plays.slice(2, plays.size()) : plays.slice(1, plays.size())
        fail(line, "`plays chords [rootless] from NOTE`") if rest.size() != 2 || rest[0] != "from"
        v.from = midi(rest[1], line)
      },
      what => fail(line, "a voice plays melody, roots or chords, not {what}"),
    }
    v
  }

  # `late 3/16` in steps.
  fn whole_fraction(text, line) {
    let (n, d) = halves(text, "/") ?? fail(line, "a length is N/D of a whole note")
    let steps = to_long(n) * WHOLE / to_long(d)
    fail(line, "{text} is not a whole number of steps") if steps * to_long(d) != to_long(n) * WHOLE
    steps
  }

  fn parse_band(b) {
    let name = name_at(b.head, 1, b.line, "a band")
    mut voices = {}
    mut drums = {}
    for item in b.body {
      fail(item.line, "a band holds no blocks") if item.has("head")
      # `plays hits` on a drum: it strikes on a bar's hits.
      let at_hits = iota(item.words.size()).find(|i| item.words[i] == "plays" && i + 1 < item.words.size() && item.words[i + 1] == "hits")
      let w = at_hits == nil ? item.words : [...item.words.slice(0, at_hits), ...item.words.slice(at_hits + 2, item.words.size())]
      fail(item.line, "only a drum plays hits") if at_hits != nil && item.words[0] != "drum"
      let verb = w.size() > 1 ? w[1] : ""
      cond {
        w[0] == "voice" => {
          let v = parse_voice(w, item.line)
          fail(item.line, "voice {v.name} is defined twice") if voices.has(v.name)
          # A channel plays one note at a time, so it is one voice's.
          let sharing = voices.values().find(|u| v.wave != nil && u.wave == v.wave)
          fail(item.line, "{v.name} and {sharing?.name} both play on {w[2]}") if sharing != nil
          voices[v.name] = v
        },
        # `drum NAME host SOUND [name=value...] vol P%`: a sound the program
        # supplies, played through to its end.
        w[0] == "drum" && w.size() > 2 && w[2] == "host" => {
          let dn = name_at(w, 1, item.line, "a drum")
          let plain = w.filter(|x| !x.contains("="))
          let sound = name_at(plain, 3, item.line, "a host drum's sound")
          let o = options(plain, 4, ["len", "vol", "env"])
          fail(item.line, "a host drum plays its sound through: no len or env") if o.has("len") || o.has("env")
          let vol = o.get("vol", nil) ?? fail(item.line, "a drum needs its vol")
          fail(item.line, "a host drum's vol is a percentage: vol 60%") if vol.size() != 1 || !vol[0].ends_with("%")
          drums[dn] = {
            on_hits: at_hits != nil,
            host: sound,
            params: host_params(w),
            vol: amount(vol[0]),
          }
        },
        w[0] == "drum" => {
          let dn = name_at(w, 1, item.line, "a drum")
          fail(item.line, "a drum plays on noise, or a host sound") if w.size() < 4 || w[2] != "noise"
          let o = options(w, 4, ["len", "vol", "env"])
          let (f0, f1) = halves(w[3], "->") ?? (w[3], w[3])
          let env = env_of(o, item.line)
          fail(item.line, "a noise drum needs its len and its vol: len 1  vol 20") if !o.has("len") || !o.has("vol")
          drums[dn] = {
            on_hits: at_hits != nil,
            freq: number_of(f0, item.line, "a drum's frequency"),
            end_freq: number_of(f1, item.line, "a drum's frequency"),
            dur: option_number(o, "len", item.line, 0),
            vol: option_number(o, "vol", item.line, 0),
            attack: env[0],
            decay: env[1],
            release: env[2],
          }
        },
        verb == "echoes" => {
          fail(item.line, "`V echoes SOURCE late N/D level P%`") if w.size() < 3
          let o = options(w, 3, ["late", "level"])
          fail(item.line, "an echo says how late and how loud: late 3/16  level 70%") if o.get("late", []).size() != 1 || o.get("level", []).size() != 1
          fail(item.line, "no voice {w[0]} to echo") if !voices.has(w[0])
          voices[w[0]].echo = {
            of: w[2],
            late: whole_fraction(o.late[0], item.line),
            level: amount(o.level[0]),
          }
        },
        verb == "harmonizes" => {
          fail(item.line, "`V harmonizes SOURCE under INTERVAL`") if w.size() != 5 || w[3] != "under"
          fail(item.line, "no voice {w[0]} to harmonize") if !voices.has(w[0])
          voices[w[0]].harmony = {
            of: w[2],
            under: INTERVALS.get(w[4], nil) ?? fail(item.line, "not an interval: {w[4]}"),
          }
        },
        _ => fail(item.line, "unknown band line: {w[0]}"),
      }
    }
    {name: name, voices: voices, drums: drums}
  }

  # --- grooves and sections ---------------------------------------------------

  # A groove's rows, name -> row text; `first` and `last` blocks hold rows that
  # replace the same rows in the first and the last bar.
  fn parse_groove(b) {
    let rows_of = fn (body) {
      body.filter(|it| !it.has("head")).map(fn (it) {
        let rname = name_at(it.words, 0, it.line, "a row")
        (rname, {text: it.text.slice(rname.size(), it.text.size()).to_string(), line: it.line})
      }).to_object()
    }
    mut out = {name: name_at(b.head, 1, b.line, "a groove"), rows: rows_of(b.body), mut first: {}, mut last: {}}
    for it in b.body.filter(|x| x.has("head")) {
      let which = it.head[0]
      fail(it.line, "a groove holds `first` and `last` blocks") if which != "first" && which != "last"
      out[which] = rows_of(it.body)
    }
    out
  }

  fn parse_section(b) {
    let name = name_at(b.head, 1, b.line, "a section")
    let key = b.head.size() > 3 && b.head[2] == "key" ? b.head[3] : nil
    mut groove = name
    mut named = false
    # The swing for the bars after a `swing` or `straight` line; nil keeps the
    # song's.
    mut swing = nil
    # The meter for the bars after a `meter` line; nil keeps the song's.
    mut meter = nil
    mut groove_bar = 0
    mut low = nil
    mut pickup = nil
    mut body = []
    mut endings = {}
    mut part_blocks = []
    # A bar line is its chords, then its notes. The chords are the words up
    # front that start with a capital letter (or are N.C.), and `/`, a beat more
    # of the chord before it; a bar that names none keeps the chords of the bar
    # before it in the same section or ending, and one without notes rests.
    let bar = fn (it, gname, gbar, as_is, said, before) {
      fail(it.line, "a bar is its chords, then its notes, with no |") if it.text.contains("|")
      let words = it.words
      mut n = 0
      while n < words.size() && (words[n] == "N.C." || words[n] == "/" || is_name(words[n])) {
        n += 1
      }
      if n < words.size() && words[n].ends_with(":") && ordinal(words[n], it.line) != nil {
        fail(it.line, "the time comes first: `{words[n].slice(0, words[n].size() - 1)} {words[0]}:`")
      }
      let first_bar = "the first bar of a section or an ending names its chords"
      mut named_chords = []
      for w in words.slice(0, n) {
        if w != "/" {
          named_chords.push(chord(w, it.line))
        } else if !named_chords.empty() {
          named_chords.push(named_chords[named_chords.size() - 1])
        } else {
          # A bar that starts with `/` goes on with the chord sounding.
          let held = before ?? fail(it.line, first_bar)
          named_chords.push(held[held.size() - 1])
        }
      }
      let chords = n > 0 ? named_chords : (before ?? fail(it.line, first_bar))
      {
        chords: chords,
        slashes: words.slice(0, n).contains("/"),
        notes: words.slice(n, words.size()).join(" "),
        line: it.line,
        groove: gname,
        groove_bar: gbar,
        as_written: as_is,
        named_groove: said,
        parts: {},
        again: {},
        parts_again: {},
        hits: {},
      }
    }
    # The latest time a `2nd:` line counts to, and its line.
    mut latest = nil
    let count_to = fn (nth, line) {
      latest = {n: nth, line: line} if latest == nil || nth > latest.n
    }
    # `2nd: ...` under a bar is the bar as it plays the 2nd time the song plays
    # the section; `2nd Name: ...` is part Name's notes that time.
    # The bar a `what` line goes under: the last one written, not a repeat.
    let bar_above = fn (bars_in, line, what) {
      fail(line, "{what} comes under a bar") if bars_in.empty()
      let last = bars_in[bars_in.size() - 1]
      fail(line, "a {last.repeat} bar plays as the bar it repeats; write the bar out to change it") if last.has("repeat")
      last
    }
    let add_again = fn (bars_in, it, nth) {
      let w = it.words
      let last = bar_above(bars_in, it.line, w[0])
      if w[0].ends_with(":") {
        fail(it.line, "`{w[0]}` gives the bar's chords, its notes, or both") if w.size() < 2
        let rest = {words: w.slice(1, w.size()), line: it.line, text: it.text}
        let again = bar(rest, nil, 0, false, false, last.chords)
        # Chords alone keep the bar's notes.
        last.again["{nth}"] = again.notes.empty() ? {...again, notes: last.notes, line: last.line} : again
      } else if w.size() > 1 && w[1] == "hits:" {
        fail(it.line, "`{w[0]} hits:` gives the hits that time, or . for none") if w.size() < 3
        last.hits["{nth}"] = {text: w.slice(2, w.size()).join(" "), line: it.line}
      } else {
        fail(it.line, "`{w[0]}:` for the bar, or `{w[0]} Name:` for a part") if w.size() < 2 || !w[1].ends_with(":") || !is_name(w[1])
        last.parts_again["{nth}"] ??= {}
        last.parts_again["{nth}"][w[1].strip_suffix(":")] = {notes: w.slice(2, w.size()).join(" "), line: it.line, how: "under"}
      }
      count_to(nth, it.line)
    }
    # `hits: x8 r ...` under a bar: the rhythm the band plays that bar in place
    # of its grooves.
    let add_hits = fn (bars_in, it) {
      let into = bar_above(bars_in, it.line, "hits:").hits
      fail(it.line, "hits: is written twice under one bar") if into.has("all")
      fail(it.line, "hits: gives a rhythm of x and r") if it.words.size() < 2
      into["all"] = {text: it.words.slice(1, it.words.size()).join(" "), line: it.line}
    }
    # `Name: NOTES` under a bar is that part's notes in the bar.
    let part_line = fn (it) {
      let w0 = it.words[0]
      w0.ends_with(":") && is_name(w0) ? w0.strip_suffix(":") : nil
    }
    let add_part = fn (bars_in, pname, notes, line, how) {
      let into = bar_above(bars_in, line, "{pname}:").parts
      # Blocks are added after every bar, so what is here came from a line above.
      fail(line, "{pname}: is written twice under one bar") if into.has(pname)
      into[pname] = {notes: notes, line: line, how: how}
    }
    # `%` is the bar before again, and `%%` the two bars before, as written and
    # with what is under them; nil for any other line. The copies are placed
    # where the line is (its groove bar, meter and swing) by the caller.
    let repeated = fn (bars_in, it) {
      let w = it.words
      return nil if w.size() != 1 || (w[0] != "%" && w[0] != "%%")
      let span_bars = w[0].size()
      fail(it.line, "{w[0]} repeats the {span_bars == 1 ? "bar" : "two bars"} before it in its section or ending") if bars_in.size() < span_bars
      bars_in.slice(bars_in.size() - span_bars, bars_in.size()).map(fn (src) {
        let again_parts = src.parts_again.keys().map(|t| (t, {...src.parts_again[t]})).to_object()
        {...src, line: it.line, parts: {...src.parts}, again: {...src.again}, parts_again: again_parts, hits: {...src.hits}, repeat: w[0]}
      })
    }
    # A `part NAME { ... }` block: a line a bar of its section or ending, `.`
    # for a bar the part has nothing in.
    # A `2nd: ...` line in it is the line above as the part plays it the 2nd
    # time, and is not a bar of its own.
    let add_block = fn (bars_in, blk) {
      let pname = name_at(blk.head, 1, blk.line, "a part")
      let lines = blk.body.filter(|l| !l.has("head"))
      mut k = -1
      for l in lines {
        let nth_here = ordinal(l.words[0], l.line)
        if nth_here != nil {
          fail(l.line, "in a part block, a `2nd:` line comes under the line it changes") if k < 0 || !l.words[0].ends_with(":")
          bars_in[k].parts_again["{nth_here}"] ??= {}
          bars_in[k].parts_again["{nth_here}"][pname] = {notes: l.words.slice(1, l.words.size()).join(" "), line: l.line, how: "block"}
          count_to(nth_here, l.line)
          continue
        }
        k += 1
        continue if l.text == "." || k >= bars_in.size()
        let into = bars_in[k].parts
        fail(l.line, "{pname} is written both under its bars and in a part block") if into.has(pname)
        into[pname] = {notes: l.text, line: l.line, how: "block"}
      }
      if k + 1 != bars_in.size() {
        fail(blk.line, "part {pname}'s block has {k + 1} lines for {bars_in.size()} bars")
      }
    }
    mut part_low = {}
    for it in b.body {
      if it.has("head") {
        if it.head[0] == "part" {
          part_blocks.push(it)
          continue
        }
        fail(it.line, "a section holds `ending` and `part` blocks") if it.head[0] != "ending"
        let written = it.head.slice(2, it.head.size()).join(" ") == "as written"
        mut rows = []
        mut blocks_here = []
        mut gb = groove_bar
        mut swing_here = swing
        mut meter_here = meter
        mut groove_here = groove
        mut named_here = named
        for r in it.body {
          if r.has("head") {
            fail(r.line, "an ending holds `part` blocks") if r.head[0] != "part"
            blocks_here.push(r)
            continue
          }
          if r.words[0] == "swing" || r.words[0] == "straight" {
            swing_here = swing_line(r)
            continue
          }
          if r.words[0] == "meter" {
            meter_here = parse_meter(r.words, r.line)
            continue
          }
          if r.words[0] == "groove" {
            groove_here = name_at(r.words, 1, r.line, "a groove")
            named_here = true
            gb = 0
            continue
          }
          let nth_of_row = ordinal(r.words[0], r.line)
          if nth_of_row != nil {
            add_again(rows, r, nth_of_row)
            continue
          }
          if r.words[0] == "hits:" {
            add_hits(rows, r)
            continue
          }
          let pn = part_line(r)
          if pn != nil {
            add_part(rows, pn, r.words.slice(1, r.words.size()).join(" "), r.line, "under")
            continue
          }
          for placed in repeated(rows, r) ?? [bar(r, groove_here, gb, written, named_here, rows.empty() ? nil : rows[rows.size() - 1].chords)] {
            rows.push({...placed, groove: groove_here, groove_bar: gb, as_written: written, named_groove: named_here, swing: swing_here, meter: meter_here})
            gb += 1
          }
        }
        for blk in blocks_here {
          add_block(rows, blk)
        }
        endings[it.head[1]] = rows
      } else if it.words[0] == "groove" {
        groove = name_at(it.words, 1, it.line, "a groove")
        named = true
        groove_bar = 0
      } else if it.words[0] == "swing" || it.words[0] == "straight" {
        swing = swing_line(it)
      } else if it.words[0] == "meter" {
        meter = parse_meter(it.words, it.line)
      } else if it.words[0] == "melody" {
        low = window(it.words[2], it.line)
      } else if it.words[0] == "part" {
        fail(it.line, '`part NAME in LO..HI`, or a block `part NAME {`') if it.words.size() != 4 || it.words[2] != "in"
        part_low[name_at(it.words, 1, it.line, "a part")] = window(it.words[3], it.line)
      } else if ordinal(it.words[0], it.line) != nil {
        add_again(body, it, ordinal(it.words[0], it.line))
      } else if it.words[0] == "hits:" {
        add_hits(body, it)
      } else if part_line(it) != nil {
        add_part(body, part_line(it), it.words.slice(1, it.words.size()).join(" "), it.line, "under")
      } else if it.words[0] == "pickup" {
        fail(it.line, "a pickup comes before the section's first bar") if !body.empty()
        pickup = {notes: it.words.slice(1, it.words.size()).join(" "), line: it.line}
      } else {
        for placed in repeated(body, it) ?? [bar(it, groove, groove_bar, false, named, body.empty() ? nil : body[body.size() - 1].chords)] {
          body.push({...placed, groove: groove, groove_bar: groove_bar, as_written: false, named_groove: named, swing: swing, meter: meter})
          groove_bar += 1
        }
      }
    }
    for blk in part_blocks {
      add_block(body, blk)
    }
    {name: name, key: key, low: low, part_low: part_low, pickup: pickup, body: body, endings: endings, latest: latest}
  }

  # `swing 8`, `swing 16 3:2` from word `at`: {unit (a note value), first,
  # second}, the long and the short of each pair.
  fn parse_swing(words, at, line) {
    let unit = at < words.size() ? words[at] : ""
    fail(line, "swing is on eighths or sixteenths: swing 8, swing 16") if unit != "8" && unit != "16"
    let ratio = at + 1 < words.size() ? halves(words[at + 1], ":") : ("2", "1")
    fail(line, "a swing's ratio is long:short, as 3:2") if ratio == nil || at + 2 < words.size()
    let (first, second) = (ratio[0].to_string(), ratio[1].to_string())
    fail(line, "a swing's ratio is long:short, as 3:2") if ![first, second].all(is_count) || to_long(first) <= to_long(second)
    {unit: to_long(unit), first: to_long(first), second: to_long(second)}
  }

  # `meter 3/4` as {beats, beat}, the beat in units.
  fn parse_meter(words, line) {
    let form = "a meter is N/D: meter 3/4"
    fail(line, form) if words.size() != 2
    let (top, bottom) = halves(words[1], "/") ?? fail(line, form)
    let (n, d) = (top.to_string(), bottom.to_string())
    fail(line, form) if ![n, d].all(is_count)
    fail(line, "a meter's beat is a note value: {words[1]}") if !NOTE_VALUES.contains(to_long(d))
    {beats: to_long(n), beat: WHOLE / to_long(d)}
  }

  # A section's `swing ...` line, or `straight`.
  fn swing_line(it) {
    return {straight: true} if it.words[0] == "straight" && it.words.size() == 1
    fail(it.line, "`straight` stands alone") if it.words[0] == "straight"
    parse_swing(it.words, 1, it.line)
  }

  # `2nd` or `2nd:` as 2. A word that starts with digits and goes on in letters
  # is an ordinal, and has to be spelt as one.
  fn ordinal(w, line) {
    let word = w.strip_suffix(":")
    mut d = 0
    while d < word.size() && "0123456789".contains(word.slice(d, d + 1).to_string()) {
      d += 1
    }
    let suffix = word.slice(d, word.size()).to_string()
    return nil if d == 0 || suffix.empty() || !all_of(suffix, "abcdefghijklmnopqrstuvwxyz")
    let n = to_long(word.slice(0, d).to_string())
    let want = cond {
      n % 100 >= 11 && n % 100 <= 13 => "th",
      n % 10 == 1 => "st",
      n % 10 == 2 => "nd",
      n % 10 == 3 => "rd",
      _ => "th",
    }
    fail(line, "the {n}{want} time is written {n}{want}, not {word}") if suffix != want
    fail(line, "times count from 1st") if n == 0
    n
  }

  fn parse_song(b) {
    mut song = {name: name_at(b.head, 1, b.line, "a song"), line: b.line, mut loop: false, plays: [], part_low: {}}
    for it in b.body {
      let w = it.words
      match w[0] {
        "tempo" => song.tempo = (w.size() > 1 ? tempo_of(w[1], it.line) : nil) ?? fail(it.line, TEMPO_FORM),
        "meter" => {
          let m = parse_meter(w, it.line)
          song.beats = m.beats
          song.beat = m.beat
        },
        "key" => song.key = w[1],
        "band" => song.band = w[1],
        "groove" => song.groove = name_at(w, 1, it.line, "a groove"),
        "part" => {
          fail(it.line, "`part NAME in LO..HI`") if w.size() != 4 || w[2] != "in"
          song.part_low[name_at(w, 1, it.line, "a part")] = window(w[3], it.line)
        },
        "melody" => song.low = window(w[2], it.line),
        "loop" => song.loop = true,
        "swing" => song.swing = parse_swing(w, 1, it.line),
        "mark" => song.plays.push({mark: name_at(w, 1, it.line, "a mark")}),
        _ => {
          let sname = name_at(w, 0, it.line, "a section to play")
          let keys = ["with", "ending", "in"]
          let o = options(w, 1, keys)
          for key in keys {
            fail(it.line, "`{key}` is said once a line") if w.filter(|t| t == key).size() > 1
          }
          fail(it.line, "`xN` is said once a line") if w.filter(|t| t.starts_with("x") && t.size() > 1).size() > 1
          let times = w.find(|t| t.starts_with("x") && t.size() > 1)
          song.plays.push({
            section: sname,
            with: first_of(o, "with", nil),
            ending: first_of(o, "ending", nil),
            key: first_of(o, "in", nil),
            times: times == nil ? 1 : to_long(times.slice(1, times.size()).to_string()),
            line: it.line,
          })
        },
      }
    }
    song
  }

  # What `about { ... }` says of the song: each line an item and its text, to
  # the end of the line; `year` a number.
  let ABOUT = ["title", "composer", "lyricist", "arranger", "year", "source", "license", "note"]
  fn parse_about(b) {
    fail(b.line, "about takes no name") if b.head.size() != 1
    mut about = {}
    for it in b.body {
      fail(it.line, "about holds lines, not blocks") if it.has("head")
      let key = it.words[0]
      fail(it.line, "about says {ABOUT.join(", ")}, not {key}") if !ABOUT.contains(key)
      fail(it.line, "about says {key} once") if about.has(key)
      let text = it.text.slice(key.size(), it.text.size()).trim().to_string()
      fail(it.line, "{key} needs its text: {key} ...") if text.empty()
      fail(it.line, "a year is a number: {text}") if key == "year" && !all_of(text, "0123456789")
      about[key] = key == "year" ? to_long(text) : text
    }
    about
  }

  # Bar `r` as it plays the `nth` time its section plays: its `2nd:` line if it
  # has one for that time, and its parts with that time's lines over them (a
  # `.` there leaves the part out).
  fn as_played(r, nth) {
    let again = r.again.get(nth, nil) ?? r
    let parts = {...r.parts, ...(r.parts_again.get(nth, nil) ?? {})}
    let kept = parts.keys().filter(|pn| parts[pn].notes != ".").map(|pn| (pn, parts[pn])).to_object()
    let hits = r.hits.get(nth, nil) ?? r.hits.get("all", nil)
    {...r, chords: again.chords, slashes: again.slashes, notes: again.notes, line: again.line, parts: kept,
     band_hits: hits == nil || hits.text == "." ? nil : hits}
  }

  # --- the whole song -----------------------------------------------------------

  # The top-level items of `src`, each `use 'file'` replaced by that file's.
  # `load(name, from)` reads the file `name` names from the file `from`, and
  # answers its text and where it is.
  fn tops(src, file, load, used) {
    blocks(src, file).flat_map(fn (top) {
      fail(top.line, "a file that is used holds no song") if used && top.has("head") && top.head[0] == "song"
      fail(top.line, "a file that is used holds no about") if used && top.has("head") && top.head[0] == "about"
      return [top] if top.has("head") || top.words[0] != "use"
      let quoted = top.words.size() == 2 ? top.words[1] : ""
      fail(top.line, "use takes a quoted file name: use 'band.kau'") if quoted.size() < 3 || !quoted.starts_with("'") || !quoted.ends_with("'")
      let (text, name) = load(quoted.slice(1, quoted.size() - 1).to_string(), file, top.line)
      tops(text, name, load, true)
    })
  }

  fn parse(src, file, load) {
    mut out = {bands: {}, grooves: {}, sections: {}, songs: [], mut about: nil}
    qualities = {...CLOSE}
    # Two of a kind may not share a name; a groove and a section may.
    let define = fn (kind, table, name, line) {
      fail(line, "{kind} {name} is defined twice") if table.has(name)
    }
    for top in tops(src, file, load, false) {
      fail(top.line, "unknown line: {top.words[0]}") if !top.has("head")
      match top.head[0] {
        "band" => {
          let bd = parse_band(top)
          define("band", out.bands, bd.name, top.line)
          out.bands[bd.name] = bd
        },
        "groove" => {
          let g = parse_groove(top)
          define("groove", out.grooves, g.name, top.line)
          out.grooves[g.name] = g
        },
        "section" => {
          let sec = parse_section(top)
          define("section", out.sections, sec.name, top.line)
          out.sections[sec.name] = sec
        },
        "song" => out.songs.push(parse_song(top)),
        "about" => {
          fail(top.line, "a file says about once") if out.about != nil
          out.about = parse_about(top)
        },
        # `voicings { NAME INTERVAL... }` adds chord qualities to the table.
        "voicings" => {
          for it in top.body {
            fail(it.line, "a voicing is a name and its intervals") if it.has("head") || it.words.size() < 3
            # `Cb9` is C flat's 9, so a quality may not start as an accidental does.
            fail(it.line, "a quality does not start with # or b: {it.words[0]}") if "#b".contains(it.words[0].slice(0, 1).to_string())
            fail(it.line, "{it.words[0]} is already a chord quality") if qualities.has(it.words[0]) || SPELLINGS.has(it.words[0])
            qualities[it.words[0]] = it.words.slice(1, it.words.size()).map(|x| to_long(x))
          }
        },
        k => fail(top.line, "unknown block {k}"),
      }
    }
    out
  }

  # The file's song expanded: its notes, rows and hits in units, and what
  # times them.
  fn expand(src, file, load) {
    let s = parse(src, file, load)
    fail(where(file, 1), "a file that plays holds one song") if s.songs.size() != 1
    let song = s.songs[0]
    fail(song.line, "a song needs its tempo: tempo 120") if !song.has("tempo")
    fail(song.line, "a song needs its band: band NAME") if !song.has("band")
    let band = s.bands.get(song.band, nil) ?? fail(song.line, "no band {song.band}")
    let voices = band.voices
    let role = fn (which) {
      voices.values().find(|v| v.role == which)
    }
    let bass_v = role("bass") ?? {name: nil}
    let chord_v = role("chords") ?? {name: nil}
    let melody_v = role("melody") ?? fail(song.line, "no voice of {band.name} plays melody")
    let singers = voices.values().filter(|v| v.role == "melody").map(|v| v.name).collect()
    fail(song.line, "one voice of {band.name} plays melody, not {singers.join(" and ")}") if singers.size() > 1
    # The voices that play a derived part, which follow the melody's voice.
    let derived = voices.values().filter(|v| v.has("echo") || v.has("harmony")).collect()
    for v in derived {
      for d in [v.get("echo", nil), v.get("harmony", nil)] {
        fail(song.line, "{v.name} follows {d.of}, and the melody is {melody_v.name}'s") if d != nil && d.of != melody_v.name
      }
    }
    let song_key = key_pc(song.key ?? "C", song.line)
    let BEATS = song.get("beats", 4)
    let BEAT = song.get("beat", WHOLE / 4)

    # Every bar the song plays, in order, each from unit `at` for `len` units
    # in its own meter.
    mut bars = []
    mut marks = {}
    let fresh_state = fn () {
      return {
        mut ties: {}, notes: [], mut slur: false, mut slur_line: 0, mut level: "mf", mut swell: false, mut swell_dir: nil, mut swell_line: 0, mut last: nil, mut tuplet_depth: 0, mut span: nil, holds: [], points: [], lead: [], mut open: nil,
      }
    }
    let voice_state = fresh_state()
    let song_tempo = song.tempo
    mut tempos = {}
    mut part_states = {}
    # Where the next bar starts.
    mut pos = 0
    # Whether the melody rests for `units` from `at`, where a pickup goes.
    let rests = |at, units| !voice_state.lead.any(|o| o.at < at + units && o.at + o.len > at)
    # How many times the song has played each section so far.
    mut played = {}
    for p in song.plays {
      if p.has("mark") {
        marks[p.mark] = bars.size()
        continue
      }
      let sec = s.sections.get(p.section, nil) ?? fail(p.line, "no section {p.section}")
      mut rows = sec.body
      if !sec.endings.empty() {
        let n = p.ending ?? fail(p.line, "{p.section} has endings; say which")
        rows = [...rows, ...(sec.endings.get(n, nil) ?? fail(p.line, "no ending {n}"))]
      }
      let from = sec.key == nil ? song_key : key_pc(sec.key, p.line)
      let up = p.key == nil ? 0 : Math.wrap(key_pc(p.key, p.line) - from, 12)
      let shift = up > 6 ? up - 12 : up
      let low = sec.low ?? song.low ?? fail(p.line, "say `melody in LO..HI`")
      for _ in 0..p.times {
        played[p.section] = played.get(p.section, 0) + 1
        let nth = "{played[p.section]}"
        if sec.pickup != nil {
          let pk = sec.pickup
          # The pickup's first note without a value is a beat of the bar it
          # leads into.
          let into = rows[0].meter ?? {beats: BEATS, beat: BEAT}
          let (events, len) = melody_notes(pk.notes, pk.line, into.beat)
          if bars.empty() {
            # At the very start the song begins with the pickup: a bar of its
            # own length, before the first, that the band does not play in.
            write_melody(events, low, shift, pk.line, 0, tempos, song_tempo, voice_state)
            bars.push({lead_in: true, at: 0, len: len, beats: 0, beat: into.beat, chords: [nil], parts: {}, line: pk.line, first: false, last: false, swing: nil})
            pos = len
          } else {
            let before = bars[bars.size() - 1]
            fail(pk.line, "the pickup is longer than the bar before it") if len > before.len
            let from_at = pos - len
            fail(pk.line, "the bar before the pickup has to rest where the pickup plays") if !rests(from_at, len) || !voice_state.ties.empty()
            write_melody(events, low, shift, pk.line, from_at, tempos, song_tempo, voice_state)
          }
        }
        for i, written_bar in rows.enumerate() {
          let r = as_played(written_bar, nth)
          let n = r.as_written ? 0 : shift
          fail(r.line, "a bar line holds one bar") if r.notes.contains("|")
          let m = r.meter ?? {beats: BEATS, beat: BEAT}
          let blen = m.beats * m.beat
          fail(r.line, "with slashes, a bar gives a chord or a / for each of its {m.beats} beats") if r.slashes && r.chords.size() != m.beats
          fail(r.line, "{r.chords.size()} chords do not share {m.beats} beats evenly") if m.beats % r.chords.size() != 0
          let base = pos
          # A bar of chords alone is a bar of rest for the melody.
          let written_notes = r.notes.empty() ? repeat(m.beats, "r{WHOLE / m.beat}").join(" ") : r.notes
          let (events, len) = melody_notes(written_notes, r.line, m.beat)
          if len != blen {
            fail(r.line, "the bar is {len * 1.0 / m.beat} beats long, want {m.beats}")
          }
          write_melody(events, low, n, r.line, base, tempos, song_tempo, voice_state)
          for pname, pt in r.parts {
            if !part_states.has(pname) {
              part_states[pname] = fresh_state()
            }
            let (pev, plen) = melody_notes(pt.notes, pt.line, m.beat)
            if plen != blen {
              fail(pt.line, "{pname}'s bar is {plen * 1.0 / m.beat} beats long, want {m.beats}")
            }
            let plow = sec.part_low.get(pname, nil) ?? song.part_low.get(pname, nil) ?? low
            write_melody(pev, plow, n, pt.line, base, tempos, song_tempo, part_states[pname])
          }
          bars.push({
            ...r,
            at: base,
            len: blen,
            beats: m.beats,
            beat: m.beat,
            chords: r.chords.map(|cd| transposed(cd, n)),
            first: i == 0,
            last: i == rows.size() - 1,
            with: p.with,
            with_line: p.line,
            # The section's swing, else the song's; `straight` none.
            swing: r.swing == nil ? song.get("swing", nil) : (r.swing.has("straight") ? nil : r.swing),
          })
          pos += blen
        }
      }
    }
    let lead_in = bars.size() > 0 && bars[0].has("lead_in")
    # In a song that loops, a pickup at the start plays again over the end.
    if lead_in && song.loop {
      let opening = bars[0].len
      fail(bars[0].line, "the song loops, so its last bar has to rest where the pickup plays") if !rests(pos - opening, opening)
    }

    # What the song leaves unfinished at its end.
    let unfinished = fn (st, what) {
      for _, held in st.ties {
        fail(st.notes[held].line, "{what}: a tie at the end of the song, with no note to tie to")
      }
      fail(st.slur_line, "{what}: a slur the song never closes") if st.slur
      fail(st.swell_line, "{what}: a {st.swell_dir} that nothing ends") if st.swell
    }
    unfinished(voice_state, "the melody")
    for pname, st in part_states {
      unfinished(st, "part {pname}")
    }

    # A line's level at unit `t` from its dynamics points: the level of the last
    # point at or before `t`, or a ramp from it to the next point when that one
    # ends a cresc or dim; nil before the line's first point.
    let points_of = |st| st.points.sorted_by(|pt| pt.at)
    let level_on = fn (pts, t) {
      mut i = -1
      while i + 1 < pts.size() && pts[i + 1].at <= t {
        i += 1
      }
      return nil if i < 0
      let here = pts[i]
      if i + 1 < pts.size() && pts[i + 1].ramp {
        let ahead = pts[i + 1]
        return here.level + (ahead.level - here.level) * (t - here.at) * 1.0 / (ahead.at - here.at)
      }
      here.level
    }
    # The dynamics in the bar lines are the band's: everything plays at this
    # level, a song starting at mf.
    let band_points = points_of(voice_state)
    let band_level = |t| level_on(band_points, t) ?? DYNAMICS["mf"]
    # A part's own dynamics set that part alone, from where they stand until the
    # band's next one.
    let part_points = part_states.keys().map(|pn| (pn, points_of(part_states[pn]))).to_object()
    let last_at = fn (pts, t) {
      let prior = pts.filter(|pt| pt.at <= t)
      prior.empty() ? -1 : prior[prior.size() - 1].at
    }
    let part_level = fn (pname, t) {
      let own = part_points.get(pname, [])
      let mine = last_at(own, t)
      return band_level(t) if mine < 0 || mine < last_at(band_points, t)
      level_on(own, t)
    }

    for sname, sec in s.sections {
      let last = sec.latest
      continue if last == nil || !played.has(sname) || last.n <= played[sname]
      fail(last.line, "{sname} plays {played[sname]} times, so this time never comes")
    }

    for b in bars {
      continue if b.swing == nil
      let span = 2 * WHOLE / b.swing.unit
      fail(b.line, "swing {b.swing.unit} pairs notes, and a bar of {b.beats} beats is not whole pairs") if b.len % span != 0
    }

    let steps = pos
    # The bar that unit `t` falls in.
    let bar_index = fn (t) {
      mut lo = 0
      mut hi = bars.size() - 1
      while lo < hi {
        let mid = (lo + hi + 1) / 2
        if bars[mid].at <= t {
          lo = mid
        } else {
          hi = mid - 1
        }
      }
      lo
    }
    # When written time `t` (units from the song's start) is heard. Under swing,
    # each pair of the swing's notes, counted from the bar's start, plays its
    # first half in first/(first+second) of the pair and its second half in the
    # rest, and a time inside a half moves in proportion.
    let swung_time = fn (t) {
      let b = bars[bar_index(Math.min(t, steps - 1))]
      let sw = b.swing
      return t * 1.0 if sw == nil
      let pair = 2 * WHOLE / sw.unit
      let half = pair / 2
      let x = (t - b.at) % pair
      let start = t - x
      let long = pair * 1.0 * sw.first / (sw.first + sw.second)
      x <= half ? start + x * long / half : start + long + (x - half) * (pair - long) / half
    }
    # A note as heard, {pitch, start, len}: a note in a tuplet keeps its place
    # evenly between where the tuplet starts and ends are heard.
    let heard = fn (nt) {
      let at = fn (t) {
        return swung_time(t) if nt.span == nil || t < nt.span[0] || t > nt.span[1]
        let (s0, s1) = (nt.span[0], nt.span[1])
        swung_time(s0) + (t - s0) * (swung_time(s1) - swung_time(s0)) / (s1 - s0)
      }
      let start = at(nt.start)
      {pitch: nt.pitch, start: start, len: at(nt.start + nt.len) - start}
    }

    # Who plays each part; in a bar where a part is written, its players play
    # it instead of their groove rows.
    let players = fn (pname) {
      voices.keys().filter(|vn| voices[vn].parts.contains(pname))
    }
    # Notes that sound together need a voice that plays several at once.
    let together = fn (notes) {
      let by_start = notes.sorted_by(|nt| nt.start)
      mut end = -1
      mut clash = nil
      for nt in by_start {
        clash = clash ?? (nt.start < end ? nt : nil)
        end = Math.max(end, nt.start + nt.len)
      }
      clash
    }
    let melody_clash = together(voice_state.notes)
    if melody_clash != nil && !melody_v.poly {
      fail(melody_clash.line, "the melody sounds notes together, and {melody_v.name} is not poly")
    }
    for pname, st in part_states {
      let part_clash = together(st.notes)
      for vn in players(pname) {
        if part_clash != nil && !voices[vn].poly {
          fail(part_clash.line, "part {pname} sounds notes together, and {vn} is not poly")
        }
      }
    }
    let busy = fn (bar, vname) {
      return false if vname == nil
      bar.parts.keys().any(|pname| voices[vname].parts.contains(pname))
    }
    let lead = voice_state.lead
    let chord_at = fn (bar, slot) {
      bar.chords[Math.min(slot * bar.chords.size() / bar.len, bar.chords.size() - 1)]
    }
    let bass_note = fn (cd) {
      at_or_above(cd.bass, bass_v.low)
    }
    # The tokens of row `rname` for bar `b` ({size, toks}), and the row's
    # line: its `first` or `last` row if it has one there, else the groove's
    # own, cycling through its bars. A row is read once for each bar length.
    mut grids = {}
    let row_steps = fn (bar, gr, rname, is_hit) {
      return nil if rname == nil
      let own = cond {
        bar.first && gr.first.has(rname) => gr.first[rname],
        bar.last && gr.last.has(rname) => gr.last[rname],
        gr.rows.has(rname) => gr.rows[rname],
        _ => nil,
      }
      return nil if own == nil
      let key = "{own.line} {bar.len}"
      grids[key] ??= grid(own.text, is_hit, own.line, bar.len)
      let all = grids[key]
      let at = own == gr.rows.get(rname, nil) ? bar.groove_bar % all.size() : 0
      (all[at], own.line)
    }

    # A bar's hits as [{at, len, accent, staccato}] from its start: a rhythm of
    # `x` (the band strikes) and `r` (it rests), read as notes are, a tie
    # holding a hit on.
    let hits_of = fn (b) {
      let h = b.band_hits
      let toks = lex(h.text)
      let written = toks.map(fn (t) {
        return t if t == "]" || t.ends_with("[")
        mut i = 0
        while i < t.size() && ".!".contains(t.slice(i, i + 1).to_string()) {
          i += 1
        }
        let head = t.slice(i, i + 1).to_string()
        fail(h.line, "hits are x (strike) and r (rest), with a value and the marks ! and .: {t}") if head != "x" && head != "r"
        head == "x" ? t.slice(0, i).to_string() + "c" + t.slice(i + 1, t.size()).to_string() : t
      }).join(" ")
      let (events, len) = melody_notes(written, h.line, b.beat)
      fail(h.line, "the hits are {len * 1.0 / b.beat} beats long, want {b.beats}") if len != b.len
      mut out = []
      mut at_unit = 0
      mut tied = false
      for ev in events {
        continue if !ev.has("length")
        if !ev.rest {
          if tied {
            out[out.size() - 1].len += ev.length
          } else {
            out.push({at: at_unit, mut len: ev.length, accent: ev.marks.contains("accent"), staccato: ev.marks.contains("staccato")})
          }
          tied = ev.tie
        }
        at_unit += ev.length
      }
      out
    }
    # Units offset `offset` in bar `nc_bar`, at most `most`, before the bar goes to
    # N.C., which silences the voices that play roots and chords.
    let before_nc = fn (nc_bar, offset, most) {
      let shares = nc_bar.chords.size()
      mut share = offset * shares / nc_bar.len
      while share < shares && share * nc_bar.len / shares < offset + most {
        return Math.max(0, share * nc_bar.len / shares - offset) if nc_bar.chords[share] == nil
        share += 1
      }
      most
    }
    # A row's notes: each starts on its token and lasts through the `-` tokens
    # after it, across bar lines, as a melody note does. `open` is the note
    # the next `-` holds on, if any.
    let row_voice = fn (voice) {
      {voice: voice, notes: [], mut open: nil}
    }
    let hold = fn (rv, in_bar, offset, units) {
      let o = rv.open
      if o == nil || o.at + o.len != in_bar.at + offset {
        rv.open = nil
        return nil
      }
      let kept = before_nc(in_bar, offset, units)
      o.len += kept
      rv.open = nil if kept < units
    }
    let strike = fn (rv, in_bar, offset, units, pitches, accent) {
      let o = {at: in_bar.at + offset, mut len: 0, pitches: pitches, accent: accent}
      rv.notes.push(o)
      rv.open = o
      hold(rv, in_bar, offset, units)
    }
    let bass_row = row_voice(bass_v)
    let chord_row = row_voice(chord_v)
    # Drum hits: a noise drum's by unit (the channel sounds one at a time), and
    # the host drums', which sound together.
    mut noise_hits = {}
    mut host_hits = []
    let hit_drum = fn (dn, hit_bar, offset, line) {
      let step = hit_bar.at + offset
      if band.drums[dn].has("host") {
        host_hits.push({at: step, name: dn})
      } else {
        let beat = 1 + offset * 1.0 / hit_bar.beat
        fail(line, "{noise_hits[step]} and {dn} both hit at beat {beat} of the bar at {hit_bar.line}, on the noise channel") if noise_hits.has(step)
        noise_hits[step] = dn
      }
    }
    # A derived part needs its voice.
    for b in bars {
      let w = b.get("with", nil)
      if w != nil && !derived.any(|v| v.has(w)) {
        fail(b.with_line, "`with {w}` needs a voice that {w == "echo" ? "echoes" : "harmonizes"} the melody")
      }
    }
    for bi, b in bars.enumerate() {
      # A bar plays the groove its section names; else the groove of the
      # section's own name; else the song's; else no accompaniment.
      let g = cond {
        # The band does not play under a pickup that starts the song.
        b.has("lead_in") => {rows: {}, first: {}, last: {}},
        b.named_groove => s.grooves.get(b.groove, nil) ?? fail(b.line, "no groove {b.groove}"),
        s.grooves.has(b.groove) => s.grooves[b.groove],
        song.get("groove", nil) != nil => s.grooves.get(song.groove, nil) ?? fail(song.line, "no groove {song.groove}"),
        _ => {rows: {}, first: {}, last: {}},
      }
      # In a bar with hits the band plays them instead of its grooves: the bass
      # the chord's bass note, the chord voice the whole chord, and the drums
      # that play hits strike; every other drum rests.
      let hit_plan = b.has("band_hits") && b.band_hits != nil ? hits_of(b) : nil
      if hit_plan != nil {
        for h in hit_plan {
          for dn in band.drums.keys() {
            continue if !band.drums[dn].on_hits
            hit_drum(dn, b, h.at, b.band_hits.line)
          }
        }
      } else {
        for dn in band.drums.keys() {
          let found = row_steps(b, g, dn, true)
          continue if found == nil
          let (row, row_line) = found
          for k, t in row.toks.enumerate() {
            continue if t == "."
            fail(row_line, "{dn} hits with `{t}`, not x, under the bar at {b.line}") if t != "x"
            hit_drum(dn, b, k * row.size, row_line)
          }
        }
      }
      # The bar the song plays next; the last bar of a song that does not loop
      # has none, and its `>` and `<` play the chord's own 1.
      let next = cond {
        bi + 1 < bars.size() => bars[bi + 1],
        song.loop => bars[lead_in ? 1 : 0],
        _ => nil,
      }
      # What a voice plays in the bar, as (slot, token, units, accent): its
      # groove row, token by token; or the hits, each held for its length (half
      # of it under a staccato); or nothing, where it plays a part.
      let pattern = fn (vname) {
        return [] if vname == nil || busy(b, vname)
        if hit_plan != nil {
          # A chord voice that is not poly takes the chord's top tone.
          let tok = vname == bass_v.name ? "1" : (chord_v.poly ? "x" : "99")
          return hit_plan.map(|h| (h.at, tok, h.staccato ? Math.max(1, h.len / 2) : h.len, h.accent))
        }
        let voice_row = row_steps(b, g, vname, false)
        return [] if voice_row == nil
        let row = voice_row[0]
        row.toks.enumerate().map(|(k, t)| (k * row.size, t, row.size, false))
      }
      for (slot, hit, units, accent) in pattern(bass_v.name) {
        let c = chord_at(b, slot)
        cond {
          hit == "." || c == nil => bass_row.open = nil,
          hit == "-" => hold(bass_row, b, slot, units),
          _ => {
            let low = bass_note(c)
            let p = match hit {
              "1" => low,
              "8" => low + 12,
              ">" => next == nil ? low : bass_note(next.chords[0] ?? fail(b.line, "> leads into N.C.")) - 1,
              "<" => next == nil ? low : bass_note(next.chords[0] ?? fail(b.line, "< leads into N.C.")) + 1,
              _ => at_or_above(c.root + degree(c, hit, b.line), low),
            }
            strike(bass_row, b, slot, units, [p], accent)
          },
        }
      }
      for (slot, pick, units, accent) in pattern(chord_v.name) {
        let c = chord_at(b, slot)
        cond {
          pick == "." || c == nil => chord_row.open = nil,
          pick == "-" => hold(chord_row, b, slot, units),
          _ => {
            let tones = voiced(chord_v, c)
            let struck = pick == "x" ? tones : [tones[Math.min(to_long(pick) - 1, tones.size() - 1)]]
            strike(chord_row, b, slot, units, chord_v.poly ? struck : [struck[0]], accent)
          },
        }
      }
    }
    # The derived parts, from the melody's notes: an echo sounds a note where
    # it falls `late` after one, in a bar played `with echo` (in a looping song
    # the first bars echo the last); a harmony sounds with each note of a bar
    # played `with harmony`.
    let second = derived.map(|v| ({voice: v, notes: []}))
    for entry in lead {
      let (i, sung, len) = (entry.at, entry.pitch, entry.len)
      let sung_bar = bars[bar_index(i)]
      let c = chord_at(sung_bar, i - sung_bar.at)
      for dv in second {
        if dv.voice.has("echo") {
          let e = dv.voice.echo
          let echo_at = Math.wrap(i + e.late, steps)
          dv.notes.push({at: echo_at, len: len, pitches: [sung], level: e.level}) if bars[bar_index(echo_at)].get("with", nil) == "echo"
        }
        if dv.voice.has("harmony") && sung_bar.get("with", nil) == "harmony" && c != nil {
          let under = dv.voice.harmony.under
          let tones = voiced(chord_v.name == nil ? {rootless: false, from: 60} : chord_v, c)
          dv.notes.push({at: i, len: len, pitches: [tones.map(|t| sung - under - Math.wrap(sung - under - t, 12)).max()], level: 1.0})
        }
      }
    }
    # Fermatas from the melody and the parts; two at one time are one.
    let all_holds = [voice_state, ...part_states.keys().map(|k| part_states[k])].flat_map(|st| st.holds)
    mut holds = []
    for h in all_holds.sorted_by(|x| x[0] * steps + x[1]) {
      holds.push(h) if holds.empty() || holds[holds.size() - 1] != h
    }
    {
      song: song,
      about: s.about ?? {},
      voices: voices,
      drums: band.drums,
      # Where each mark is, as the bar it comes before.
      marks: marks.keys().map(|k| (k, marks[k] < bars.size() ? bars[marks[k]].at : steps)).to_object(),
      # Where the song starts again when it loops: after a pickup that starts it.
      loop_at: lead_in ? bars[1].at : 0,
      # The pickup that starts the song, which a song that loops plays again over
      # its last bar.
      opening: lead_in ? bars[0].len : 0,
      steps: steps,
      melody: {voice: melody_v, notes: voice_state.notes},
      parts: part_states.keys().map(|pname| (pname, {
        notes: part_states[pname].notes,
        players: players(pname),
      })).to_object(),
      # What the groove's voices and the derived parts play, {voice, notes}
      # with each note {at, len, pitches, accent, level}, and the drums' hits,
      # {at, name}; in units.
      rows: [...second, {voice: bass_v, notes: bass_row.notes}, {voice: chord_v, notes: chord_row.notes}],
      hits: [
        ...noise_hits.keys().map(|u| ({at: u, name: noise_hits[u]})),
        ...host_hits,
      ],
      # Written times under a fermata, (start, length): each plays twice as
      # long, the whole band held with it.
      holds: holds,
      tempo: song.tempo,
      tempos: tempos,
      # The band's level at unit t (grooves, drums and derived parts play at it,
      # the melody too), and a part's, which its own dynamics can set.
      level_at: band_level,
      part_level_at: part_level,
      swung: swung_time,
      heard: heard,
    }
  }

  # --- playing -------------------------------------------------------------------

  # A voice's `vol` is its level at mf.
  let MF = DYNAMICS["mf"]

  # How a groove's voice plays a hit written with `!`.
  let ACCENT = {legato: false, staccato: false, accent: true, tenuto: false, marcato: false}

  # A level louder than `level`, as an accent plays it.
  fn louder(level) {
    LEVELS.map(|l| DYNAMICS[l]).find(|x| x > level + 0.001) ?? level * 1.15
  }

  # A MIDI note's frequency.
  fn hz(p) {
    440.0 * Math.exp((p - 69) / 12.0 * Math.log(2.0))
  }

  # Seconds from the song's start to written unit `u` (a Float between units
  # too): the tempo, with its ritardandos and accelerandos, and each fermata's
  # time played twice.
  fn clock(x) {
    # The tempo as segments from unit `from` to `to`, `b0` beats a minute at
    # the start and `b1` at the end.
    mut segs = []
    mut from = 0
    mut bpm = x.tempo
    mut ramp = nil
    for p in x.tempos.keys().sorted() {
      let change = x.tempos[p]
      segs.push({from: from, to: p, b0: bpm, b1: ramp ?? bpm}) if p > from
      # A ramp reaches its tempo at the next direction, which sets its own.
      bpm = ramp ?? bpm
      ramp = change.gradual ? change.bpm : nil
      bpm = change.gradual ? bpm : change.bpm
      from = p
    }
    segs.push({from: from, to: Math.max(from + 1, x.steps), b0: bpm, b1: ramp ?? bpm})
    # Seconds from a segment's start to `u` in it: a unit is a 480th of a beat.
    let within = fn (sg, u) {
      let d = Math.min(u, sg.to) - sg.from
      let slope = (sg.b1 - sg.b0) * 1.0 / (sg.to - sg.from)
      let ramped = sg.b0 == sg.b1
        ? 60.0 * d / (sg.b0 * WHOLE / 4)
        : 60.0 / (WHOLE / 4) / slope * Math.log((sg.b0 + slope * d) / sg.b0)
      # Past its end (a note ringing on after the last bar) the tempo holds.
      ramped + 60.0 * Math.max(0, u - sg.to) / (sg.b1 * WHOLE / 4)
    }
    mut starts = []
    mut total = 0.0
    for sg in segs {
      starts.push(total)
      total += within(sg, sg.to)
    }
    let tempo_time = fn (u) {
      mut i = segs.size() - 1
      while i > 0 && u < segs[i].from {
        i -= 1
      }
      starts[i] + within(segs[i], u)
    }
    fn (u) {
      mut t = tempo_time(u)
      for h in x.holds {
        let (s, n) = h
        t += tempo_time(Math.max(s, Math.min(u, s + n))) - tempo_time(s)
      }
      t
    }
  }

  # What the song plays, in order: {at, len, by, pitch, vol} for each note and
  # hit, `at` and `len` in seconds, `by` the voice or drum, `pitch` a MIDI note
  # (nil for a drum) and `vol` its level (a host sound's as a fraction of its
  # own). A tone carries what `Audio.tone` needs, and a host sound its key.
  fn render(x) {
    let time = clock(x)
    let at = |unit| time(x.swung(unit))
    mut out = []
    let band_level = x.level_at
    # Voice `v`'s note, heard from `t0` to `t1` seconds, at `level` (a dynamic);
    # `touch` is how a written note is played, nil for one a groove plays.
    let note = fn (v, pitch, t0, t1, level, touch) {
      let loud = touch != nil && (touch.accent || touch.marcato) ? louder(level) : level
      let len = t1 - t0
      if v.host != nil {
        out.push({at: t0, len: len, by: v.name, pitch: pitch, vol: v.vol * loud / MF, sound: "{v.name} {pitch}"})
        return nil
      }
      let vol = v.vol * loud / MF
      let sounded = cond {
        touch == nil => len,
        touch.staccato => len / 2,
        touch.marcato => len * 3 / 4,
        _ => len,
      }
      let joined = touch != nil && (touch.legato || touch.tenuto)
      let sustain = Math.max(0.0, sounded - (joined ? 0 : v.gap) / 60.0)
      let f = hz(pitch)
      out.push({
        at: t0, len: len, by: v.name, pitch: pitch, vol: vol,
        tone: [v.wave, f, f, v.attack / 60.0, v.decay / 60.0, sustain, v.release / 60.0, vol, vol, v.duty],
      })
    }
    # A written note as heard, `shift` units later than it is written.
    let written = fn (v, nt, shift, level) {
      let h = x.heard({...nt, start: nt.start + shift, span: nt.span == nil ? nil : nt.span.map(|k| k + shift)})
      note(v, h.pitch, time(h.start), time(h.start + h.len), level, nt.touch)
    }
    let melody = x.melody
    for nt in melody.notes {
      written(melody.voice, nt, 0, band_level(nt.start))
      # A song that loops plays the pickup it starts with again over its end.
      if x.song.loop && nt.start < x.opening {
        written(melody.voice, nt, x.steps - x.opening, band_level(nt.start))
      }
    }
    for pname, part in x.parts {
      for vname in part.players {
        for nt in part.notes {
          written(x.voices[vname], nt, 0, x.part_level_at(pname, nt.start))
        }
      }
    }
    # The groove's voices and the derived part, and the drums.
    for row in x.rows {
      for n in row.notes {
        let from = at(n.at)
        let until = at(n.at + n.len)
        let row_level = band_level(n.at) * n.get("level", 1.0)
        for tone_pitch in n.pitches {
          note(row.voice, tone_pitch, from, until, row_level, n.get("accent", false) ? ACCENT : nil)
        }
      }
    }
    for hit in x.hits {
      let d = x.drums[hit.name]
      let hit_vol = d.vol * band_level(hit.at) / MF
      if d.has("host") {
        out.push({at: at(hit.at), len: 0.0, by: hit.name, pitch: nil, vol: hit_vol, sound: hit.name})
      } else {
        out.push({
          at: at(hit.at), len: d.dur / 60.0, by: hit.name, pitch: nil, vol: hit_vol,
          tone: [3, d.freq, d.end_freq, d.attack / 60.0, d.decay / 60.0, d.dur / 60.0, d.release / 60.0, hit_vol, hit_vol, 2],
        })
      }
    }
    out.sorted_by(|e| e.at)
  }

  # The host sounds the song needs, key -> how to build one: a host voice's
  # for each pitch it plays, a host drum's once.
  fn host_sounds(x, events, voices, drums) {
    let fn_for = fn (table, kwarg, host, what) {
      let key = host.lower()
      let f = (table ?? {}).get(key, nil)
      throw {kind: "ValueError", message: "Audio.Kauai: {what} plays `host {host}`, and {kwarg}: has no `{key}`"} if f == nil
      f
    }
    mut out = {}
    for e in events {
      continue if !e.has("sound") || out.has(e.sound)
      if e.pitch == nil {
        let d = x.drums[e.by]
        let make = fn_for(drums, "drums", d.host, "drum {e.by}")
        out[e.sound] = || make(d.params)
      } else {
        let v = x.voices[e.by]
        let make_note = fn_for(voices, "voices", v.host, "voice {e.by}")
        let p = e.pitch
        out[e.sound] = || make_note(p, v.params)
      }
    }
    out
  }

  # A song, read and checked, ready to play.
  class Song {
    new(x, voices, drums) {
      self._events = render(x)
      let time = clock(x)
      self._length = time(x.steps)
      self._loop_at = x.song.loop ? time(x.loop_at) : -1.0
      self._marks = x.marks.keys().map(|k| (k, time(x.marks[k]))).to_object()
      self._about = x.about
      self._build = host_sounds(x, self._events, voices, drums)
      self._todo = self._build.keys()
      self._sounds = {}
      self._score = nil
      self._started = nil
      self._volume = 1.0
    }

    # Build one host sound the song still needs; answers how many remain.
    prepare() {
      if !self._todo.empty() {
        let key = self._todo.pop()
        let s = self._build[key]()
        if type_of(s) != "Sound" {
          throw {kind: "TypeError", message: "Audio.Kauai: a host instrument answers an Audio.Sound, not {type_of(s)}"}
        }
        self._sounds[key] = s
      }
      self._todo.size()
    }

    # Play from the top (again, if it is playing), after building what is left.
    play() {
      while self.prepare() > 0 {}
      if self._score == nil {
        let flat = self._events.flat_map(fn (e) {
          return [e.at, ...e.tone] if !e.has("sound")
          [e.at, -1, self._sounds[e.sound]._id, e.vol, 0, 0, 0, 0, 0, 0, 0]
        })
        self._score = _Audio.score_new(flat, self._length, self._loop_at)
        _Audio.score_volume(self._score, self._volume)
      }
      _Audio.score_play(self._score)
      self._started = _Time.monotonic()
    }

    stop() {
      _Audio.score_stop(self._score) if self._score != nil
      self._started = nil
    }

    # The level the whole song plays at, 0.0..1.0, from its next note on.
    volume(v: Long | Float) {
      self._volume = v * 1.0
      _Audio.score_volume(self._score, v) if self._score != nil
    }

    # Whether it is playing: from play() until stop(), or its end.
    playing() {
      self._started != nil && (self._loop_at >= 0 || _Time.monotonic() - self._started < self._length)
    }

    # Whether it has played as far as `mark NAME`.
    reached(name: String) {
      let t = self._marks.get(name, nil)
      throw {kind: "ValueError", message: "Audio.Kauai: the song has no mark {name}"} if t == nil
      self._started != nil && _Time.monotonic() - self._started >= t
    }

    # Seconds once through.
    length() {
      self._length
    }

    # What its `about` says: {title, composer, ...}, the items it writes.
    about() {
      {...self._about}
    }

    # What it plays, in order: {at, len, by, pitch, vol}, in seconds from its
    # start; `by` names the voice or the drum, and a drum's pitch is nil.
    events() {
      self._events.map(|e| ({at: e.at, len: e.len, by: e.by, pitch: e.pitch, vol: e.vol}))
    }

    drop() {
      _Audio.score_free(self._score) if self._score != nil
    }
  }

  # `name` beside the file `from`.
  fn beside(from, name) {
    let dir = from == nil ? "" : FS.dirname(from)
    dir == "" ? name : FS.join(dir, name)
  }
  {
    # A song file, and the files it uses, relative to it: from the disk, or
    # from `dir` (a `Dir.embedded`, or anything with `exists(name)` and
    # `read(name)`).
    load: fn (path: String, voices = nil, drums = nil, dir = nil) {
      let exists = dir == nil ? |n| FS.exists(n) : |n| dir.exists(n)
      let read = dir == nil ? |n| FS.read(n) : |n| dir.read(n)
      throw {kind: "IOError", message: "Audio.Kauai: no song file {path}"} if !exists(path)
      let load_used = fn (name, from, line) {
        let file = beside(from, name)
        fail(line, "no file {file} to use") if !exists(file)
        (read(file), file)
      }
      Song.new(expand(read(path), path, load_used), voices, drums)
    },
    # A song given as text, which uses no other files.
    new: fn (text: String, voices = nil, drums = nil) {
      let no_files = fn (name, from, line) {
        fail(line, "a song given as text uses no files: use '{name}'")
      }
      Song.new(expand(text, nil, no_files), voices, drums)
    },
  }
}
let _audio_module = fn () {
  # tone's channels: WASM-4's two pulse waves, triangle and noise, plus the
  # culebra-only sawtooth. The values pass straight through to the host.
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

  # Play a tone: WASM-4's APU, in WASM-4's units. tone(freq, dur) is a
  # `dur`-tick note at `freq` (a tick is 1/60 s). The optional args expose the
  # full envelope: the note slides `freq` -> `end_freq` while an ADSR envelope
  # (attack/decay/release in ticks, `dur` the sustain) shapes the level from 0
  # up to `peak`, down to the sustain `vol`, and back to 0 (both 0..100).
  # `wave` picks the channel and `duty` the pulse shape.
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
    _Audio.tone(freq, ef, attack, decay, dur, release, vol, pk, wave, duty)
  }

  # A one-shot sample decoded once from WAV, MP3 or Ogg bytes (a String, e.g.
  # from FS.read or Embed) and played per call. One voice: play() restarts
  # it. Raises ValueError when the bytes are none of the three formats.
  class Sound {
    new(data: String) {
      self._id = _Audio.sound_load(data)
    }
    play() {
      _Audio.sound_play(self._id)
    }
    stop() {
      _Audio.sound_stop(self._id)
    }
    playing() {
      _Audio.sound_playing(self._id)
    }
    volume(v: Long | Float) {
      _Audio.sound_volume(self._id, v)
    }
    pitch(p: Long | Float) {
      _Audio.sound_pitch(self._id, p)
    }
    pan(p: Long | Float) {
      _Audio.sound_pan(self._id, p)
    }
    # Free the decoded sample with the last reference (a constructor that
    # threw drops before _id was ever set).
    drop() {
      _Audio.sound_free(self._id) if self._id != nil
    }
  }

  # A streamed file from MP3 or Ogg bytes. The runtime keeps it fed, so a
  # program with no frame loop keeps its music. stop() rewinds; pause() holds
  # the position. Raises ValueError when the bytes are neither format.
  class Music {
    new(data: String, loop: Bool = true) {
      self._id = _Audio.music_load(data, loop)
    }
    play() {
      _Audio.music_play(self._id)
    }
    stop() {
      _Audio.music_stop(self._id)
    }
    pause() {
      _Audio.music_pause(self._id)
    }
    resume() {
      _Audio.music_resume(self._id)
    }
    playing() {
      _Audio.music_playing(self._id)
    }
    seek(seconds: Long | Float) {
      _Audio.music_seek(self._id, seconds)
    }
    volume(v: Long | Float) {
      _Audio.music_volume(self._id, v)
    }
    pitch(p: Long | Float) {
      _Audio.music_pitch(self._id, p)
    }
    pan(p: Long | Float) {
      _Audio.music_pan(self._id, p)
    }
    drop() {
      _Audio.music_free(self._id) if self._id != nil
    }
  }

  # PCM the script synthesises, a block at a time. push takes the whole frames
  # in `samples` (stereo: interleaved L,R) and answers how many it took; with
  # no audio device nothing plays, and the block still counts.
  class PCM {
    new(rate: Long, channels: Long, buffer: Long) {
      self._id = _Audio.pcm_new(rate, channels, buffer)
    }
    ready() {
      _Audio.pcm_ready(self._id)
    }
    needed() {
      _Audio.pcm_needed(self._id)
    }
    push(samples: Array) {
      _Audio.pcm_push(self._id, samples)
    }
    submit() {
      _Audio.pcm_submit(self._id)
    }
    latency() {
      _Audio.pcm_latency(self._id)
    }
    play() {
      _Audio.pcm_play(self._id)
    }
    stop() {
      _Audio.pcm_stop(self._id)
    }
    pause() {
      _Audio.pcm_pause(self._id)
    }
    resume() {
      _Audio.pcm_resume(self._id)
    }
    playing() {
      _Audio.pcm_playing(self._id)
    }
    volume(v: Long | Float) {
      _Audio.pcm_volume(self._id, v)
    }
    pitch(p: Long | Float) {
      _Audio.pcm_pitch(self._id, p)
    }
    pan(p: Long | Float) {
      _Audio.pcm_pan(self._id, p)
    }
    drop() {
      _Audio.pcm_free(self._id) if self._id != nil
    }
  }

  # The microphone, read a block at a time. The runtime keeps the last second
  # of samples (Floats in -1..1, stereo interleaved L,R) while it runs; read
  # takes up to `frames` of them (all that are waiting when omitted) and
  # answers an Array, empty when none are. `rate` is a request, met by
  # resampling. With no microphone, or none the program may open, nothing
  # arrives: ready() says which, and Audio.capture_available() says before.
  class Capture {
    new(rate: Long = 44100, channels: Long = 1) {
      self._id = _Audio.capture_new(rate, channels)
    }
    ready() {
      _Audio.capture_ready(self._id)
    }
    start() {
      _Audio.capture_start(self._id)
    }
    stop() {
      _Audio.capture_stop(self._id)
    }
    running() {
      _Audio.capture_running(self._id)
    }
    waiting() {
      _Audio.capture_waiting(self._id)
    }
    read(frames = nil) {
      _Audio.capture_read(self._id, frames == nil ? -1 : frames)
    }
    drop() {
      _Audio.capture_free(self._id) if self._id != nil
    }
  }

  {
    available: fn () {
      _Audio.available()
    },
    capture_available: fn () {
      _Audio.capture_present()
    },
    tone: tone,
    Sound: Sound,
    Music: Music,
    PCM: PCM,
    Capture: Capture,
    # Songs in the Kauai language (src/preambles/kauai.cul).
    Kauai: _kauai_module(),
    PULSE: PULSE,
    PULSE2: PULSE2,
    TRIANGLE: TRIANGLE,
    NOISE: NOISE,
    SAWTOOTH: SAWTOOTH,
    DUTY_EIGHTH: DUTY_EIGHTH,
    DUTY_QUARTER: DUTY_QUARTER,
    DUTY_HALF: DUTY_HALF,
    DUTY_THREE_QUARTER: DUTY_THREE_QUARTER,
  }
}
let Audio = _audio_module()
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
  let _is_repeated = fn (a) {
    a.has("repeated") && a.repeated
  }
  # `positional` says which one it is when the inference would say the other:
  # a positional that may be omitted carries a `default`, which on its own
  # reads as an option.
  let _is_option = fn (a) {
    return !a.positional if a.has("positional")
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
  # What an argument left out of argv is worth, and whether it may be left out
  # at all: a `default` is the value, a repeated one collects nothing, and a
  # flag that was not passed is false.
  let _is_optional = fn (a) {
    a.has("default") || _is_repeated(a) || _arg_type(a) == "Bool"
  }
  let _missing_value = fn (a) {
    return a.default if a.has("default")
    return [] if _is_repeated(a)
    false
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
    let pos_args = spec.args.filter(_is_positional)
    let opt_args = spec.args.filter(_is_option)
    parts.push("Usage: {name}")
    parts.push(" [options]") if !opt_args.empty()
    let mut j = 0
    while j < pos_args.size() {
      let a = pos_args[j]
      if _is_repeated(a) {
        parts.push(" [<{a.name}>...]")
      } else if _is_optional(a) {
        parts.push(" [<{a.name}>]")
      } else {
        parts.push(" <{a.name}>")
      }
      j += 1
    }
    parts.push("\n")
    if !pos_args.empty() {
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
    if !opt_args.empty() {
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
  # `--name[=v]` and `-n[=v]` differ only in how many dashes they carry. Puts
  # what it read into `result`, and answers the index of the last token it took
  # — one past `i` when the value was the next token rather than after the `=`.
  let _take_option = fn (result, argv, i, spec, dash) {
    let parts = argv[i].slice(dash.size(), argv[i].size()).split("=")
    let name = parts[0]
    let has_value = parts.size() > 1
    let spec_a = _find_by_name(spec.args, name)
    if spec_a == nil {
      throw {kind: "ArgParseError", message: "unknown option '{dash}{name}'"}
    }
    if _arg_type(spec_a) == "Bool" && !has_value {
      result[spec_a.name] = true
      return i
    }
    let mut last = i
    let raw = if has_value {
      parts.slice(1, parts.size()).join("=")  # only the first `=` separates
    } else {
      last += 1
      if last >= argv.size() {
        throw {
          kind: "ArgParseError",
          message: "option '{dash}{name}' expects a value",
        }
      }
      argv[last]
    }
    let v = _coerce(raw, _arg_type(spec_a), spec_a.name)
    if _is_repeated(spec_a) {
      result[spec_a.name] ??= []
      result[spec_a.name].push(v)
    } else {
      result[spec_a.name] = v
    }
    last
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
        i = _take_option(result, argv, i, spec, "--")
      } else if tok.starts_with("-") && tok.size() > 1 {
        i = _take_option(result, argv, i, spec, "-")
      } else {
        positionals.push(tok)
      }
      i += 1
    }
    # Positionals are filled in spec order, but one that may be omitted only
    # takes a token when more are left than the required ones after it still
    # need. That is what makes `<files>... <dest>` hand `dest` the last token
    # instead of swallowing it, and `[from] <to>` fill `to` from a lone token.
    let pos_specs = spec.args.filter(_is_positional)
    let mut required_after = pos_specs.filter(|s| !_is_optional(s)).size()
    let mut taken = 0
    for a in pos_specs {
      required_after -= 1 if !_is_optional(a)
      let left = positionals.size() - taken
      let spare = left - required_after
      if _is_repeated(a) {
        let mine = positionals.slice(taken, taken + Math.max(spare, 0))
        result[a.name] = mine.map(|t| _coerce(t, _arg_type(a), a.name))
        taken += mine.size()
      } else if left > 0 && (spare > 0 || !_is_optional(a)) {
        result[a.name] = _coerce(positionals[taken], _arg_type(a), a.name)
        taken += 1
      }
    }
    if taken < positionals.size() {
      throw {
        kind: "ArgParseError",
        message: "unexpected positional argument '{positionals[taken]}'",
      }
    }
    let mut k = 0
    while k < spec.args.size() {
      let a = spec.args[k]
      if !result.has(a.name) {
        if !_is_optional(a) {
          throw {
            kind: "ArgParseError",
            message: "missing required argument '{a.name}'",
          }
        }
        result[a.name] = _missing_value(a)
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

inline constexpr const char* MATCHERS_MODULE_SOURCE = R"=culpre=(// assert_throws is the one matcher that stays here: it calls the function it
// is handed and reads `f.params`, and its failure message already names the
// expected and the actual kind. The comparison matchers are native (see
// kBuiltinFns in stdlib_rt.h).
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
  if type_of(x) == "Regex" {
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

inline constexpr const char* PEG_MODULE_SOURCE = R"=culpre=(fn _peg_walk(node) {
  yield node
  for c in node.nodes {
    yield from _peg_walk(c)
  }
}
fn _peg_find(node, name) {
  for n in _peg_walk(node) {
    return n if n.name == name
  }
  nil
}
fn _peg_find_all(node, name) {
  let mut out = []
  for n in _peg_walk(node) {
    out.push(n) if n.name == name
  }
  out
}
fn _peg_str_at(node, level) {
  let pad = "  ".repeat(level)
  let mut out = if node.is_token {
    pad + "- " + node.name + " (" + node.token + ")\n"
  } else {
    pad + "+ " + node.name + "\n"
  }
  for c in node.nodes {
    out = out + _peg_str_at(c, level + 1)
  }
  out
}
fn _peg_str(node) {
  _peg_str_at(node, 0)
}
let _peg_module = fn () {
  class PEG {
    new(grammar, start = "", optimize = true, packrat = true) {
      self._src = grammar
      self._start = start
      self._optimize = optimize
      self._packrat = packrat
      _PEG.check(grammar, start, packrat)
    }
    parse(text, path = "", actions = nil) {
      _PEG.parse(self._src, text, self._start, self._optimize, self._packrat, path, actions)
    }
    test(text) {
      _PEG.test(self._src, text, self._start, self._packrat)
    }
  }
  {
    compile: fn (grammar, start = "", optimize = true, packrat = true) {
      PEG.new(grammar, start, optimize, packrat)
    },
    check: fn (grammar, start = "") {
      _PEG.check(grammar, start, true)
    },
    parse: fn (grammar, text, start = "", optimize = true, packrat = true, path = "", actions = nil) {
      _PEG.parse(grammar, text, start, optimize, packrat, path, actions)
    },
    test: fn (grammar, text, start = "", packrat = true) {
      _PEG.test(grammar, text, start, packrat)
    },
    walk: _peg_walk,
    find: _peg_find,
    find_all: _peg_find_all,
    str: _peg_str,
    PEG: PEG,
  }
}
let PEG = _peg_module()
)=culpre=";

inline constexpr const char* FST_MODULE_SOURCE = R"=culpre=(let _fst_module = fn () {
  class Set {
    new(bytecode) {
      self._bc = bytecode
      _FST.set_check(bytecode)
    }
    contains(key) {
      _FST.set_contains(self._bc, key)
    }
    common_prefix_search(text) {
      _FST.set_common_prefix_search(self._bc, text)
    }
    longest_common_prefix_search(text) {
      _FST.set_longest_common_prefix_search(self._bc, text)
    }
    predictive_search(prefix) {
      _FST.set_predictive_search(self._bc, prefix)
    }
    edit_distance_search(
      word,
      max_edits,
      insert_cost = 1,
      delete_cost = 1,
      replace_cost = 1,
    ) {
      _FST.set_edit_distance_search(
        self._bc,
        word,
        max_edits,
        insert_cost,
        delete_cost,
        replace_cost,
      )
    }
    suggest(word) {
      _FST.set_suggest(self._bc, word)
    }
  }
  class Map {
    new(bytecode) {
      self._bc = bytecode
      _FST.map_check(bytecode)
    }
    get(key) {
      _FST.map_get(self._bc, key)
    }
    common_prefix_search(text) {
      _FST.map_common_prefix_search(self._bc, text)
    }
    longest_common_prefix_search(text) {
      _FST.map_longest_common_prefix_search(self._bc, text)
    }
    predictive_search(prefix) {
      _FST.map_predictive_search(self._bc, prefix)
    }
    edit_distance_search(
      word,
      max_edits,
      insert_cost = 1,
      delete_cost = 1,
      replace_cost = 1,
    ) {
      _FST.map_edit_distance_search(
        self._bc,
        word,
        max_edits,
        insert_cost,
        delete_cost,
        replace_cost,
      )
    }
    suggest(word) {
      _FST.map_suggest(self._bc, word)
    }
  }
  class IndexMap {
    new(bytecode) {
      self._bc = bytecode
      _FST.index_check(bytecode)
    }
    get(key) {
      _FST.index_get(self._bc, key)
    }
    common_prefix_search(text) {
      _FST.index_common_prefix_search(self._bc, text)
    }
    longest_common_prefix_search(text) {
      _FST.index_longest_common_prefix_search(self._bc, text)
    }
    predictive_search(prefix) {
      _FST.index_predictive_search(self._bc, prefix)
    }
    edit_distance_search(
      word,
      max_edits,
      insert_cost = 1,
      delete_cost = 1,
      replace_cost = 1,
    ) {
      _FST.index_edit_distance_search(
        self._bc,
        word,
        max_edits,
        insert_cost,
        delete_cost,
        replace_cost,
      )
    }
    suggest(word) {
      _FST.index_suggest(self._bc, word)
    }
  }
  {
    compile_set: fn (keys, sorted = false) {
      _FST.compile_set(keys, sorted)
    },
    compile_map: fn (entries, sorted = false) {
      _FST.compile_map(entries, sorted)
    },
    compile_index_map: fn (entries, sorted = false) {
      _FST.compile_index_map(entries, sorted)
    },
    compile_auto_index: fn (keys, sorted = false) {
      _FST.compile_auto_index(keys, sorted)
    },
    Set: Set,
    Map: Map,
    IndexMap: IndexMap,
  }
}
let FST = _fst_module()
)=culpre=";

inline constexpr const char* STRING_FNS_MODULE_SOURCE = R"=culpre=(let replace = fn (s, pat, repl) {
  if type_of(pat) == "String" || type_of(pat) == "StringView" {
    s.split(pat).join(repl)
  } else {
    pat.replace_all(s, repl)
  }
}

let replace_first = fn (s, pat, repl) {
  if type_of(pat) == "String" || type_of(pat) == "StringView" {
    s.split(pat, 2).join(repl)
  } else {
    pat.replace_first(s, repl)
  }
}

let split_once = fn (s, sep) {
  let i = sep.empty() ? -1 : s.index_of(sep)
  i < 0 ? nil : (s.slice(0, i), s.slice(i + sep.size(), s.size()))
}

let rsplit_once = fn (s, sep) {
  let i = sep.empty() ? -1 : s.last_index_of(sep)
  i < 0 ? nil : (s.slice(0, i), s.slice(i + sep.size(), s.size()))
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
      with: fn (fields) {
        _methods({...bound, ...fields})
      },
    }
  }
  let _set_level = fn (level) {
    let n = _levels.get(level, -1)
    throw "Log.set_level: unknown level '" + level + "'" if n < 0
    _threshold = n
  }
  let _set_format = fn (format) {
    if format != "text" && format != "json" {
      throw "Log.set_format: unknown format '{format}'"
    }
    _format = format
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
  # A Path is what the Path class built, and `type_of` names it: nothing
  # else can answer to the name, so the probe is one comparison.
  let _is_path = fn (o) {
    type_of(o) == "Path"
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
    join(other) {
      Path.new(FS.join(self._path, _s(other)))
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
# `@value`: two declared Float fields, no identity, frozen once `new`
# returns — a Vector2 has always been used as a pure value (every operator
# returns a fresh one; nothing in this codebase ever wrote `v.x = ...` after
# construction), so the decorator only closes a gap the type already
# behaved as if it had. It also derives `eq`/`hash` from the fields itself
# (the same pair `@derive(Eq, Hash)` used to generate here — nominal, not
# structural: a same-shaped Vector3 stays unequal, and a non-Vector2
# operand is simply unequal, never a thrown error), and the whole-scope
# unboxing this decorator makes possible is why the flat layout matters at
# all: a chain like `V2.new(a, b).len()`, or a `let mut` accumulator
# reassigned every loop iteration, compiles to plain scalar slots — no
# object, no call (docs/internals/vm.md §5.3).
#
# BREAKING CHANGE (project_vector_types_design.md, project_value_type_spec.md
# §15 in memory): a Vector2 instance can no longer be mutated after
# construction (`v.x = 5.0`, `v.z = 9`, `v.remove('x')` all now raise
# ImmutableError, where they previously succeeded on the ordinary mutable
# Object every class produced before this).
let _vector2_module = fn () {
  @value
  class Vector2 {
    x: Float
    y: Float

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

    # Each of these spells its arithmetic out rather than delegating
    # (length -> length_squared -> dot): a dynamic call costs more than the
    # two products it would wrap, and these are what a per-frame loop calls.
    dot(other) {
      self.x * other.x + self.y * other.y
    }
    length_squared() {
      let x = self.x
      let y = self.y
      x * x + y * y
    }
    length() {
      let x = self.x
      let y = self.y
      Math.sqrt(x * x + y * y)
    }
    # A zero vector's normalized() divides by 0.0 like any other Float
    # division in culebra — raises ZeroDivisionError. No silent fallback.
    normalized() {
      let x = self.x
      let y = self.y
      let len = Math.sqrt(x * x + y * y)
      Vector2.new(x / len, y / len)
    }
    # Component-wise, not `(self - o).length_squared()` — avoids allocating a
    # throwaway Vector2 just to measure it. Ranking or thresholding distances
    # never needs the square root, so distance_squared_to is the form a hot
    # path wants.
    distance_to(other) {
      let dx = self.x - other.x
      let dy = self.y - other.y
      Math.sqrt(dx * dx + dy * dy)
    }
    distance_squared_to(other) {
      let dx = self.x - other.x
      let dy = self.y - other.y
      dx * dx + dy * dy
    }
  }
  Vector2
}
let Vector2 = _vector2_module()
)=culpre=";

inline constexpr const char* VECTOR3_MODULE_SOURCE = R"=culpre=(# Vector3 — the 3D counterpart of Vector2 (see vector2.cul for the shared
# design rationale: Float-only, no Point split, derived nominal equality,
# and why `@value` — BREAKING CHANGE — applies here too).
# cross() is intentionally omitted from both: Vector2's cross is a scalar
# (perp dot), Vector3's is a vector — the asymmetry invites naming/shape
# drift, and no example in this repo needs it yet.
let _vector3_module = fn () {
  @value
  class Vector3 {
    x: Float
    y: Float
    z: Float

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

    # Each of these spells its arithmetic out rather than delegating
    # (length -> length_squared -> dot): a dynamic call costs more than the
    # three products it would wrap, and these are what a per-frame loop calls.
    dot(other) {
      self.x * other.x + self.y * other.y + self.z * other.z
    }
    length_squared() {
      let x = self.x
      let y = self.y
      let z = self.z
      x * x + y * y + z * z
    }
    length() {
      let x = self.x
      let y = self.y
      let z = self.z
      Math.sqrt(x * x + y * y + z * z)
    }
    normalized() {
      let x = self.x
      let y = self.y
      let z = self.z
      let len = Math.sqrt(x * x + y * y + z * z)
      Vector3.new(x / len, y / len, z / len)
    }
    # Component-wise, not `(self - o).length_squared()` — avoids allocating a
    # throwaway Vector3 just to measure it. Ranking or thresholding distances
    # never needs the square root, so distance_squared_to is the form a hot
    # path wants.
    distance_to(other) {
      let dx = self.x - other.x
      let dy = self.y - other.y
      let dz = self.z - other.z
      Math.sqrt(dx * dx + dy * dy + dz * dz)
    }
    distance_squared_to(other) {
      let dx = self.x - other.x
      let dy = self.y - other.y
      let dz = self.z - other.z
      dx * dx + dy * dy + dz * dz
    }
  }
  Vector3
}
let Vector3 = _vector3_module()
)=culpre=";

inline constexpr const char* DEQUE_MODULE_SOURCE = R"=culpre=(# Deque — double-ended queue backed by a growable ring buffer (array +
# head index + count), so push/pop at either end are O(1) amortized.
# `Array` only ever grows/shrinks at the end (`push`/`pop`), so a FIFO
# queue built on it needs `remove_at(0)`, which shifts every remaining
# element — O(n) per dequeue. Reach for `Deque` whenever the front
# matters: a FIFO queue (`push_back` + `pop_front`), a sliding window,
# or a stack where "which end" should be explicit (`push_back` +
# `pop_back`, same as `Array.push`/`pop`).
#
# Mirrors `Array.pop()`: `pop_front`/`pop_back`/`peek_front`/`peek_back`
# return `nil` on an empty Deque rather than throwing (same ambiguity
# `Array.pop()` already accepts — a `nil` element and an empty Deque
# are indistinguishable — so callers that push `nil` should track size
# separately).
let _deque_module = fn () {
  fn _deque_iter(buf, head, count) {
    for i in 0..count {
      yield buf[(head + i) % buf.size()]
    }
  }

  class Deque {
    new() {
      self._buf = []
      self._head = 0
      self._count = 0
    }

    size() {
      self._count
    }
    empty() {
      self._count == 0
    }

    _grow() {
      let old_cap = self._buf.size()
      let new_cap = old_cap == 0 ? 8 : old_cap * 2
      let mut new_buf = range(self._count)
        .map(|i| self._buf[(self._head + i) % old_cap])
        .collect()
      new_buf.extend(repeat(new_cap - self._count, nil))
      self._buf = new_buf
      self._head = 0
    }

    push_back(x) {
      if self._count == self._buf.size() {
        self._grow()
      }
      self._buf[(self._head + self._count) % self._buf.size()] = x
      self._count += 1
    }

    push_front(x) {
      if self._count == self._buf.size() {
        self._grow()
      }
      self._head = (self._head - 1 + self._buf.size()) % self._buf.size()
      self._buf[self._head] = x
      self._count += 1
    }

    pop_front() {
      return nil if self.empty()
      let x = self._buf[self._head]
      self._buf[self._head] = nil
      self._head = (self._head + 1) % self._buf.size()
      self._count -= 1
      x
    }

    pop_back() {
      return nil if self.empty()
      let i = (self._head + self._count - 1) % self._buf.size()
      let x = self._buf[i]
      self._buf[i] = nil
      self._count -= 1
      x
    }

    peek_front() {
      return nil if self.empty()
      self._buf[self._head]
    }

    peek_back() {
      return nil if self.empty()
      self._buf[(self._head + self._count - 1) % self._buf.size()]
    }

    to_array() {
      _deque_iter(self._buf, self._head, self._count).collect()
    }

    iter() {
      _deque_iter(self._buf, self._head, self._count)
    }

    __str__() {
      "Deque({self.to_array()})"
    }
  }
  Deque
}
let Deque = _deque_module()
)=culpre=";

inline constexpr const char* PRIORITY_QUEUE_MODULE_SOURCE = R"=culpre=(# PriorityQueue — binary min-heap over an Array. `push`/`pop` are
# O(log n); the naive alternative (an Array kept sorted, or scanned for
# the minimum on every pop) is O(n) or O(n log n) per operation.
#
# Ordering follows the same convention as `Array.sort`/`sort_by`:
# elements compare by `<` (so a class's `__lt__` is honored) unless
# `key:` is given, and `reverse: true` flips to a max-heap. `pop`/`peek`
# return `nil` on an empty queue, matching `Array.pop()`.
let _priority_queue_module = fn () {
  class PriorityQueue {
    new(*, key: Function | Nil = nil, reverse: Bool = false) {
      self._heap = []
      self._key = key
      self._reverse = reverse
    }

    size() {
      self._heap.size()
    }
    empty() {
      self._heap.empty()
    }

    # Heap-array order is not priority order, so echoing `_heap` here
    # (the default formatter's fallback) would mislead more than help.
    __str__() {
      "PriorityQueue(size={self.size()})"
    }

    _less(a, b) {
      let ka = self._key == nil ? a : self._key(a)
      let kb = self._key == nil ? b : self._key(b)
      self._reverse ? kb < ka : ka < kb
    }

    _swap(i, j) {
      (self._heap[i], self._heap[j]) = (self._heap[j], self._heap[i])
    }

    push(x) {
      self._heap.push(x)
      let mut i = self._heap.size() - 1
      while i > 0 {
        let parent = (i - 1) / 2
        if self._less(self._heap[i], self._heap[parent]) {
          self._swap(i, parent)
          i = parent
        } else {
          break
        }
      }
    }

    peek() {
      return nil if self.empty()
      self._heap[0]
    }

    pop() {
      return nil if self.empty()
      let top = self._heap[0]
      let last = self._heap.pop()
      if !self._heap.empty() {
        self._heap[0] = last
        self._sift_down(0)
      }
      top
    }

    _sift_down(start) {
      let mut i = start
      let n = self._heap.size()
      while true {
        let l = i * 2 + 1
        let r = i * 2 + 2
        let mut smallest = i
        if l < n && self._less(self._heap[l], self._heap[smallest]) {
          smallest = l
        }
        if r < n && self._less(self._heap[r], self._heap[smallest]) {
          smallest = r
        }
        if smallest == i {
          break
        }
        self._swap(i, smallest)
        i = smallest
      }
    }
  }
  PriorityQueue
}
let PriorityQueue = _priority_queue_module()
)=culpre=";

inline constexpr const char* STATE_MACHINE_MODULE_SOURCE = R"=culpre=(# StateMachine — a hierarchical state machine (a statechart). States nest, an
# event the active state does not handle bubbles to its ancestors, and a
# transition exits and re-enters exactly the states below its domain: the
# deepest state that properly contains both the state declaring it and its
# target. docs/stdlib.md carries the reference; this header is the why.
#
# Two front ends meet at one engine. `StateMachine.parse` is a pure
# text -> description transform; `StateMachine.new` checks that description,
# resolves every name in it, and links it. Both the names and the `[!g]`
# negation are gone by the time the machine runs — the table holds plain
# functions, and `_select` has one notion of a guard passing.
#
# What a transition does is fixed by its source and target, so the domain and
# the path entered afterwards are settled once, in `_link`, rather than
# recomputed per event.
#
# The DSL is parsed with the native `_PEG` primitives rather than the `PEG`
# module: a preamble that named `PEG` would need that module registered too,
# and only the program's own tokens decide what gets registered.
let _state_machine_module = fn () {
  # The machine itself is a composite state holding the top-level states, so
  # `initial`, bubbling and the domain need no separate case for "at the top".
  # Its name is one no identifier can spell, and it stays inside: a machine
  # with no initial state is rejected, so `state()` always names a real leaf,
  # and `in_state` stops before the root.
  let ROOT = ""

  let STATE_KEYS = ["initial", "entry", "exit", "on", "states"]
  let CAND_KEYS = ["guard", "negate", "action", "target"]

  # An ill-formed machine; a caller passing the wrong type gets the standard
  # `TypeError` instead, as in path.cul / time.cul.
  fn _fail(msg) {
    throw {kind: "StateMachineError", message: msg}
  }
  fn _want(what, got, where) {
    throw {
      kind: "TypeError",
      message: "type error: expected {what} for {where}, got {type_of(got)}",
    }
  }

  fn _reject_unknown(o, allowed, where) {
    for k in o.keys() {
      if !allowed.contains(k) {
        _fail("{where}: unknown key '{k}' (expected {allowed.join(', ')})")
      }
    }
  }

  # A guard/action slot holds a callable, or a name for one that the
  # `guards:` / `actions:` table answers; the DSL only ever emits names.
  fn _resolve(v, table, kind, where) {
    return nil if v == nil
    return v if type_of(v) == "Function"
    _want("Function|String", v, "{kind} at {where}") if type_of(v) != "String"
    let f = table.get(v, nil)
    _fail("{where}: no {kind} named '{v}'") if f == nil
    f
  }

  fn _candidates(spec, guards, actions, where) {
    let mut out = []
    for c in type_of(spec) == "Array" ? spec : [spec] {
      _reject_unknown(c, CAND_KEYS, where)
      let g = _resolve(c.get("guard", nil), guards, "guard", where)
      out.push({
        # `[!g]` folds into the guard here, so nothing downstream knows it.
        guard: g != nil && c.get("negate", false) ? |ctx, ev| !g(ctx, ev) : g,
        action: _resolve(c.get("action", nil), actions, "action", where),
        target: c.get("target", nil),
        mut domain: nil,
        mut enter: nil,
      })
    }
    out
  }

  fn _walk(map, parent, states, guards, actions) {
    _want("Object", map, "a states description") if type_of(map) != "Object"
    for (name, d) in map.iter() {
      _fail("duplicate state '{name}'") if states.has(name)
      _want("Object", d, "state '{name}'") if type_of(d) != "Object"
      _reject_unknown(d, STATE_KEYS, "state '{name}'")
      states[name] = {
        parent: parent,
        mut initial: nil,
        entry: _resolve(
          d.get("entry", nil),
          actions,
          "action",
          "state '{name}' entry",
        ),
        exit: _resolve(
          d.get("exit", nil),
          actions,
          "action",
          "state '{name}' exit",
        ),
        on: {},
      }
      if d.get("initial", false) {
        if states[parent].initial != nil {
          _fail("'{states[parent].initial}' and '{name}' are both initial")
        }
        states[parent].initial = name
      }
      for (event, spec) in d.get("on", {}).iter() {
        let where = "state '{name}' on '{event}'"
        states[name].on[event] = _candidates(spec, guards, actions, where)
      }
      _walk(d.get("states", {}), name, states, guards, actions)
    }
  }

  # Entering a composite state means entering its initial child, down to a
  # leaf: that is where a machine actually rests.
  fn _leaf(states, s) {
    let mut cur = s
    while states[cur].initial != nil {
      cur = states[cur].initial
    }
    cur
  }

  # The deepest state that is a proper ancestor of both.
  fn _domain(states, a, b) {
    let mut x = states[a].parent
    while x != nil {
      let mut y = states[b].parent
      while y != nil {
        return x if x == y
        y = states[y].parent
      }
      x = states[x].parent
    }
    ROOT
  }

  # The states strictly below `domain`, down to `target`, root-first.
  fn _path(states, domain, target) {
    let mut out = []
    let mut s = target
    while s != domain {
      out.push(s)
      s = states[s].parent
    }
    out.reverse()
    out
  }

  # A second pass, because a transition may name a state declared later. Every
  # composite needs an initial child — checked from the child's side, since a
  # state is composite exactly when another names it as parent.
  fn _link(states) {
    _fail("the machine has no initial state") if states[ROOT].initial == nil
    for (name, s) in states.iter() {
      if s.parent != nil && states[s.parent].initial == nil {
        _fail("composite state '{s.parent}' has no initial child")
      }
      for (event, list) in s.on.iter() {
        for t in list {
          if t.target != nil {
            if !states.has(t.target) {
              _fail("state '{name}' on '{event}': no state named '{t.target}'")
            }
            t.domain = _domain(states, name, t.target)
            t.enter = _path(states, t.domain, _leaf(states, t.target))
          }
        }
      }
    }
  }

  # `Item <- State / Transition` leans on the trailing block: a state header is
  # followed by `{`, a transition never is, so the ordered choice settles it
  # without a keyword. The `![a-zA-Z0-9_]` is what stops `initialentry: e` from
  # reading as `initial` plus a second modifier — a name that merely begins
  # with a keyword (`initialize`) is rejected either way, since a bare
  # identifier is not legal in modifier position. It has to live *inside* the
  # `< >` token: outside one the whitespace after the literal is skipped first,
  # so `initial entry: e` would see the `e` as the character that must not
  # follow. `Entry`/`Exit` do not need it — their mandatory `:` is already a
  # boundary — but carry it so the three read alike.
  let GRAMMAR = `
    Machine     <- Ident '{' State* '}'
    State       <- Ident Modifier* '{' Item* '}'
    Modifier    <- Initial / Entry / Exit
    Initial     <- < 'initial' ![a-zA-Z0-9_] >
    Entry       <- < 'entry' ![a-zA-Z0-9_] > ':' Ident
    Exit        <- < 'exit' ![a-zA-Z0-9_] > ':' Ident
    Item        <- State / Transition
    Transition  <- Ident Guard? Action? Target?
    Guard       <- '[' Bang? Ident ']'
    Bang        <- '!'
    Action      <- ':' Ident
    Target      <- '->' Ident
    Ident       <- < [a-zA-Z_][a-zA-Z0-9_]* >
    %whitespace <- ([ \t\r\n] / '#' [^\n]*)*
  `

  # Each optional part of a transition is tagged, because `sv.values[1]` on
  # its own cannot say whether the guard, the action or the target matched.
  let ACTIONS = {
    Ident: |sv| sv.token,
    Bang: |sv| true,
    # `Bang` only ever prepends, so the name is last either way.
    Guard: |sv| {
      tag: "guard",
      name: sv.values[-1],
      negate: sv.values.size() > 1,
    },
    Action: |sv| {tag: "action", name: sv.values[0]},
    Target: |sv| {tag: "target", name: sv.values[0]},
    Initial: |sv| {tag: "initial"},
    Entry: |sv| {tag: "entry", name: sv.values[0]},
    Exit: |sv| {tag: "exit", name: sv.values[0]},
    Transition: fn (sv) {
      let mut c = {
        mut guard: nil,
        mut negate: false,
        mut action: nil,
        mut target: nil,
      }
      for i in 1..sv.values.size() {
        match sv.values[i] {
          {tag: "guard", name, negate} => {
            c.guard = name
            c.negate = negate
          },
          {tag: "action", name} => c.action = name,
          {tag: "target", name} => c.target = name,
        }
      }
      {tag: "transition", event: sv.values[0], cand: c}
    },
    State: fn (sv) {
      let mut d = {}
      let mut on = {}
      let mut sub = {}
      for i in 1..sv.values.size() {
        match sv.values[i] {
          {tag: "initial"} => d["initial"] = true,
          {tag: "entry", name} => d["entry"] = name,
          {tag: "exit", name} => d["exit"] = name,
          {tag: "state", name, desc} => sub[name] = desc,
          {
            tag: "transition",
            event,
            cand,
          } => on.get_or_put(event, || []).push(cand),
        }
      }
      d["on"] = on if !on.empty()
      d["states"] = sub if !sub.empty()
      {tag: "state", name: sv.values[0], desc: d}
    },
    Machine: fn (sv) {
      let mut states = {}
      for i in 1..sv.values.size() {
        states[sv.values[i].name] = sv.values[i].desc
      }
      {name: sv.values[0], states: states}
    },
  }

  class StateMachine {
    new(desc, *, .name = "", guards = {}, actions = {}, .context = nil) {
      let mut states = {}
      states[ROOT] = {
        parent: nil,
        mut initial: nil,
        entry: nil,
        exit: nil,
        on: {},
      }
      _walk(desc, ROOT, states, guards, actions)
      _link(states)
      self._states = states
      self._boot = _path(states, ROOT, _leaf(states, ROOT))
      # Standing at the root, `reset` is exactly "enter the initial one".
      self._current = ROOT
      self.reset()
    }

    # Build from the text DSL. A malformed machine raises `PEGError`, whose
    # message carries `path` when one is given.
    static parse(text, *, guards = {}, actions = {}, context = nil, path = "") {
      # _PEG.parse(grammar, text, start, optimize, packrat, path, actions)
      let m = _PEG.parse(GRAMMAR, text, "", true, true, path, ACTIONS)
      StateMachine.new(
        m.states,
        name: m.name,
        guards: guards,
        actions: actions,
        context: context,
      )
    }

    state() {
      self._current
    }

    # True for the active leaf and for every composite state containing it.
    in_state(name) {
      let mut s = self._current
      while s != ROOT {
        return true if s == name
        s = self._states[s].parent
      }
      false
    }

    fire(event, payload = nil) {
      let ev = {name: event, payload: payload}
      let t = self._select(event, ev)
      return false if t == nil
      if t.enter == nil {
        # An internal transition: the action runs, the configuration stands.
        t.action(self.context, ev) if t.action != nil
        return true
      }
      self._exit(t.domain, ev)
      t.action(self.context, ev) if t.action != nil
      self._enter(t.enter, ev)
      true
    }

    # Whether `fire` would take a transition. Guards see the same event, so
    # they must be free of side effects for this to mean anything.
    can_fire(event, payload = nil) {
      self._select(event, {name: event, payload: payload}) != nil
    }

    # Leave the active configuration and enter the initial one again. The
    # context is the caller's data and is left alone.
    reset() {
      let ev = {name: nil, payload: nil}
      self._exit(ROOT, ev)
      self._enter(self._boot, ev)
    }

    __str__() {
      self.name == ""
        ? "StateMachine(state={self._current})"
        : "StateMachine({self.name}, state={self._current})"
    }

    _exit(domain, ev) {
      let mut s = self._current
      while s != domain {
        let ex = self._states[s].exit
        ex(self.context, ev) if ex != nil
        s = self._states[s].parent
      }
    }

    _enter(path, ev) {
      for n in path {
        let en = self._states[n].entry
        en(self.context, ev) if en != nil
      }
      self._current = path[-1]
    }

    # Walk out from the active leaf; the innermost state that both names the
    # event and passes its guard wins. A state that names it but whose guards
    # all fail has not handled it, so the search carries on to the parent.
    _select(name, ev) {
      let mut s = self._current
      while s != nil {
        let list = self._states[s].on.get(name, nil)
        if list != nil {
          for t in list {
            return t if t.guard == nil || t.guard(self.context, ev)
          }
        }
        s = self._states[s].parent
      }
      nil
    }
  }
  StateMachine
}
let StateMachine = _state_machine_module()
)=culpre=";

inline constexpr const char* DIR_MODULE_SOURCE = R"=culpre=(# Dir — a read-only set of files, wherever they live: a directory on disk,
# files held in memory, assets baked into the binary, or the entries of a ZIP
# archive. Each kind is a class over thin natives (`_Dir.*`), and every one
# conforms to the built-in `trait Dir` (shared.h): the five required methods
# are here, the defaults (`files` / `glob` / `size` / `copy_to`) come from the
# trait unless a kind answers faster itself. Each class names its kind to the
# natives in its constructor (`_Dir.mark`, JitSpecialTable::dir_kind).
#
# Paths are relative to the root and `/` separated, and `_Dir.normalize` is
# the one rule for every kind (vfs.h): `""` and `"."` name the root, `.` and
# empty segments drop out, and a path that starts with `/` or `\` or climbs
# with `..` is simply not there.
let _dir_module = fn () {
  let _win = FS.sep() == "\\"

  # A path argument that names a directory on disk: a String or a Path.
  let _disk_path = fn (who, p) {
    let t = type_of(p)
    if t != 'String' && t != 'StringView' && t != 'Path' {
      throw {
        kind: 'TypeError',
        message: "type error: {who} expects String|Path, got {t}",
      }
    }
    to_string(p)
  }
  let _string_arg = fn (who, v) {
    let t = type_of(v)
    if t != 'String' && t != 'StringView' {
      throw {
        kind: 'TypeError',
        message: "type error: {who} expects String, got {t}",
      }
    }
    to_string(v)
  }

  # The two kinds of IOError every Dir owes, worded once for all of them.
  let _need_file = fn (d, op, path) {
    if !d.is_file(path) {
      throw {
        kind: 'IOError',
        message: d.is_dir(path)
          ? "Dir.{op}: '{path}' is a directory"
          : "Dir.{op}: no such file '{path}'",
      }
    }
  }
  let _need_dir = fn (d, op, path) {
    if !d.is_dir(path) {
      throw {
        kind: 'IOError',
        message: d.is_file(path)
          ? "Dir.{op}: '{path}' is a file"
          : "Dir.{op}: no such directory '{path}'",
      }
    }
  }

  class DiskDir {
    new(path) {
      _Dir.mark(self, 'disk')
      let p = _disk_path('Dir.disk', path)
      self.root = FS.abspath(p)
      if !FS.is_dir(self.root) {
        throw {kind: 'IOError', message: "Dir.disk: no such directory '{p}'"}
      }
    }
    # The OS path of `path` under the root, or nil when it is not inside it.
    # On Windows `\` and `:` would leave the root (a separator, a drive), so
    # a path holding either is not there either.
    _full(path) {
      let rel = _Dir.normalize(path)
      cond {
        rel == nil || _win && (rel.contains("\\") || rel.contains(':')) => nil,
        rel == '' => self.root,
        _ => FS.join(self.root, rel),
      }
    }
    read(path) {
      _need_file(self, 'read', path)
      FS.read(self._full(path))
    }
    list_dir(path) {
      _need_dir(self, 'list_dir', path)
      FS.list_dir(self._full(path)).sorted()
    }
    exists(path) {
      let f = self._full(path)
      f != nil && FS.exists(f)
    }
    is_file(path) {
      let f = self._full(path)
      f != nil && FS.is_file(f)
    }
    is_dir(path) {
      let f = self._full(path)
      f != nil && FS.is_dir(f)
    }
    size(path) {
      _need_file(self, 'size', path)
      FS.size(self._full(path))
    }
    files() {
      let skip = self.root.size() + (self.root.ends_with(FS.sep()) ? 0 : 1)
      FS
        .walk(self.root)
        .filter(|p| FS.is_file(p))
        .map(|p| to_string(p.slice(skip, p.size())))
        .map(|p| _win ? p.replace("\\", '/') : p)
        .sorted()
    }
  }

  class MemoryDir {
    new(files) {
      _Dir.mark(self, 'memory')
      if type_of(files) != 'Object' {
        throw {
          kind: 'TypeError',
          message: "type error: Dir.memory expects Object, got {type_of(files)}",
        }
      }
      mut contents = {}
      for key, value in files {
        if type_of(key) != 'String' {
          throw {
            kind: 'TypeError',
            message: "Dir.memory: a path is a String, got {type_of(key)}",
          }
        }
        let t = type_of(value)
        if t != 'String' && t != 'StringView' {
          throw {
            kind: 'TypeError',
            message: "Dir.memory: '{key}' holds {t}, not String (a file in a subdirectory is a key like 'sub/name.txt')",
          }
        }
        let rel = _Dir.normalize(key)
        if rel == nil || rel == '' {
          throw {
            kind: 'ValueError',
            message: "Dir.memory: '{key}' is not a file inside the directory",
          }
        }
        if contents.has(rel) {
          throw {
            kind: 'ValueError',
            message: "Dir.memory: '{key}' names a file another key already names",
          }
        }
        contents[rel] = to_string(value)
      }
      # Every directory a file lies under, so asking about one is a lookup.
      mut dirs = {}
      for rel in contents.keys() {
        let parts = rel.split('/')
        for i in range(1, parts.size()) {
          let parent = parts.slice(0, i).join('/')
          if contents.has(parent) {
            throw {
              kind: 'ValueError',
              message: "Dir.memory: '{parent}' is both a file and a directory",
            }
          }
          dirs[parent] = true
        }
      }
      self.contents = contents
      self.dirs = dirs
    }
    _is_dir(rel) {
      rel == '' || self.dirs.has(rel)
    }
    read(path) {
      _need_file(self, 'read', path)
      self.contents[_Dir.normalize(path)]
    }
    list_dir(path) {
      _need_dir(self, 'list_dir', path)
      let rel = _Dir.normalize(path)
      let prefix = rel == '' ? '' : rel + '/'
      self
        .contents
        .keys()
        .iter()
        .filter(|k| k.starts_with(prefix))
        .map(|k| to_string(k.slice(prefix.size(), k.size()).split('/')[0]))
        .distinct()
        .collect()
        .sorted()
    }
    exists(path) {
      let rel = _Dir.normalize(path)
      rel != nil && (self.contents.has(rel) || self._is_dir(rel))
    }
    is_file(path) {
      let rel = _Dir.normalize(path)
      rel != nil && self.contents.has(rel)
    }
    is_dir(path) {
      let rel = _Dir.normalize(path)
      rel != nil && self._is_dir(rel)
    }
    files() {
      self.contents.keys().sorted()
    }
  }

  # A directory next to the entry script, baked into the binary by
  # `culebra build` when `name` is a string literal; read live from disk
  # otherwise. Only the name is held, so it crosses an Isolate as it is.
  class EmbedDir {
    new(name) {
      _Dir.mark(self, 'embedded')
      self.name = _string_arg('Dir.embedded', name)
    }
    read(path) {
      _need_file(self, 'read', path)
      _Dir.embedded_read(self.name, path)
    }
    list_dir(path) {
      _need_dir(self, 'list_dir', path)
      _Dir.embedded_list_dir(self.name, path)
    }
    exists(path) {
      self.is_file(path) || self.is_dir(path)
    }
    is_file(path) {
      _Dir.embedded_is_file(self.name, path)
    }
    is_dir(path) {
      _Dir.embedded_is_dir(self.name, path)
    }
    size(path) {
      _need_file(self, 'size', path)
      _Dir.embedded_size(self.name, path)
    }
  }

  # The entries of a ZIP archive: a file, read lazily off its central
  # directory, or an archive already in memory. The open archive belongs to
  # this Runtime (`_id` names it in the natives' table), so a ZipArchive never
  # crosses an Isolate — its kind says so — and a worker opens its own.
  # `close()` frees it early, the last reference frees it otherwise, and any
  # use after that is a ClosedError.
  class ZipArchive {
    new(path, bytes) {
      _Dir.mark(self, 'zip')
      if (path == nil) == (bytes == nil) {
        throw {
          kind: 'TypeError',
          message: "type error: Dir.zip takes a path or bytes:, one of the two",
        }
      }
      if path != nil {
        self.path = _disk_path('Dir.zip', path)
        self._id = _Dir.zip_open(self.path)
      } else {
        # The open archive keeps its own copy of the bytes.
        self._id = _Dir.zip_open_bytes(_string_arg('Dir.zip bytes:', bytes))
      }
    }
    read(path) {
      _need_file(self, 'read', path)
      _Dir.zip_read(self._id, path)
    }
    list_dir(path) {
      _need_dir(self, 'list_dir', path)
      _Dir.zip_list_dir(self._id, path)
    }
    exists(path) {
      self.is_file(path) || self.is_dir(path)
    }
    is_file(path) {
      _Dir.zip_is_file(self._id, path)
    }
    is_dir(path) {
      _Dir.zip_is_dir(self._id, path)
    }
    size(path) {
      _need_file(self, 'size', path)
      _Dir.zip_size(self._id, path)
    }
    files() {
      _Dir.zip_files(self._id)
    }
    close() {
      _Dir.zip_close(self._id)
    }
    drop() {
      _Dir.zip_close(self._id) if self._id != nil
    }
  }

  {
    disk: fn (path) {
      DiskDir(path)
    },
    memory: fn (files) {
      MemoryDir(files)
    },
    embedded: fn (name) {
      EmbedDir(name)
    },
    zip: fn (path = nil, *, bytes = nil) {
      ZipArchive(path, bytes)
    },
  }
}
let Dir = _dir_module()
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
# frame (`_fork` below) and drives the fresh copy, leaving the snapshot intact
# — that is what makes a continuation multi-shot.
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
  let _id_resume = fn (v = nil) {
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

  # One frame of a fork: the native shallow copy, then the frame's own chance
  # to finish it. A body local some closure reads lives in a box (so the
  # closure captures the box rather than the frame), and the shallow copy
  # aliases that box — every fork would then write the same one, where the
  # scalar it replaced was copied per fork. `_eff_refork` re-boxes them, so a
  # fork's locals are its own whether or not a closure reads them.
  fn _fork(comp) {
    let c = __eff_copy(comp)
    c._eff_refork()
    c
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
        let resume = fn (v = nil) {
          _drive(snapshot.map(fn (c) { _fork(c) }), v, ret)
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

inline constexpr const char* TEST_AMBIENT_MODULE_SOURCE = R"=culpre=(# `culebra test`'s ambient bindings, in culebra so that both engines run the
# same ones. The runner (include/cli/test_runner.h) reads __TestRegistry back and
# calls each entry; every field it needs is computed here, so nothing but the
# function value itself has to cross the C++ boundary.
#
# An entry is {name, fn, params, args}: `params` is the positional parameter
# list dependency injection resolves against, and `args` is the fixed argument
# list a `@parametrize` case carries (nil when the entry takes fixtures).
let __TestRegistry = []

# A keyword-only parameter and a `**kwargs` rest have no fixture to bind to.
let __test_params = fn (f) {
  f.params.filter(|p| !p.kw_only && !p.kwargs_rest).map(|p| p.name)
}

let __test_add = fn (name, f, args) {
  __TestRegistry.push({name: name, fn: f, params: __test_params(f), args: args})
}

# `@parametrize` registers `<fn>[i]` entries; a `@test` stacked over it would
# add a bare one that then fails fixture injection, so it stands aside.
let __test_has_cases = fn (fname) {
  __TestRegistry.any(|e| e.name.starts_with("{fname}["))
}

# `test("name", fn)` or `test(fn)` — the decorator form takes the function's
# own name and hands it back, so `@test` leaves the binding alone.
let test = fn (*args) {
  if args.size() == 2 {
    if type_of(args[0]) != "String" {
      throw {
        kind: "TypeError",
        message: "test(name, fn): name must be a String",
      }
    }
    if type_of(args[1]) != "Function" {
      throw {
        kind: "TypeError",
        message: "test(name, fn): fn must be a Function",
      }
    }
    __test_add(args[0], args[1], nil)
    return nil
  }
  if args.size() == 1 {
    let f = args[0]
    if type_of(f) != "Function" {
      throw {
        kind: "TypeError",
        message: "test(fn): argument must be a Function",
      }
    }
    if f.name == "" {
      throw {
        kind: "ValueError",
        message: "@test requires a named function (got anonymous); use test(\"name\", fn) for anonymous bodies",
      }
    }
    __test_add(f.name, f, nil) if !__test_has_cases(f.name)
    return f
  }
  throw {
    kind: "ArityError",
    message: "test() expects 1 or 2 arguments (got {args.size()})",
  }
}

# `@parametrize(cases)` — one entry per case, named `<fn>[i]`. A case that is
# a Tuple or an Array spreads across the parameters; anything else is a single
# argument.
let parametrize = fn (cases) {
  if type_of(cases) != "Array" {
    throw {
      kind: "TypeError",
      message: "parametrize(cases): cases must be an Array",
    }
  }
  fn (f) {
    if type_of(f) != "Function" {
      throw {
        kind: "TypeError",
        message: "parametrize(cases): the decorated value must be a Function",
      }
    }
    if f.name == "" {
      throw {
        kind: "ValueError",
        message: "@parametrize requires a named function",
      }
    }
    for i in range(cases.size()) {
      let c = cases[i]
      let t = type_of(c)
      let spread = t == "Tuple" || t == "Array"
      __test_add("{f.name}[{i}]", f, spread ? c.to_array() : [c])
    }
    f
  }
}
)=culpre=";

