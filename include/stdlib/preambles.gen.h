// Generated from src/preambles/*.cul by misc/gen_preambles.sh — do not edit.
// Edit the .cul sources, then run `just gen-preambles` (CI checks sync).
#pragma once

inline constexpr const char* TIME_MODULE_SOURCE = R"=culpre=(let _time_module = fn () {
  let _type_error = fn (want, got) {
    __raise("TypeError", "type error: expected {want}, got {type_of(got)}")
  }
  # Nanoseconds are a Long, so every step checks the ±292-year range rather
  # than letting the count wrap; a Float rounds to the nearest nanosecond.
  let _out_of_range = fn () {
    __raise("ValueError", "Time: out of range (a Long count of nanoseconds, about ±292 years)")
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
      @_nanos = nanos
    }
    seconds() {
      to_float(@_nanos) / 1000000000.0
    }
    milliseconds() {
      to_float(@_nanos) / 1000000.0
    }
    minutes() {
      @seconds() / 60.0
    }
    hours() {
      @seconds() / 3600.0
    }
    days() {
      @seconds() / 86400.0
    }
    abs() {
      if @_nanos < 0 {
        -self
      } else {
        Duration.new(@_nanos)
      }
    }
    __add__(o) {
      let n = match o {
        d: Duration => d._nanos,
      }
      _type_error("Duration", o) if n == nil
      Duration.new(_add_ns(@_nanos, n))
    }
    __sub__(o) {
      let n = match o {
        d: Duration => d._nanos,
      }
      _type_error("Duration", o) if n == nil
      Duration.new(_sub_ns(@_nanos, n))
    }
    __mul__(n) {
      Duration.new(_scale_ns(n, @_nanos))
    }
    __div__(n) {
      Duration.new(match n {
        i: Long => {
          _out_of_range() if i == -1 && @_nanos == _long_min
          @_nanos / i
        },
        f: Float => _float_ns(to_float(@_nanos) / f),
        _ => _type_error("Long or Float", n),
      })
    }
    __neg__() {
      Duration.new(_sub_ns(0, @_nanos))
    }
    __lt__(o) {
      let n = match o {
        d: Duration => d._nanos,
      }
      _type_error("Duration", o) if n == nil
      @_nanos < n
    }
    __le__(o) {
      let n = match o {
        d: Duration => d._nanos,
      }
      _type_error("Duration", o) if n == nil
      @_nanos <= n
    }
    __eq__(o) {
      let n = match o {
        d: Duration => d._nanos,
      }
      n != nil && @_nanos == n
    }
  }
  class Instant {
    new(nanos) {
      @_nanos = nanos
    }
    iso(utc = true) {
      _Time.iso_nanos(@_nanos, utc)
    }
    format(fmt, utc = false) {
      _Time.format_nanos(@_nanos, fmt, utc)
    }
    parts(utc = false) {
      _Time.parts_nanos(@_nanos, utc)
    }
    weekday(utc = false) {
      _Time.weekday_nanos(@_nanos, utc)
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
        @_nanos,
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
      Instant.new(_Time.start_of_nanos(@_nanos, unit, utc))
    }
    unix() {
      to_float(@_nanos) / 1000000000.0
    }
    unix_nanos() {
      @_nanos
    }
    __add__(o) {
      let n = match o {
        d: Duration => d._nanos,
      }
      _type_error("Duration", o) if n == nil
      Instant.new(_add_ns(@_nanos, n))
    }
    __sub__(o) {
      let r = match o {
        d: Duration => Instant.new(_sub_ns(@_nanos, d._nanos)),
        i: Instant => Duration.new(_sub_ns(@_nanos, i._nanos)),
      }
      _type_error("Duration or Instant", o) if r == nil
      r
    }
    __lt__(o) {
      let n = match o {
        i: Instant => i._nanos,
      }
      _type_error("Instant", o) if n == nil
      @_nanos < n
    }
    __le__(o) {
      let n = match o {
        i: Instant => i._nanos,
      }
      _type_error("Instant", o) if n == nil
      @_nanos <= n
    }
    __eq__(o) {
      let n = match o {
        i: Instant => i._nanos,
      }
      n != nil && @_nanos == n
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
      let p = if fg is Tuple || fg is Array {
        _rgbfg_params(fg[0], fg[1], fg[2])
      } else {
        _fg_params(fg)
      }
      parts.push(p) if p != ""
    }
    if bg != nil {
      let p = if bg is Tuple || bg is Array {
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
      @_w = 0
      @_h = 0
      @_fw = 0
      @_back = []
      @_front = []
      @_bstyle = []
      @_fstyle = []
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
      @_w = w
      @_h = h
      # Reuse the buffers on the common path (size unchanged); only reallocate
      # when the terminal was resized.
      if @_back.size() == n {
        for i in 0..n {
          @_back[i] = " "
          @_bstyle[i] = ""
        }
      } else {
        @_back = []
        @_bstyle = []
        for _ in 0..n {
          @_back.push(" ")
          @_bstyle.push("")
        }
      }
      self
    }
    set(x, y, glyph, style = "") {
      let gs = to_string(glyph)  # graphemes()/slices yield StringView
      if x >= 0 && x < @_w && y >= 0 && y < @_h {
        let idx = y * @_w + x
        @_back[idx] = gs
        @_bstyle[idx] = style
        if x + 1 < @_w {
          # "" marks the right half of a wide glyph, so it must be written and
          # taken back with its owner: a narrow glyph landing here frees the
          # cell the previous wide one held.
          if _Term.width(gs) == 2 {
            @_back[idx + 1] = ""
            @_bstyle[idx + 1] = style
          } else if @_back[idx + 1] == "" {
            @_back[idx + 1] = " "
            @_bstyle[idx + 1] = style
          }
        }
      }
      self
    }
    put(x, y, s, style = "") {
      mut cx = x
      for g in s.graphemes() {
        let gs = to_string(g)
        @set(cx, y, gs, style)
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
      let n = @_w * @_h
      mut out = ""
      # A reshape that keeps the cell count (80x24 -> 48x40) moves every cell to
      # a new column, so the width has to invalidate the front buffer too.
      if @_front.size() != n || @_fw != @_w {
        @_front = []
        @_fstyle = []
        for _ in 0..n {
          @_front.push("\x00")
          @_fstyle.push("")
        }  # force a full repaint
        @_fw = @_w
        out = "\x1b[2J"  # wipe stale content
      }
      mut cy = -1
      mut cx = -1
      mut pen = ""  # SGR currently applied at the terminal ("" = default)
      for y in 0..@_h {
        for x in 0..@_w {
          let idx = y * @_w + x
          let back = @_back[idx]
          if back == "" {
            @_front[idx] = ""
            @_fstyle[idx] = @_bstyle[idx]
          } else {
            let st = @_bstyle[idx]
            # A wide glyph owns the cell to its right, so it is stale when that
            # cell shows anything but a continuation — an overlapping draw wrote
            # there last frame and only the owner can paint over it.
            let half_lost = x + 1 < @_w &&
              @_back[idx + 1] == "" &&
              @_front[idx + 1] != ""
            if back != @_front[idx] || st != @_fstyle[idx] || half_lost {
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
              @_front[idx] = back
              @_fstyle[idx] = st
              # Advance the tracked cursor by the glyph's real display width, the
              # same measure set() uses to blank the continuation cell. Keying off
              # the neighbour cell instead under-counts when overlapping draws have
              # since filled it, drifting cx and stranding a later cell's erase.
              let wide = x + 1 < @_w && _Term.width(back) == 2
              cy = y
              if wide {
                cx = x + 2
                @_front[idx + 1] = ""
                @_fstyle[idx + 1] = st
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
      IO.print(@render())
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
      @_id = _Canvas.sprite_load(rgba_px, w, h)
      @_w = _Canvas.sprite_width(@_id)
      @_h = _Canvas.sprite_height(@_id)
    }
    # Decode a PNG image (its bytes as a String, e.g. from `FS.read`) and
    # upload it. The size comes from the image, so there is nothing to pass.
    # Raises ValueError on anything that isn't a decodable PNG.
    new(png: String) {
      @_id = _Canvas.sprite_from_png(png)
      @_w = _Canvas.sprite_width(@_id)
      @_h = _Canvas.sprite_height(@_id)
    }
    # A blank w×h sprite in one colour — the raw material of an offscreen
    # draw target (see Canvas.draw_to).
    new(w: Long, h: Long, color = 0) {
      @_id = _Canvas.sprite_blank(w, h, color)
      @_w = _Canvas.sprite_width(@_id)
      @_h = _Canvas.sprite_height(@_id)
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
      _Canvas.sprite_free(@_id) if @_id != nil
    }
    width() {
      @_w
    }
    height() {
      @_h
    }
    # Blit the whole sprite to (x, y). flip_x/flip_y mirror; transpose swaps
    # X/Y. Transparent (alpha 0) pixels are skipped.
    draw(x, y, flip_x = false, flip_y = false, transpose = false) {
      _Canvas.blit(
        @_id,
        x,
        y,
        0,
        0,
        @_w,
        @_h,
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
        @_id,
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
        @_id,
        x,
        y,
        w,
        h,
        0,
        0,
        @_w,
        @_h,
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
        @_id,
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
      _Canvas.sprite_to_png(@_id)
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
      @_id = _Canvas.ttf_load(data)
    }
    # A font's native memory frees when the last reference drops, mirroring
    # Sprite. A constructor that threw (bad font bytes) drops before _id was
    # ever set.
    drop() {
      _Canvas.ttf_free(@_id) if @_id != nil
    }
    # Pixel ascent at `size`: how far the tallest glyph reaches above the
    # baseline. draw()/text_width() use this so (x, y) reads as visual
    # top-left, matching Canvas.text's convention even though the native
    # primitive places glyphs by baseline.
    ascent(size) {
      _Canvas.ttf_ascent(@_id, size)
    }
    advance(codepoint, size) {
      _Canvas.ttf_advance(@_id, codepoint, size)
    }
    # Draw `s` at (x, y) -- visual top-left -- in `color` at `size` px. Every
    # Unicode scalar value in `s` is drawn (full Unicode, unlike the built-in
    # ASCII-only bitmap font): an unmapped codepoint draws stb_truetype's
    # .notdef glyph rather than being skipped. No kerning (v1): advance is
    # the sum of each glyph's own width.
    draw(s, x, y, color, size) {
      let baseline = y + @ascent(size)
      mut cx = x
      for cp in s.code_points() {
        cx += _Canvas.ttf_glyph(@_id, cp, cx, baseline, color, size)
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
      let baseline = y + @ascent(size)
      mut cx = x
      for cp in s.code_points() {
        cx += _Canvas.ttf_glyph_screen(@_id, cp, cx, baseline, color, size)
      }
    }
    # Pixel width `s` will occupy at `size` -- for right-aligning / centring.
    text_width(s, size) {
      mut w = 0
      for cp in s.code_points() {
        w += @advance(cp, size)
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
      @_prev = 0
      @_cur = 0
    }
    update() {
      @_prev = @_cur
      @_cur = _Canvas.buttons()
      self
    }
    down(btn) {
      @_cur & btn != 0
    }  # held now
    pressed(btn) {
      @_cur & btn != 0 && @_prev & btn == 0
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

inline constexpr const char* AUDIO_MODULE_SOURCE = R"=culpre=(let _audio_module = fn () {
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

  # A mode list as resonate's natives take it, three numbers a mode.
  let flat_modes = fn (modes: Array, who: String) {
    modes.flat_map(fn (m) {
      if type_of(m) != "Object" {
        __raise(
          "TypeError",
          "{who}: a mode is \{freq, width, gain}, each a number",
        )
      }
      [m.freq, m.width, m.gain]
    })
  }

  # `samples` through a bank of resonances, as a guitar's body rings with its
  # strings: each mode {freq, width, gain} is a two-pole resonator ringing at
  # `freq` Hz, `width` Hz wide, its peak about `gain`; `dry` of the samples
  # themselves is added. The answer is as long as `samples`.
  let resonate = fn (
    samples: Array,
    modes: Array,
    dry: Long | Float = 0.0,
    rate: Long | Float = 44100,
  ) {
    _Audio.resonate(samples, flat_modes(modes, "Audio.resonate"), dry, rate)
  }

  # A plucked string (Karplus-Strong), `seconds` long: `tunings` say what it
  # is tuned to from when, {at, freq, ring} (seconds, Hz, and seconds to ring
  # down by 60 dB), in the order they come, the first from the start; each
  # strike {at, samples} is added into the string from `at` on. A change of
  # tuning keeps the string moving, as a finger hammering on does.
  let pluck = fn (
    seconds: Long | Float,
    tunings: Array,
    strikes: Array = [],
    rate: Long | Float = 44100,
  ) {
    let flat = tunings.flat_map(fn (t) {
      if type_of(t) != "Object" {
        __raise(
          "TypeError",
          "Audio.pluck: a tuning is \{at, freq, ring}, each a number",
        )
      }
      [t.at, t.freq, t.ring]
    })
    for s in strikes {
      if type_of(s) != "Object" || s.samples !is Array {
        __raise(
          "TypeError",
          "Audio.pluck: a strike is \{at, samples}, its samples numbers",
        )
      }
    }
    _Audio.pluck(
      seconds,
      flat,
      strikes.flat_map(|s| [s.at, s.samples.size()]),
      strikes.flat_map(|s| s.samples),
      rate,
    )
  }

  # A harmonic tone `seconds` long, its controls given as points it moves
  # between in straight lines: `freq` and `gain` envelopes of (at, value);
  # `spectrum` of (at, amps), the k-th amp the partial at k times the freq;
  # and `noise` {low, high, seed, steady, pulsed}, band noise from `low` to
  # `high` Hz whose level is heard steadily and pulsing with the tone's
  # wave, each an envelope.
  let partials = fn (
    seconds: Long | Float,
    freq: Array,
    spectrum: Array,
    gain: Array = [],
    noise = nil,
    rate: Long | Float = 44100,
  ) {
    for p in spectrum {
      if p !is Tuple || p.size() != 2 || p[1] !is Array {
        __raise(
          "TypeError",
          "Audio.partials: a spectrum is a list of (at, amps), amps a list of numbers",
        )
      }
    }
    let quiet = noise == nil
    if !quiet && type_of(noise) != "Object" {
      __raise(
        "TypeError",
        "Audio.partials: the noise is \{low, high, seed, steady, pulsed}",
      )
    }
    _Audio.partials(
      seconds,
      freq,
      gain,
      spectrum.flat_map(|p| [p[0], p[1].size()]),
      spectrum.flat_map(|p| p[1]),
      quiet ? [] : [noise.low, noise.high, noise.get("seed", 1)],
      quiet ? [] : noise.get("steady", []),
      quiet ? [] : noise.get("pulsed", []),
      rate,
    )
  }

  # `samples` through a two-pole filter: "low" lets through what is below
  # `cutoff` Hz, "high" what is above it, "band" what is about it. `cutoff`
  # is a number or an envelope of (at, value), and `q` the resonance.
  let filter = fn (
    samples: Array,
    kind: String,
    cutoff: Long | Float | Array,
    q: Long | Float = 0.707,
    rate: Long | Float = 44100,
  ) {
    let at = cutoff is Array ? cutoff : [(0.0, cutoff)]
    _Audio.filter(samples, kind, at, q, rate)
  }

  # `samples` times `gain`, an envelope of (at, value) it moves between in
  # straight lines.
  let envelope = fn (samples: Array, gain: Array, rate: Long | Float = 44100) {
    _Audio.envelope(samples, gain, rate)
  }

  # The `parts` added together, each samples or {samples, at, gain}: from
  # `at` seconds on (0 if left out), times `gain` (1). The answer runs to the
  # end of the part that ends last.
  let mix = fn (parts: Array, rate: Long | Float = 44100) {
    let each = parts.map(|p| type_of(p) == "Object" ? p : {samples: p})
    _Audio.mix(
      each.map(|p| p.get("samples", nil)),
      each.flat_map(|p| [p.get("at", 0.0), p.get("gain", 1.0)]),
      rate,
    )
  }

  # One value a part of a graph reads, moved by changes scheduled on the
  # graph's clock, in seconds from its start: set(at, value) jumps to
  # `value` at `at`; ramp(at, value) arrives there in a straight line from
  # the change before; cancel(at) drops the changes after `at` and holds the
  # value it has there. Each answers the value, so changes chain.
  class AudioGraphValue {
    new(graph, part: Long, index: Long) {
      @_graph = graph
      @_part = part
      @_index = index
    }
    set(at: Long | Float, value: Long | Float) {
      self._change(0, at, value)
    }
    ramp(at: Long | Float, value: Long | Float) {
      self._change(1, at, value)
    }
    cancel(at: Long | Float) {
      self._change(2, at, 0)
    }
    # A change as _Audio.graph_schedule takes it: `how` 0 sets, 1 ramps and
    # 2 cancels.
    _change(how, at, to) {
      _Audio.graph_schedule(@_graph._id, [@_part, @_index, how, at, to])
      self
    }
  }

  # A part of a graph, which keeps its graph as an AudioGraphValue does.
  # What moves in it is an AudioGraphValue: a wave's freq, feedback and
  # phase, a filter's cutoff, a gain's gain, a string's freq and ring, a
  # delay's time, a drive's gain, an impulse's and an lfnoise's freq, a
  # dust's density.
  class AudioGraphPart {
    freq = nil
    feedback = nil
    phase = nil
    cutoff = nil
    gain = nil
    ring = nil
    time = nil
    density = nil
    new(graph, index: Long) {
      @_graph = graph
      @_index = index
    }
    # Adds what this part answers into `target`: into what the part takes
    # in, or into the value, on top of its schedule, as Web Audio's
    # AudioNode.connect does. A loop has to pass through a delay. Answers
    # `target`.
    connect(target: AudioGraphPart | AudioGraphValue) {
      if target._graph._id != @_graph._id {
        __raise(
          "ValueError",
          "Audio.Graph: a part connects to parts of its own graph",
        )
      }
      if target is AudioGraphValue {
        _Audio.graph_connect(@_graph._id, @_index, target._part, target._index)
      } else {
        _Audio.graph_connect(@_graph._id, @_index, target._index, -1)
      }
      target
    }
  }

  # Parts wired together, which the runtime works out as they play (play)
  # or ahead of time (render), the same either way. It is freed, and stops,
  # with the last reference to it or to any of its parts and values.
  class AudioGraph {
    new(rate: Long | Float = 44100) {
      @_id = _Audio.graph_new(rate)
    }
    # `inputs`, the parts the part takes in, as their indexes in the graph.
    _part(family: String, kind: String, inputs: Array, options: Array) {
      AudioGraphPart.new(
        self,
        _Audio.graph_part(@_id, family, kind, @_indexes(inputs), options),
      )
    }
    _indexes(parts: Array) {
      for p in parts {
        if class_of(p) != AudioGraphPart {
          __raise(
            "TypeError",
            "type error: parameter 'inputs' expects an Array of AudioGraphPart",
          )
        }
        if p._graph._id != @_id {
          __raise(
            "ValueError",
            "Audio.Graph: a part takes in parts of its own graph",
          )
        }
      }
      parts.map(|p| p._index)
    }
    _value(part: AudioGraphPart, index: Long) {
      AudioGraphValue.new(self, part._index, index)
    }
    wave(
      kind: String = "sine",
      freq: Long | Float = 440.0,
      feedback: Long | Float = 0.0,
      phase: Long | Float = 0.0,
    ) {
      let p = @_part("wave", kind, [], [freq, feedback, phase])
      p.freq = @_value(p, 0)
      p.feedback = @_value(p, 1)
      p.phase = @_value(p, 2)
      p
    }
    noise(seed: Long = 1) {
      @_part("noise", "", [], [seed])
    }
    filter(
      kind: String,
      input: AudioGraphPart,
      cutoff: Long | Float = 1000.0,
      q: Long | Float = 0.707,
    ) {
      let p = @_part("filter", kind, [input], [cutoff, q])
      p.cutoff = @_value(p, 0)
      p
    }
    gain(input: AudioGraphPart, gain: Long | Float = 1.0) {
      let p = @_part("gain", "", [input], [gain])
      p.gain = @_value(p, 0)
      p
    }
    mix(inputs: Array, gain: Long | Float = 1.0) {
      let p = @_part("gain", "", inputs, [gain])
      p.gain = @_value(p, 0)
      p
    }
    resonate(input: AudioGraphPart, modes: Array, dry: Long | Float = 0.0) {
      @_part(
        "resonate",
        "",
        [input],
        [dry, ...flat_modes(modes, "Audio.Graph.resonate")],
      )
    }
    pluck(
      input: AudioGraphPart,
      freq: Long | Float = 220.0,
      ring: Long | Float = 2.0,
    ) {
      let p = @_part("pluck", "", [input], [freq, ring])
      p.freq = @_value(p, 0)
      p.ring = @_value(p, 1)
      p
    }
    # `input` may be left for a later connect: a loop takes in a part made
    # after the delay.
    delay(input: AudioGraphPart | Nil = nil, time: Long | Float = 0.0) {
      let p = @_part("delay", "", input == nil ? [] : [input], [time])
      p.time = @_value(p, 0)
      p
    }
    drive(input: AudioGraphPart, gain: Long | Float = 1.0) {
      let p = @_part("drive", "", [input], [gain])
      p.gain = @_value(p, 0)
      p
    }
    limit(
      input: AudioGraphPart,
      ceiling: Long | Float = 0.9,
      release: Long | Float = 0.2,
    ) {
      @_part("limit", "", [input], [ceiling, release])
    }
    impulse(freq: Long | Float = 1.0, phase: Long | Float = 0.0) {
      let p = @_part("impulse", "", [], [freq, phase])
      p.freq = @_value(p, 0)
      p
    }
    dust(density: Long | Float = 1.0, seed: Long = 1) {
      let p = @_part("dust", "", [], [density, seed])
      p.density = @_value(p, 0)
      p
    }
    lfnoise(kind: String = "step", freq: Long | Float = 1.0, seed: Long = 1) {
      let p = @_part("lfnoise", kind, [], [freq, seed])
      p.freq = @_value(p, 0)
      p
    }
    lag(input: AudioGraphPart, time: Long | Float = 0.1) {
      @_part("lag", "", [input], [time])
    }
    decay(input: AudioGraphPart, time: Long | Float = 1.0) {
      @_part("decay", "", [input], [time])
    }
    abs(input: AudioGraphPart) {
      @_part("math", "abs", [input], [-1.0, 1.0])
    }
    clip(
      input: AudioGraphPart,
      lo: Long | Float = -1.0,
      hi: Long | Float = 1.0,
    ) {
      @_part("math", "clip", [input], [lo, hi])
    }
    exp(input: AudioGraphPart) {
      @_part("math", "exp", [input], [-1.0, 1.0])
    }
    output(part: AudioGraphPart) {
      if part._graph._id != @_id {
        __raise(
          "ValueError",
          "Audio.Graph: the output is a part of its own graph",
        )
      }
      _Audio.graph_output(@_id, part._index)
    }
    render(seconds: Long | Float) {
      _Audio.graph_render(@_id, seconds)
    }
    play() {
      _Audio.graph_play(@_id)
    }
    stop() {
      _Audio.graph_stop(@_id)
    }
    now() {
      _Audio.graph_now(@_id)
    }
    drop() {
      _Audio.graph_free(@_id) if @_id != nil
    }
  }

  # A one-shot sample decoded once from WAV, MP3 or Ogg bytes (a String, e.g.
  # from FS.read or a Dir) and played per call. One voice: play() restarts
  # it. Raises ValueError when the bytes are none of the three formats.
  class Sound {
    new(data: String) {
      @_id = _Audio.sound_load(data)
      @_data = data
    }
    # One from samples the program made: -1.0..1.0, mono, `rate` a second,
    # kept as 16 bits as a WAV file keeps them.
    new(samples: Array, rate: Long = 44100) {
      let made = _Audio.sound_samples(samples, rate)
      @_id = made[0]
      @_data = made[1]
    }
    # The bytes it was made from: the WAV, MP3 or Ogg it was given, or the
    # 16-bit WAV of the samples. Audio.Sound.new(s.data()) is the same sound.
    data() {
      @_data
    }
    play() {
      _Audio.sound_play(@_id)
    }
    stop() {
      _Audio.sound_stop(@_id)
    }
    playing() {
      _Audio.sound_playing(@_id)
    }
    volume(v: Long | Float) {
      _Audio.sound_volume(@_id, v)
    }
    pitch(p: Long | Float) {
      _Audio.sound_pitch(@_id, p)
    }
    pan(p: Long | Float) {
      _Audio.sound_pan(@_id, p)
    }
    # Free the decoded sample with the last reference (a constructor that
    # threw drops before _id was ever set).
    drop() {
      _Audio.sound_free(@_id) if @_id != nil
    }
  }

  # A streamed file from MP3 or Ogg bytes. The runtime keeps it fed, so a
  # program with no frame loop keeps its music. stop() rewinds; pause() holds
  # the position. Raises ValueError when the bytes are neither format.
  class Music {
    new(data: String, loop: Bool = true) {
      @_id = _Audio.music_load(data, loop)
    }
    play() {
      _Audio.music_play(@_id)
    }
    stop() {
      _Audio.music_stop(@_id)
    }
    pause() {
      _Audio.music_pause(@_id)
    }
    resume() {
      _Audio.music_resume(@_id)
    }
    playing() {
      _Audio.music_playing(@_id)
    }
    seek(seconds: Long | Float) {
      _Audio.music_seek(@_id, seconds)
    }
    volume(v: Long | Float) {
      _Audio.music_volume(@_id, v)
    }
    pitch(p: Long | Float) {
      _Audio.music_pitch(@_id, p)
    }
    pan(p: Long | Float) {
      _Audio.music_pan(@_id, p)
    }
    drop() {
      _Audio.music_free(@_id) if @_id != nil
    }
  }

  # PCM the script synthesises, a block at a time. push takes the whole frames
  # in `samples` (stereo: interleaved L,R) and answers how many it took; with
  # no audio device nothing plays, and the block still counts.
  class PCM {
    new(rate: Long, channels: Long, buffer: Long) {
      @_id = _Audio.pcm_new(rate, channels, buffer)
    }
    ready() {
      _Audio.pcm_ready(@_id)
    }
    needed() {
      _Audio.pcm_needed(@_id)
    }
    push(samples: Array) {
      _Audio.pcm_push(@_id, samples)
    }
    submit() {
      _Audio.pcm_submit(@_id)
    }
    latency() {
      _Audio.pcm_latency(@_id)
    }
    play() {
      _Audio.pcm_play(@_id)
    }
    stop() {
      _Audio.pcm_stop(@_id)
    }
    pause() {
      _Audio.pcm_pause(@_id)
    }
    resume() {
      _Audio.pcm_resume(@_id)
    }
    playing() {
      _Audio.pcm_playing(@_id)
    }
    volume(v: Long | Float) {
      _Audio.pcm_volume(@_id, v)
    }
    pitch(p: Long | Float) {
      _Audio.pcm_pitch(@_id, p)
    }
    pan(p: Long | Float) {
      _Audio.pcm_pan(@_id, p)
    }
    drop() {
      _Audio.pcm_free(@_id) if @_id != nil
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
      @_id = _Audio.capture_new(rate, channels)
    }
    ready() {
      _Audio.capture_ready(@_id)
    }
    start() {
      _Audio.capture_start(@_id)
    }
    stop() {
      _Audio.capture_stop(@_id)
    }
    running() {
      _Audio.capture_running(@_id)
    }
    waiting() {
      _Audio.capture_waiting(@_id)
    }
    read(frames = nil) {
      _Audio.capture_read(@_id, frames == nil ? -1 : frames)
    }
    drop() {
      _Audio.capture_free(@_id) if @_id != nil
    }
  }

  {
    available: fn () {
      _Audio.available()
    },
    # latency(seconds) asks for the device's period before it opens;
    # latency() answers the one it runs at, nil when it is not open.
    latency: fn (seconds: Long | Float | Nil = nil) {
      return _Audio.latency() if seconds == nil
      if !(seconds > 0 && seconds <= 0.5) {
        __raise("ValueError", "Audio.latency: a period is above 0 and at most 0.5 seconds, not {seconds}")
      }
      if !_Audio.latency_ask(seconds) {
        __raise("ValueError", "Audio.latency: the device is open already; ask before the first sound")
      }
      nil
    },
    capture_available: fn () {
      _Audio.capture_present()
    },
    tone: tone,
    resonate: resonate,
    pluck: pluck,
    partials: partials,
    filter: filter,
    envelope: envelope,
    mix: mix,
    # A recording's bytes (WAV, MP3 or Ogg) as it is: {rate, channels}, each
    # channel its samples, -1.0..1.0.
    decode: fn (data: String) {
      let made = _Audio.decode(data)
      {rate: made[0], channels: made.slice(1, made.size())}
    },
    # The magnitudes of `samples` a frame at a time: frames[t][k] is bin k
    # (k * rate / size Hz) of the `size` samples from t * hop on.
    spectrum: fn (samples: Array, size: Long = 2048, hop: Long = 512) {
      _Audio.spectrum(samples, size, hop)
    },
    # The same frames' bins as they are: {re, im}, re[t][k] and im[t][k]
    # bin k's real and imaginary parts.
    stft: fn (samples: Array, size: Long = 2048, hop: Long = 512) {
      let read = _Audio.stft(samples, size, hop)
      {re: read[0], im: read[1]}
    },
    # The samples frames of bins stand for, as Audio.stft answers them: each
    # frame's inverse through the window, overlap-added and divided by the
    # window's squares there.
    istft: fn (re: Array, im: Array, size: Long = 2048, hop: Long = 512) {
      _Audio.istft(re, im, size, hop)
    },
    # `samples` taken `from` times a second, as they are taken `to` times.
    resample: fn (samples: Array, from: Long | Float, to: Long | Float) {
      _Audio.resample(samples, from, to)
    },
    # The pitch of `samples` every 10 ms, searched from `low` to `high` Hz:
    # {hz, clarity}, hz[t] the pitch about t * 0.01 s (nil where there is
    # none) and clarity[t] how periodic the sound is there, 0 to 1.
    pitch: fn (samples: Array, low: Long | Float, high: Long | Float, rate: Long | Float = 44100) {
      let read = _Audio.pitch(samples, low, high, rate)
      {hz: read[0], clarity: read[1]}
    },
    # The harmonics of `samples` along their pitch every 10 ms (`f0`, as
    # Audio.pitch's hz: nil where there is none): {harmonics, rest},
    # harmonics[t] the amplitudes of frame t's `count` harmonics (nil where
    # it has no pitch) and rest the samples with the harmonics taken out.
    harmonics: fn (samples: Array, f0: Array, count: Long = 40, rate: Long | Float = 44100) {
      let read = _Audio.harmonics(samples, f0, count, rate)
      {harmonics: read[0], rest: read[1]}
    },
    # How loud `samples` are at their loudest, in LUFS; nil when silent.
    loudness: fn (samples: Array, rate: Long | Float = 44100) {
      _Audio.loudness(samples, rate)
    },
    # How loud `samples` are over their whole length, in LUFS (BS.1770's
    # integrated loudness, gated); nil when silent.
    integrated_loudness: fn (samples: Array, rate: Long | Float = 44100) {
      _Audio.integrated_loudness(samples, rate)
    },
    Graph: AudioGraph,
    Sound: Sound,
    Music: Music,
    PCM: PCM,
    Capture: Capture,
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

inline constexpr const char* KAUAI_MODULE_SOURCE = R"=culpre=(# Kauai: the Kauai language (docs/kauai/language.md), read, checked and
# played. A song is expanded to its notes, each timed in seconds, and handed to
# the runtime whole as a schedule (_Audio.schedule_*), which starts every note on
# the audio stream's own clock. A namespace of its own, beside Audio, whose
# runtime it plays on.
let _kauai_module = fn () {
  let WHOLE = 1920  # time units in a whole note: a sixty-fourth is 30, a triplet eighth 160
  let GRACE = WHOLE / 64  # what an arpeggio rolls by, and what a grace note takes

  # `line` is where the mistake is: "file.kau:12", or "line 12" in a song given
  # as text.
  fn fail(line, msg) {
    __raise("KauaiError", "{line}: {msg}")
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
  # the next key; `flags` are keys that take none. A word before the first key
  # or after a flag is an error, and each option's reader checks how many words
  # it has, so nothing written on a line is left unread.
  fn options(words, skip, keys, flags, what, line) {
    mut out = {}
    mut key = nil
    for w in words.slice(skip, words.size()) {
      if keys.contains(w) || flags.contains(w) {
        fail(line, "`{w}` is said once a line") if out.has(w)
        key = w
        out[w] = []
      } else {
        fail(line, "{what} takes no `{w}`") if key == nil || flags.contains(key)
        out[key].push(w)
      }
    }
    out
  }

  # An option's one word, or `dflt` when the option is not there.
  fn option_word(o, key, line, dflt) {
    return dflt if !o.has(key)
    fail(line, "{key} takes a word: {key} ...") if o[key].size() != 1
    o[key][0]
  }

  # A line of exactly `n` words, as `form` says: one more is an error, not
  # left unread, and one fewer is not read as nil.
  fn exactly(words, n, line, form) {
    fail(line, form) if words.size() != n
    words
  }

  # The NAME of a `KEYWORD NAME` line, which says nothing more.
  fn line_name(words, line, what) {
    name_at(exactly(words, 2, line, "`{words[0]} NAME`"), 1, line, what)
  }

  # A block's lines: a block inside it is an error, as `form` says.
  fn lines_of(blk, form) {
    let inner = blk.body.find(|x| x.has("head"))
    fail(inner.line, form) if inner != nil
    blk.body
  }

  # `table[key] = value` once: a second one is an error, not one that takes
  # the first one's place unsaid.
  fn put_once(table, key, value, line, what) {
    fail(line, "{what} is written twice") if table.has(key)
    table[key] = value
  }

  # "46%" is 0.46; an echo's level is a whole percent and nothing else.
  fn amount(s, line) {
    let digits = s.strip_suffix("%")
    fail(line, "a level is a whole percent: level 70%") if digits == s || digits.empty() || !digits.is_digit()
    to_long(digits) / 100.0
  }

  # The number `word` is, for option `what`.
  fn number_of(word, line, what) {
    fail(line, "{what} takes a number: {word}") if !is_number(word) || word.starts_with(".")
    word.contains(".") ? to_float(word) : to_long(word)
  }

  # A `vol` is a level on the tones' 0 to 100: a louder one would sound as 100
  # does, and say it did not.
  fn level_in_range(n, line, who) {
    fail(line, "{who}'s vol is 0 to 100, as Audio.tone's: vol 20") if n < 0 || n > 100
    n
  }

  # A voice's or a drum's `vol`, which it needs.
  fn vol_option(o, line, who) {
    let vol = o.get("vol", nil) ?? fail(line, "{who} needs its vol: vol 20")
    fail(line, "vol takes a number: vol 20") if vol.size() != 1
    fail(line, "{who}'s vol is 0 to 100, as Audio.tone's: vol 20") if vol[0].ends_with("%")
    vol_of(vol[0], line, who)
  }

  fn vol_of(word, line, who) {
    level_in_range(number_of(word, line, "vol"), line, who)
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

  # A section's `melody in LO..HI` line, and its `part NAME in LO..HI` line
  # as {NAME: low}.
  fn melody_range(it) {
    fail(it.line, "`melody in LO..HI`") if it.words.size() != 3 || it.words[1] != "in"
    window(it.words[2], it.line)
  }

  fn part_range(it) {
    fail(it.line, '`part NAME in LO..HI`, or a block `part NAME {`') if it.words.size() != 4 || it.words[2] != "in"
    [(name_at(it.words, 1, it.line, "a part"), window(it.words[3], it.line))].to_object()
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

  # A key ("F#m"): a pitch name, and `m` for minor.
  fn key_pc(name, line) {
    let (pc, rest) = split_root(name, line)
    fail(line, "a key is a pitch name, with m for minor: Eb, F#m; not {name}") if rest != "" && rest != "m"
    pc
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

  # A chord symbol ("F#m7/A") as {root, bass, shape}, pitch classes, and as
  # written: its symbol, and its root, quality and bass spelled. The bass is
  # after a `/` that a capital letter follows; `C6/9` is a quality.
  fn chord(written_sym, line) {
    return nil if written_sym == "N.C."
    let sym = written_sym.tr("♭♯", "b#")
    let cut = iota(sym.size()).find(|i| sym.slice(i, i + 1).to_string() == "/" && LETTERS.contains(sym.slice(i + 1, i + 2).to_string()))
    let name = cut == nil ? sym : sym.slice(0, cut).to_string()
    let over = cut == nil ? nil : sym.slice(cut + 1, sym.size()).to_string()
    let (root, written) = split_root(name.to_string(), line)
    let shape = quality_shape(written, written_sym, line)
    let bass = over == nil ? root : split_root(over.to_string(), line)[0]
    let spelled = {symbol: written_sym, root: name.slice(0, name.size() - written.size()).to_string(), quality: written, bass: over}
    {root: root, bass: bass, shape: shape, spelled: spelled}
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
    # Moved, it is no longer spelled as written.
    {...c, root: Math.wrap(c.root + n, 12), bass: Math.wrap(c.bass + n, 12), spelled: nil}
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
  # The jazz articulations, written as a word before the note they mark: into
  # it from below or above, and out of it upward or down.
  let BENDS = ["scoop", "plop", "doit", "fall"]
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
      fail(line, "{tok} is too short to play") if WHOLE / n % (1 << dots) != 0
      length = dotted_len(n, dots)
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

  # Whether `k` is a knob's name: a word in lower case, as `bright`.
  fn is_word(k) {
    !k.empty() && all_of(k.slice(0, 1).to_string(), "abcdefghijklmnopqrstuvwxyz") && all_of(k, "abcdefghijklmnopqrstuvwxyz0123456789")
  }

  fn is_number(t) {
    !t.empty() && all_of(t, "0123456789.")
  }

  fn all_digits(t) {
    !t.empty() && all_of(t, "0123456789")
  }

  # `t` as seconds, written 83 or 83.5; nil where it is not.
  fn seconds_of(t) {
    let (whole, frac) = halves(t, ".") ?? (t, "0")
    all_digits(whole) && all_digits(frac) ? to_float(t) : nil
  }

  # `v` with each Array and Object in it made anew, an Object's entries read
  # and written by subscript (a parameter may be named `keys`).
  fn copied(v) {
    return v.map(copied) if v is Array
    return v if type_of(v) != "Object"
    let out = {}
    for k, x in v {
      out[k] = copied(x)
    }
    out
  }

  # An external instrument's parameter, `name=value`, as a voice in the band
  # and a direction in a score write it: a word and a number.
  fn param_of(text, line) {
    let (k, v) = halves(text, "=") ?? ("", "")
    let (whole, frac) = halves(v, ".") ?? (v, "0")
    let word = is_word(k)
    let number = !whole.empty() && !frac.empty() && all_of(whole, "0123456789") && all_of(frac, "0123456789")
    fail(line, "a parameter is a word and a number, bright=0.8: {text}") if !word || !number
    (k, to_float(v))
  }

  # The parameters as a key, the same whatever order they were set in; read
  # by subscript, since a parameter may be named `keys`.
  fn params_key(params) {
    mut set = []
    for k, x in params {
      set.push((k, x))
    }
    set.sorted_by(|p| p[0]).map(|p| "{p[0]}={p[1]}").join(" ")
  }

  let TEMPO_FORM = "a tempo is N, or a value and N: 4.=64"

  # A tempo as written: `96`, or `4.=64` for a value's beats a minute (a dotted
  # quarter's here), as {value, dots, per}; nil for a word that is no tempo.
  fn tempo_written(t, line) {
    let parts = halves(t, "=")
    return is_number(t) ? {value: 4, dots: 0, per: to_float(t)} : nil if parts == nil
    let (v, n) = (parts[0].to_string(), parts[1].to_string())
    fail(line, TEMPO_FORM) if !is_number(n) || v.empty() || !all_of(v, "0123456789.")
    let beat = value_of(melody_token("c{v}", line).length ?? fail(line, TEMPO_FORM))
    {value: beat.value, dots: beat.dots, per: to_float(n)}
  }

  # A tempo as written, as quarter notes a minute.
  fn quarters_of(t) {
    t.value == 4 && t.dots == 0 ? t.per : t.per * dotted_len(t.value, t.dots) / (WHOLE / 4)
  }

  # How long note value `n` with `dots` dots is, in units.
  fn dotted_len(n, dots) {
    WHOLE / n * 2 - WHOLE / n / (1 << dots)
  }

  # A written length in units as its note value and dots: 1440 is a dotted
  # half's {value: 2, dots: 1}.
  fn value_of(units) {
    for v in NOTE_VALUES {
      mut dots = 0
      while WHOLE / v % (1 << dots) == 0 {
        return {value: v, dots: dots} if dotted_len(v, dots) == units
        dots += 1
      }
    }
    nil
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
        # `\bright=0.8`: the line's external instruments take the parameter
        # from the next note.
        t.starts_with('\') && t.contains("=") => {
          let (name, value) = param_of(t.slice(1, t.size()).to_string(), line)
          items.push({param: name, value: value})
          i += 1
        },
        t.starts_with('\') => {
          let word = t.slice(1, t.size()).to_string()
          let takes = word == "tempo" || word == "rit" || word == "accel"
          let written = takes && i + 1 < toks.size() ? tempo_written(toks[i + 1], line) : nil
          let arg = written == nil ? nil : quarters_of(written)
          fail(line, "\\{word} takes a tempo: \\{word} 100") if (word == "rit" || word == "accel") && arg == nil
          items.push({direct: word, arg: arg, written: written})
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

  # `events` with each jazz articulation written before a note in the bar
  # taken into that note's marks.
  fn fold_bends(events, line) {
    mut pending = []
    mut out = []
    for e in events {
      cond {
        e.has("direct") && BENDS.contains(e.direct) => {
          fail(line, "\\{e.direct} is written once for its note") if pending.contains(e.direct)
          pending.push(e.direct)
        },
        e.has("length") && !pending.empty() => {
          fail(line, "\\{pending[0]} marks a note, not a rest") if e.rest
          out.push({...e, marks: [...e.marks, ...pending]})
          pending = []
        },
        _ => out.push(e),
      }
    }
    fail(line, "\\{pending[0]} with no note after it in its bar") if !pending.empty()
    out
  }

  # A bar's notes (or a pickup's) as a timeline, and how long they last; a
  # first note without a value is a `beat` long.
  fn melody_notes(text, line, beat) {
    let toks = lex(text)
    let (items, end, _) = items_from(toks, 0, beat, line)
    fail(line, "a ] without its [") if end < toks.size()
    let events = fold_bends(timeline(items, 1, 1, line), line)
    (events, events.filter(|e| e.has("length")).map(|e| e.length).sum())
  }

  # A bar's hits as the notes they are read as: `x` (the band strikes) a note,
  # `r` (it rests) a rest, with their values, marks and jazz articulations.
  fn hit_text(text, line) {
    lex(text).map(fn (t) {
      return t if t == "]" || t.ends_with("[") || t.starts_with("\\") && BENDS.contains(t.strip_prefix("\\"))
      mut i = 0
      while i < t.size() && ".!-^".contains(t.slice(i, i + 1).to_string()) {
        i += 1
      }
      let head = t.slice(i, i + 1).to_string()
      fail(line, "hits are x (strike) and r (rest), with a value, the marks . ! - ^ and a jazz articulation: {t}") if head != "x" && head != "r"
      head == "x" ? t.slice(0, i).to_string() + "c" + t.slice(i + 1, t.size()).to_string() : t
    }).join(" ")
  }

  # A new phrase's number; every voice's phrases are numbered apart.
  fn next_phrase(state) {
    state.opened.n += 1
    state.opened.n
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
  # carries a tie, a slur, the dynamics and the parameters from one bar to the
  # next.
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
          state.phrase = next_phrase(state) if state.slur
          # The last note under a slur is not joined to the one after it.
          state.last.on = false if it.slur == ")" && state.last != nil
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
        # A parameter holds for the line's notes after it, until it is set again.
        it.has("param") => {
          mut params = {...state.params}
          params[it.param] = it.value
          state.params = params
        },
        it.rest => {
          state.holds.push((pos, it.length)) if it.marks.contains("fermata")
          fail(line, "a tie into a rest") if !state.ties.empty()
          state.open = nil
          # A rest under a slur ends the phrase it joins.
          state.phrase = next_phrase(state) if state.slur
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
            state.notes.push({pitch: g, start: pos + k * GRACE, mut len: GRACE, line: line, shift: 0, grace: true, span: state.span, legato: nil, phrase: state.slur ? state.phrase : nil, marks: [], rank: 0, params: state.params})
          }
          # The notes a slur joins are a phrase, and so are notes struck together.
          let phrase = cond {
            state.slur => state.phrase,
            struck.size() > 1 => next_phrase(state),
            _ => nil,
          }
          # What is written on the struck notes, for an instrument that hears it.
          let marks = [...it.marks, ...(it.arpeggio ? ["arpeggio"] : [])]
          # Each struck note's line, from the lowest: a note a tie holds keeps
          # its own, and the struck ones take the others in turn.
          let taken = state.ties.keys().map(|tp| state.notes[state.ties[tp]].rank)
          mut ranks = {}
          mut free = 0
          for p in struck {
            while taken.contains(free) {
              free += 1
            }
            ranks[p] = free
            free += 1
          }
          # Whether the struck notes are joined to the next, which the slur's
          # end can change.
          let legato = {mut on: state.slur}
          # Every note, tied ones lengthened.
          mut ties_now = {}
          for k, p in pitches.enumerate() {
            if state.ties.has(p) {
              state.notes[state.ties[p]].len += it.length
            } else {
              state.notes.push({pitch: p, start: pos + late(p), mut len: it.length - late(p), line: line, shift: late(p), grace: false, span: state.span, legato: legato, phrase: phrase, marks: marks, rank: ranks[p], params: state.params})
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
          state.last = legato if !struck.empty()
          state.ties = ties_now
          # A fermata holds the whole band: its written time plays twice as long.
          state.holds.push((pos, it.length)) if it.marks.contains("fermata")
          pos += it.length
        },
      }
    }
  }

  # --- band --------------------------------------------------------------------

  # The instruments the mixer plays, by the channel each is: a voice's and a
  # drum's. An instrument the program gives under `voices:` or `drums:` of the
  # same name is the one a line means.
  let CHIP_VOICES = {pulse: 0, pulse2: 1, triangle: 2, saw: 4}
  let CHIP_DRUMS = {noise: 3}
  let DUTIES = {"1/8": 0, "1/4": 1, "1/2": 2, "3/4": 3}
  # The options of a voice line that take words; `poly` takes none.
  let VOICE_KEYS = ["plays", "vol"]
  # What the mixer's instruments take as `name=value`: the knobs they declare.
  let CHIP_VOICE_KNOBS = ["duty", "env", "gap"]
  let NOISE_KNOBS = ["freq", "len", "env"]

  # The `name=value` words of a line, as (name, value) pairs: a name said
  # once, and its value as written, which the instrument the line plays reads
  # (an instrument the program gives reads a number, one of the mixer's what it
  # declares). Pairs and not an object, since a knob may be named `keys`.
  fn knobs_of(words, line) {
    mut out = []
    for x in words.filter(|x| x.contains("=")) {
      let (k, v) = halves(x, "=") ?? ("", "")
      fail(line, "a knob is a word and its value, bright=0.8: {x}") if k.empty() || v.empty() || !all_of(k, "abcdefghijklmnopqrstuvwxyz0123456789")
      fail(line, "`{k}` is said once a line") if out.any(|p| p[0] == k)
      out.push((k, v))
    }
    out
  }

  # A knob's value as written, or nil for a knob not written.
  fn knob(knobs, key) {
    let p = knobs.find(|x| x[0] == key)
    p == nil ? nil : p[1]
  }

  # The knobs an instrument the program gives is handed, as numbers.
  fn params_of(knobs, line) {
    knobs.map(|p| param_of("{p[0]}={p[1]}", line)).to_object()
  }

  # An instrument of the mixer takes the knobs it declares, and no other.
  fn only_knobs(knobs, declared, what, line) {
    for p in knobs {
      fail(line, "{what} takes {declared.join(", ")}, not {p[0]}") if !declared.contains(p[0])
    }
  }

  fn knob_number(knobs, key, line, dflt) {
    let text = knob(knobs, key)
    text == nil ? dflt : number_of(text, line, key)
  }

  # `env=A,D,R`, in ticks, as {attack, decay, release}.
  fn knob_env(knobs, line) {
    let env = (knob(knobs, "env") ?? "0,0,0").split(",")
    fail(line, "env is attack, decay and release: env=0,3,2") if env.size() != 3
    let n = env.map(|x| number_of(x, line, "env"))
    {attack: n[0], decay: n[1], release: n[2]}
  }

  # The instrument a voice or a drum line plays: one of the mixer's `chips`, or
  # a name for the program's.
  fn instrument_form(chips, who) {
    "{who} plays on {chips.keys().join(", ")} or an instrument named with a capital letter"
  }

  fn check_instrument(name, chips, who, line) {
    fail(line, "{instrument_form(chips, who)}, not {name}") if !chips.has(name) && !is_name(name)
  }

  fn instrument_at(words, chips, who, line) {
    fail(line, instrument_form(chips, who)) if words.size() < 3
    check_instrument(words[2], chips, who, line)
    words[2]
  }

  # A knob is written in the parentheses after its instrument, so a `name=value`
  # word anywhere else on the line is one that has lost its parentheses.
  fn no_stray_knobs(rest, name, line) {
    for x in rest {
      fail(line, "a knob is written in parentheses after its instrument, {name}({x}), and not on its own") if x.contains("=")
    }
  }

  # The words of a voice or a drum line with the knobs taken out of the
  # parentheses after the instrument: `Snare noise(freq=4000 len=3) vol 20`
  # reads as `Snare noise freq=4000 len=3 vol 20`, as the rest of the parser
  # reads them. The knobs are separated by spaces, `pulse` and `pulse()` take
  # none, and the ( follows the name with no space. The instrument is the third
  # word, one of `chips` or a name, which is said before anything is said of its
  # knobs.
  fn open_knobs(words, chips, who, line) {
    return words if words.size() < 3
    fail(line, "the ( of an instrument's knobs follows its name with no space: {words[2]}{words[3]}") if words.size() > 3 && words[3].starts_with("(")
    let head = halves(words[2], "(")
    if head == nil {
      check_instrument(words[2], chips, who, line)
      no_stray_knobs(words.slice(3, words.size()), words[2], line)
      return words
    }
    let (name, first) = head
    fail(line, "a voice or a drum names its instrument before the (: {words[2]}") if name.empty()
    check_instrument(name, chips, who, line)
    # The words from the instrument's on, the first without what the ( opened.
    let tail = [first, ...words.slice(3, words.size())]
    let close = iota(tail.size()).find(|i| tail[i].contains(")")) ?? fail(line, "the knobs of {name} end with a ): {name}(name=value ...)")
    let (given, after) = halves(tail.slice(0, close + 1).join(" "), ")")
    fail(line, "{name}({given}) is followed by a space, or nothing: {after}") if !after.empty()
    let knobs = given.split_whitespace()
    let odd = knobs.find(|x| !x.contains("="))
    fail(line, "the parentheses after {name} hold knobs, name=value, and nothing else: {odd}") if odd != nil
    let rest = tail.slice(close + 1, tail.size())
    no_stray_knobs(rest, name, line)
    [...words.slice(0, 2), name, ...knobs, ...rest]
  }

  # How many octaves from where it is written a voice plays the melody, from
  # the words after `plays melody`: none, `octave up` or `down`, or `N
  # octaves up` or `down` for N of 2 or more.
  fn melody_octaves(w, line) {
    let way = {up: 1, down: -1}
    let many = w.size() == 3 && all_of(w[0], "0123456789") && to_long(w[0]) >= 2
    let n = cond {
      w.empty() => 0,
      w.size() == 2 && w[0] == "octave" && way.has(w[1]) => way[w[1]],
      many && w[1] == "octaves" && way.has(w[2]) => to_long(w[0]) * way[w[2]],
      _ => nil,
    }
    fail(line, "a voice plays the melody as written, an `octave up` or `down`, or `2 octaves up` or `down`: {w.join(" ")}") if n == nil
    n
  }

  fn parse_voice(written, line) {
    let words = open_knobs(written, CHIP_VOICES, "a voice", line)
    let name = name_at(words, 1, line, "a voice")
    let knobs = knobs_of(words, line)
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
    # The instrument is the third word, as the line's form says: a `plays part`
    # before it is no instrument.
    let plays_on = instrument_at(words, CHIP_VOICES, "a voice", line)
    let o = options(w, 3, VOICE_KEYS, ["poly"], "a voice", line)
    # Which instrument it is, mixer's or the program's, and what its knobs
    # mean, `resolve_band` says once the program's instruments are known.
    mut v = {
      name: name,
      instrument: plays_on,
      knobs: knobs,
      poly: o.has("poly"),
      vol: vol_option(o, line, "a voice"),
      parts: parts,
      line: line,
      mut role: nil,
    }
    # A voice with no `plays` plays only what it echoes or harmonizes.
    return v if !o.has("plays")
    let plays = o.plays
    fail(line, "a voice plays melody, roots or chords") if plays.empty()
    match plays[0] {
      "melody" => {
        v.role = "melody"
        v.octaves = melody_octaves(plays.slice(1, plays.size()), line)
      },
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
    fail(line, "a length is N/D of a whole note: {text}") if ![n, d].all(is_count)
    let steps = to_long(n) * WHOLE / to_long(d)
    fail(line, "{text} is not a whole number of steps") if steps * to_long(d) != to_long(n) * WHOLE
    steps
  }

  fn parse_band(b) {
    let name = name_at(b.head, 1, b.line, "a band")
    exactly(b.head, 2, b.line, "a band is `band NAME \{`")
    mut voices = {}
    mut drums = {}
    # A groove row names a voice or a drum, so the two share their names.
    let fresh_name = fn (kind, nm, line) {
      let had = voices.has(nm) ? "voice" : (drums.has(nm) ? "drum" : nil)
      fail(line, had == kind ? "{kind} {nm} is defined twice" : "{nm} names a {had} already, and a {kind} may not share it") if had != nil
    }
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
          fresh_name("voice", v.name, item.line)
          voices[v.name] = v
        },
        # `drum NAME INSTRUMENT[(name=value ...)] vol V [choke GROUP]`: the
        # mixer's noise, or a sound the program supplies, played through to
        # its end; the drums of a choke group stop each other, as an
        # instrument that hears `next` does.
        w[0] == "drum" => {
          let dw = open_knobs(w, CHIP_DRUMS, "a drum", item.line)
          let dn = name_at(dw, 1, item.line, "a drum")
          fresh_name("drum", dn, item.line)
          let plain = dw.filter(|x| !x.contains("="))
          let plays_on = instrument_at(plain, CHIP_DRUMS, "a drum", item.line)
          let o = options(plain, 3, ["vol", "choke"], [], "a drum", item.line)
          let choke = o.get("choke", nil)
          fail(item.line, "a choke group is one name: choke Hats") if choke != nil && (choke.size() != 1 || !is_name(choke[0]))
          drums[dn] = {
            on_hits: at_hits != nil,
            instrument: plays_on,
            knobs: knobs_of(dw, item.line),
            vol: vol_option(o, item.line, "a drum"),
            choke: choke == nil ? nil : choke[0],
            line: item.line,
          }
        },
        verb == "echoes" => {
          fail(item.line, "`V echoes SOURCE late N/D level P%`") if w.size() < 3
          let o = options(w, 3, ["late", "level"], [], "an echo", item.line)
          fail(item.line, "an echo says how late and how loud: late 3/16  level 70%") if o.get("late", []).size() != 1 || o.get("level", []).size() != 1
          fail(item.line, "no voice {w[0]} to echo") if !voices.has(w[0])
          put_once(voices[w[0]], "echo", {
            of: w[2],
            late: whole_fraction(o.late[0], item.line),
            level: amount(o.level[0], item.line),
          }, item.line, "an echo for {w[0]}")
        },
        verb == "harmonizes" => {
          fail(item.line, "`V harmonizes SOURCE under INTERVAL`") if w.size() != 5 || w[3] != "under"
          fail(item.line, "no voice {w[0]} to harmonize") if !voices.has(w[0])
          put_once(voices[w[0]], "harmony", {
            of: w[2],
            under: INTERVALS.get(w[4], nil) ?? fail(item.line, "not an interval: {w[4]}"),
          }, item.line, "a harmony for {w[0]}")
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
    let gname = name_at(b.head, 1, b.line, "a groove")
    exactly(b.head, 2, b.line, "a groove is `groove NAME \{`")
    # The rows of a body, each written once.
    let rows_of = fn (body) {
      mut rows = {}
      for row in body.filter(|x| !x.has("head")) {
        let rname = name_at(row.words, 0, row.line, "a row")
        put_once(rows, rname, {text: row.text.slice(rname.size(), row.text.size()).to_string(), line: row.line}, row.line, "row {rname}")
      }
      rows
    }
    mut out = {name: gname, rows: rows_of(b.body), mut first: {}, mut last: {}}
    mut said = {}
    for it in b.body.filter(|x| x.has("head")) {
      let which = it.head[0]
      fail(it.line, "a groove holds `first` and `last` blocks") if which != "first" && which != "last"
      exactly(it.head, 1, it.line, "`{which} \{` takes no name")
      put_once(said, which, true, it.line, "`{which}`")
      out[which] = rows_of(lines_of(it, "`{which}` holds rows, not blocks"))
    }
    out
  }

  fn parse_section(b) {
    let name = name_at(b.head, 1, b.line, "a section")
    let keyed = b.head.size() > 2 && b.head[2] == "key"
    exactly(b.head, keyed ? 4 : 2, b.line, "a section is `section NAME \{`, or `section NAME key K \{`")
    let key = keyed ? b.head[3] : nil
    key_pc(key, b.line) if keyed
    # Where a `part NAME in LO..HI` line first gives each part a range.
    mut ranged = {}
    mut groove = name
    mut named = false
    # The swing for the bars after a `swing` or `straight` line; nil keeps the
    # song's.
    mut swing = nil
    # The meter for the bars after a `meter` line; nil keeps the song's.
    mut meter = nil
    mut groove_bar = 0
    # The ranges for the bars after a `melody in` or `part NAME in` line; nil
    # (or no entry) keeps the song's.
    mut low = nil
    mut part_low = {}
    mut pickup = nil
    mut body = []
    mut endings = {}
    # The endings played as written, not moved by the song's `in`.
    mut written_endings = []
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
        put_once(last.again, "{nth}", again.notes.empty() ? {...again, notes: last.notes, line: last.line} : again, it.line, "`{w[0]}` under one bar")
      } else if w.size() > 1 && w[1] == "hits:" {
        fail(it.line, "`{w[0]} hits:` gives the hits that time, or . for none") if w.size() < 3
        put_once(last.hits, "{nth}", {text: w.slice(2, w.size()).join(" "), line: it.line}, it.line, "`{w[0]} hits:` under one bar")
      } else {
        fail(it.line, "`{w[0]}:` for the bar, or `{w[0]} Name:` for a part") if w.size() < 2 || !w[1].ends_with(":") || !is_name(w[1])
        last.parts_again["{nth}"] ??= {}
        put_once(last.parts_again["{nth}"], w[1].strip_suffix(":"), {notes: w.slice(2, w.size()).join(" "), line: it.line, how: "under"}, it.line, "`{w[0]} {w[1]}` under one bar")
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
      iota(span_bars).map(fn (k) {
        let src = bars_in[bars_in.size() - span_bars + k]
        let again_parts = src.parts_again.keys().map(|t| (t, {...src.parts_again[t]})).to_object()
        {...src, line: it.line, parts: {...src.parts}, again: {...src.again}, parts_again: again_parts, hits: {...src.hits}, repeat: w[0], repeat_index: k}
      })
    }
    # A `part NAME { ... }` block: a line a bar of its section or ending, `.`
    # for a bar the part has nothing in.
    # A `2nd: ...` line in it is the line above as the part plays it the 2nd
    # time, and is not a bar of its own.
    let add_block = fn (bars_in, blk) {
      let pname = name_at(blk.head, 1, blk.line, "a part")
      exactly(blk.head, 2, blk.line, "a part block is `part NAME \{`")
      let lines = lines_of(blk, "a part block holds its lines, not blocks")
      mut k = -1
      for l in lines {
        let nth_here = ordinal(l.words[0], l.line)
        if nth_here != nil {
          fail(l.line, "in a part block, a `2nd:` line comes under the line it changes") if k < 0 || !l.words[0].ends_with(":")
          bars_in[k].parts_again["{nth_here}"] ??= {}
          put_once(bars_in[k].parts_again["{nth_here}"], pname, {notes: l.words.slice(1, l.words.size()).join(" "), line: l.line, how: "block"}, l.line, "`{l.words[0]}` for one line of part {pname}")
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
    # A `part NAME in LO..HI` line, and where it first gives NAME a range.
    let ranging = fn (it) {
      let range = part_range(it)
      ranged[it.words[1]] ??= it.line
      range
    }
    for it in b.body {
      if it.has("head") {
        if it.head[0] == "part" {
          part_blocks.push(it)
          continue
        }
        fail(it.line, "a section holds `ending` and `part` blocks") if it.head[0] != "ending"
        let written = it.head.slice(2, it.head.size()).join(" ") == "as written"
        exactly(it.head, written ? 4 : 2, it.line, "an ending is `ending N \{`, or `ending N as written \{`")
        mut rows = []
        mut blocks_here = []
        mut gb = groove_bar
        mut swing_here = swing
        mut meter_here = meter
        mut groove_here = groove
        mut named_here = named
        mut low_here = low
        mut part_low_here = part_low
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
            groove_here = line_name(r.words, r.line, "a groove")
            named_here = true
            gb = 0
            continue
          }
          if r.words[0] == "melody" {
            low_here = melody_range(r)
            continue
          }
          if r.words[0] == "part" {
            part_low_here = {...part_low_here, ...ranging(r)}
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
          for placed in repeated(rows, r) ?? [{...bar(r, groove_here, gb, written, named_here, rows.empty() ? nil : rows[rows.size() - 1].chords), low: low_here, part_low: part_low_here}] {
            rows.push({...placed, groove: groove_here, groove_bar: gb, as_written: written, named_groove: named_here, swing: swing_here, meter: meter_here})
            gb += 1
          }
        }
        for blk in blocks_here {
          add_block(rows, blk)
        }
        put_once(endings, it.head[1], rows, it.line, "ending {it.head[1]}")
        written_endings.push(it.head[1]) if written
      } else if it.words[0] == "groove" {
        groove = line_name(it.words, it.line, "a groove")
        named = true
        groove_bar = 0
      } else if it.words[0] == "swing" || it.words[0] == "straight" {
        swing = swing_line(it)
      } else if it.words[0] == "meter" {
        meter = parse_meter(it.words, it.line)
      } else if it.words[0] == "melody" {
        low = melody_range(it)
      } else if it.words[0] == "part" {
        part_low = {...part_low, ...ranging(it)}
      } else if ordinal(it.words[0], it.line) != nil {
        add_again(body, it, ordinal(it.words[0], it.line))
      } else if it.words[0] == "hits:" {
        add_hits(body, it)
      } else if part_line(it) != nil {
        add_part(body, part_line(it), it.words.slice(1, it.words.size()).join(" "), it.line, "under")
      } else if it.words[0] == "pickup" {
        fail(it.line, "a pickup comes before the section's first bar") if !body.empty()
        fail(it.line, "a pickup is written twice") if pickup != nil
        fail(it.line, "a pickup holds its notes: pickup g8 a") if it.words.size() < 2
        pickup = {notes: it.words.slice(1, it.words.size()).join(" "), line: it.line, low: low}
      } else {
        for placed in repeated(body, it) ?? [{...bar(it, groove, groove_bar, false, named, body.empty() ? nil : body[body.size() - 1].chords), low: low, part_low: part_low}] {
          body.push({...placed, groove: groove, groove_bar: groove_bar, as_written: false, named_groove: named, swing: swing, meter: meter})
          groove_bar += 1
        }
      }
    }
    for blk in part_blocks {
      add_block(body, blk)
    }
    # A range is for the bars after it in the section, so for a part the
    # section writes.
    mut written_parts = {}
    for rs in [body, ...endings.keys().map(|en| endings[en])] {
      for r in rs {
        for pname, _ in r.parts {
          written_parts[pname] = true
        }
        for _, pa in r.parts_again {
          for pname, _ in pa {
            written_parts[pname] = true
          }
        }
      }
    }
    for pname, at in ranged {
      fail(at, "a range for part {pname}, which section {name} never writes") if !written_parts.has(pname)
    }
    {name: name, key: key, pickup: pickup, body: body, endings: endings, written_endings: written_endings, latest: latest}
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

  # The lines a song says once, the setting each one is.
  let SETTINGS = ["tempo", "meter", "key", "band", "groove", "melody", "loop", "room", "swing"]

  fn parse_song(b) {
    mut song = {name: name_at(b.head, 1, b.line, "a song"), line: b.line, mut loop: false, plays: [], part_low: {}, ranged: {}, mut meter: {beats: 4, beat: WHOLE / 4}, mut key: "C"}
    exactly(b.head, 2, b.line, "a song is `song NAME \{`")
    mut said = {}
    for it in b.body {
      let w = it.words
      put_once(said, w[0], true, it.line, "the song's {w[0]}") if SETTINGS.contains(w[0])
      match w[0] {
        "tempo" => {
          song.tempo_written = (w.size() == 2 ? tempo_written(w[1], it.line) : nil) ?? fail(it.line, TEMPO_FORM)
          song.tempo = quarters_of(song.tempo_written)
        },
        "meter" => song.meter = parse_meter(w, it.line),
        "key" => {
          song.key = exactly(w, 2, it.line, "a key is a pitch name, with m for minor: key Eb")[1]
          key_pc(song.key, it.line)
        },
        "band" => song.band = line_name(w, it.line, "a band"),
        "groove" => song.groove = line_name(w, it.line, "a groove"),
        "part" => {
          fail(it.line, "`part NAME in LO..HI`") if w.size() != 4 || w[2] != "in"
          let pname = name_at(w, 1, it.line, "a part")
          put_once(song.part_low, pname, window(w[3], it.line), it.line, "part {pname}'s range")
          song.ranged[pname] = it.line
        },
        "melody" => song.low = melody_range(it),
        "loop" => {
          exactly(w, 1, it.line, "`loop` stands alone")
          song.loop = true
        },
        "room" => {
          let wet = w.size() == 2 && w[1].ends_with("%") ? w[1].strip_suffix("%") : ""
          fail(it.line, "a room is how much of its echo is heard: room 40%") if !is_count(wet) || to_long(wet) > 100
          song.room = to_long(wet) / 100.0
        },
        "swing" => song.swing = parse_swing(w, 1, it.line),
        "mark" => {
          let mname = line_name(w, it.line, "a mark")
          put_once(said, "mark {mname}", true, it.line, "mark {mname}")
          song.plays.push({mark: mname})
        },
        _ => {
          let sname = name_at(w, 0, it.line, "a section to play")
          # `xN` may stand anywhere on the line.
          let is_times = |t| t.starts_with("x") && t.size() > 1
          let times = w.filter(is_times)
          fail(it.line, "`xN` is said once a line") if times.size() > 1
          let n = times.empty() ? "1" : times[0].slice(1, times[0].size()).to_string()
          fail(it.line, "`xN` plays it N times, N a count: x2") if !is_count(n)
          let o = options(w.filter(|t| !is_times(t)), 1, ["with", "ending", "in"], [], "a line that plays a section", it.line)
          song.plays.push({
            section: sname,
            with: option_word(o, "with", it.line, nil),
            ending: option_word(o, "ending", it.line, nil),
            key: option_word(o, "in", it.line, nil),
            times: to_long(n),
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

  # Bar `r`'s swing: its section's, else the song's; `straight` none.
  fn swing_at(r, song) {
    r.swing == nil ? song.get("swing", nil) : (r.swing.has("straight") ? nil : r.swing)
  }

  # The range part `pname` reads its notes in, in bar `r`: its section's, else
  # the song's, else the melody's, `melody_low`.
  fn part_low_at(r, pname, song, melody_low) {
    r.part_low.get(pname, nil) ?? song.part_low.get(pname, nil) ?? melody_low
  }

  # The groove bar `r` plays, by name: the one its section names with
  # `groove`; else the groove of the section's own name; else the song's;
  # else none (nil).
  fn groove_at(r, grooves, song) {
    r.named_groove || grooves.has(r.groove) ? r.groove : song.get("groove", nil)
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
    # `arrangement` keeps the band, groove and voicings blocks as they were
    # read, which the score gives back as written.
    mut out = {bands: {}, grooves: {}, sections: {}, songs: [], mut about: nil, arrangement: []}
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
          exactly(top.head, 1, top.line, "`voicings \{` takes no name")
          for it in top.body {
            fail(it.line, "a voicing is a name and its intervals") if it.has("head") || it.words.size() < 3
            let not_semitones = it.words.slice(1, it.words.size()).find(|x| !all_digits(x))
            fail(it.line, "a voicing's intervals are semitones over the root: {not_semitones}") if not_semitones != nil
            # `Cb9` is C flat's 9, so a quality may not start as an accidental does.
            fail(it.line, "a quality does not start with # or b: {it.words[0]}") if "#b".contains(it.words[0].slice(0, 1).to_string())
            fail(it.line, "{it.words[0]} is already a chord quality") if qualities.has(it.words[0]) || SPELLINGS.has(it.words[0])
            qualities[it.words[0]] = it.words.slice(1, it.words.size()).map(|x| to_long(x))
          }
        },
        k => fail(top.line, "unknown block {k}"),
      }
      out.arrangement.push(top) if ["band", "groove", "voicings"].contains(top.head[0])
    }
    out
  }

  # The entry `key` of `table` (`voices:` or `drums:` as given), nil for none.
  # Read by iteration: a table is named by the program's instruments, and one
  # may be called `keys` or `get`.
  fn entry_of(table, key) {
    for k, f in table {
      return f if k == key
    }
    nil
  }

  # The knobs an instrument the program gives is handed, from what a line
  # writes: each as a number held to the range `declared` gives it, over the
  # declared defaults; a knob not declared is an error.
  fn knob_params(declared, knobs, line, who) {
    mut names = []
    mut out = {}
    for name, d in declared {
      names.push(name)
      out[name] = d.default
    }
    for p in knobs {
      fail(line, "{who} takes {names.empty() ? "no knobs" : names.join(", ")}, not {p[0]}") if !names.contains(p[0])
      let (k, v) = param_of("{p[0]}={p[1]}", line)
      out[k] = in_range(declared[k], k, v, line)
    }
    out
  }

  # `v` for knob `k`, held to the range `d` ({default, min, max}) declares.
  fn in_range(d, k, v, line) {
    fail(line, "{k} is from {d.min} to {d.max}, not {v}") if v < d.min || v > d.max
    v
  }

  # What a note's direction (`\bright=1.3`) sets for voice `v`: the knobs its
  # instrument declares, held to their ranges. A knob only another instrument
  # of the band declares is that one's (a line sets them for its instruments),
  # and one none declares is an error. `voices` is the band's.
  fn directed(v, params, voices, line) {
    # An instrument not known yet (Kauai.parse reads no tables) takes them all.
    return params if v.external != nil && v.declared == nil
    mut mine = {}
    for k, val in params {
      let d = v.external == nil ? nil : entry_of(v.declared, k)
      if d != nil {
        mine[k] = in_range(d, k, val, line)
      } else {
        let elsewhere = voices.values().any(|u| u.external != nil && (u.declared == nil || entry_of(u.declared, k) != nil))
        fail(line, "no instrument of the band takes {k}") if !elsewhere
      }
    }
    mine
  }

  # A voice or a drum of the band as it plays: an instrument the program gives
  # under its name in `voices:` or `drums:` is that, and else the mixer's
  # (pulse, pulse2, triangle, saw; noise), whose knobs are those it declares.
  # What each takes is read here, as the line says it. An instrument the
  # program gives takes the knobs it declares and is handed them all, the
  # defaults too; one the tables do not have is not known yet (it is an error
  # when the sounds are made), and its knobs are numbers.
  fn is_given(p, given, chips) {
    entry_of(given, p.instrument.lower()) != nil || !chips.has(p.instrument)
  }

  fn as_given(p, given, kind) {
    let f = entry_of(given, p.instrument.lower())
    return {...p, external: p.instrument, params: params_of(p.knobs, p.line), declared: nil, ins: nil} if f == nil
    let key = p.instrument.lower()
    let ins = instrument_of(kind, key, f)
    {...p, external: p.instrument, params: knob_params(ins.knobs, p.knobs, p.line, "{p.instrument}"), declared: ins.knobs, ins: {...ins, key: key}}
  }

  fn resolve_voice(v, given) {
    return as_given(v, given, VOICE_KIND) if is_given(v, given, CHIP_VOICES)
    only_knobs(v.knobs, CHIP_VOICE_KNOBS, "a voice on {v.instrument}", v.line)
    let duty = knob(v.knobs, "duty") ?? "1/2"
    {
      ...v,
      ...knob_env(v.knobs, v.line),
      external: nil,
      wave: CHIP_VOICES[v.instrument],
      duty: DUTIES.get(duty, nil) ?? fail(v.line, "a duty is 1/8, 1/4, 1/2 or 3/4: {duty}"),
      gap: knob_number(v.knobs, "gap", v.line, 0),
      params: {},
    }
  }

  fn resolve_drum(name, d, given) {
    return {...as_given(d, given, DRUM_KIND), choke: d.choke ?? name} if is_given(d, given, CHIP_DRUMS)
    fail(d.line, "a noise drum sounds one at a time already; choke groups external drums") if d.choke != nil
    only_knobs(d.knobs, NOISE_KNOBS, "a noise drum", d.line)
    let freq = knob(d.knobs, "freq")
    let len = knob(d.knobs, "len")
    fail(d.line, "a noise drum needs its freq and its len: freq=120 len=1") if freq == nil || len == nil
    let (f0, f1) = halves(freq, "->") ?? (freq, freq)
    {
      ...d,
      ...knob_env(d.knobs, d.line),
      freq: number_of(f0, d.line, "freq"),
      end_freq: number_of(f1, d.line, "freq"),
      dur: number_of(len, d.line, "len"),
    }
  }

  fn resolve_band(band, given_voices, given_drums) {
    let voices = given_voices ?? {}
    let drums = given_drums ?? {}
    let resolved = band.voices.keys().map(|k| (k, resolve_voice(band.voices[k], voices))).to_object()
    # A channel plays one note at a time, so it is one voice's.
    mut on_channel = {}
    for v in resolved.values() {
      continue if v.external != nil
      let sharing = on_channel.get(v.wave, nil)
      fail(v.line, "{v.name} and {sharing} both play on {v.instrument}") if sharing != nil
      on_channel[v.wave] = v.name
    }
    {
      ...band,
      voices: resolved,
      drums: band.drums.keys().map(|k| (k, resolve_drum(k, band.drums[k], drums))).to_object(),
    }
  }

  # The file's song expanded, from what parse() read of it: its notes, rows and
  # hits in units, and what times them.
  fn expand(s, file, given_voices = nil, given_drums = nil) {
    fail(where(file, 1), "a file that plays holds one song") if s.songs.size() != 1
    let song = s.songs[0]
    fail(song.line, "a song needs its tempo: tempo 120") if !song.has("tempo")
    fail(song.line, "a song needs its band: band NAME") if !song.has("band")
    let band = resolve_band(s.bands.get(song.band, nil) ?? fail(song.line, "no band {song.band}"), given_voices, given_drums)
    let voices = band.voices
    # The voices that play `which`, in the band's order: any number play roots
    # or chords, each from its own row of the groove.
    let playing = |which| voices.values().filter(|v| v.role == which).collect()
    let bass_vs = playing("bass")
    let chord_vs = playing("chords")
    let melody_v = voices.values().find(|v| v.role == "melody") ?? fail(song.line, "no voice of {band.name} plays melody")
    # The melody's voice sounds it octaves from where it is written (`plays
    # melody octave down`), so what echoes or harmonizes it does too.
    let octave_shift = 12 * melody_v.octaves
    let singers = voices.values().filter(|v| v.role == "melody").map(|v| v.name).collect()
    fail(song.line, "one voice of {band.name} plays melody, not {singers.join(" and ")}") if singers.size() > 1
    # The voices that play a derived part, which follow the melody's voice.
    let derived = voices.values().filter(|v| v.has("echo") || v.has("harmony")).collect()
    for v in derived {
      for d in [v.get("echo", nil), v.get("harmony", nil)] {
        fail(song.line, "{v.name} follows {d.of}, and the melody is {melody_v.name}'s") if d != nil && d.of != melody_v.name
      }
    }
    let song_key = key_pc(song.key, song.line)

    # Every bar the song plays, in order, each from unit `at` for `len` units
    # in its own meter.
    mut bars = []
    mut marks = {}
    let opened = {mut n: 0}
    let fresh_state = fn () {
      return {
        mut ties: {}, notes: [], mut slur: false, mut slur_line: 0, mut phrase: nil, opened: opened, mut level: "mf", mut swell: false, mut swell_dir: nil, mut swell_line: 0, mut last: nil, mut tuplet_depth: 0, mut span: nil, holds: [], points: [], lead: [], mut open: nil, mut params: {},
      }
    }
    let voice_state = fresh_state()
    let song_tempo = song.tempo
    mut tempos = {}
    mut part_states = {}
    # Where each part is first written.
    mut part_lines = {}
    # The band's hits, a line of notes as a part is, which a tie can hold
    # across a bar line.
    let hits_state = fresh_state()
    # A part or the hits a bar leaves out, while a tie holds a note into it.
    let held_over = fn (st, message) {
      for _, held in st.ties {
        fail(st.notes[held].line, message)
      }
    }
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
      } else if p.ending != nil {
        fail(p.line, "{p.section} has no endings, so no ending {p.ending}")
      }
      let from = sec.key == nil ? song_key : key_pc(sec.key, p.line)
      let up = p.key == nil ? 0 : Math.wrap(key_pc(p.key, p.line) - from, 12)
      let shift = up > 6 ? up - 12 : up
      let low_of = |r| r.low ?? song.low ?? fail(p.line, "say `melody in LO..HI`")
      for _ in 0..p.times {
        played[p.section] = played.get(p.section, 0) + 1
        let nth = "{played[p.section]}"
        if sec.pickup != nil {
          let pk = sec.pickup
          # The pickup's first note without a value is a beat of the bar it
          # leads into.
          let into = rows[0].meter ?? song.meter
          let (events, len) = melody_notes(pk.notes, pk.line, into.beat)
          if bars.empty() {
            # At the very start the song begins with the pickup: a bar of its
            # own length, before the first, that the band does not play in.
            write_melody(events, low_of(pk), shift + octave_shift, pk.line, 0, tempos, song_tempo, voice_state)
            bars.push({lead_in: true, at: 0, len: len, beats: 0, beat: into.beat, chords: [nil], parts: {}, line: pk.line, first: false, last: false, swing: nil})
            pos = len
          } else {
            let before = bars[bars.size() - 1]
            fail(pk.line, "the pickup is longer than the bar before it") if len > before.len
            let from_at = pos - len
            fail(pk.line, "the bar before the pickup has to rest where the pickup plays") if !rests(from_at, len) || !voice_state.ties.empty()
            write_melody(events, low_of(pk), shift + octave_shift, pk.line, from_at, tempos, song_tempo, voice_state)
          }
        }
        for i, written_bar in rows.enumerate() {
          let r = as_played(written_bar, nth)
          let n = r.as_written ? 0 : shift
          fail(r.line, "a bar line holds one bar") if r.notes.contains("|")
          let m = r.meter ?? song.meter
          let blen = m.beats * m.beat
          fail(r.line, "with slashes, a bar gives a chord or a / for each of its {m.beats} beats") if r.slashes && r.chords.size() != m.beats
          fail(r.line, "{r.chords.size()} chords do not share {m.beats} beats evenly") if m.beats % r.chords.size() != 0
          let base = pos
          # A bar of a line (`what` names it) as notes, a bar long.
          let bar_notes = fn (text, at_line, what) {
            let (bar_events, bar_len) = melody_notes(text, at_line, m.beat)
            fail(at_line, "{what} {bar_len * 1.0 / m.beat} beats long, want {m.beats}") if bar_len != blen
            bar_events
          }
          # A bar of chords alone is a bar of rest for the melody.
          let written_notes = r.notes.empty() ? repeat(m.beats, "r{WHOLE / m.beat}").join(" ") : r.notes
          write_melody(bar_notes(written_notes, r.line, "the bar is"), low_of(r), n + octave_shift, r.line, base, tempos, song_tempo, voice_state)
          for pname, pt in r.parts {
            if !part_states.has(pname) {
              part_states[pname] = fresh_state()
              part_lines[pname] = pt.line
            }
            write_melody(bar_notes(pt.notes, pt.line, "{pname}'s bar is"), part_low_at(r, pname, song, low_of(r)), n, pt.line, base, tempos, song_tempo, part_states[pname])
          }
          for pname, st in part_states {
            held_over(st, "a tie holds part {pname} on into a bar that has none") if !r.parts.has(pname)
          }
          let hl = r.band_hits
          # The hits this bar plays: from the one a tie holds into it, if any,
          # to the last it writes (hits are single notes, so a held one is the
          # last one written).
          let hit_lo = hits_state.ties.empty() ? hits_state.notes.size() : hits_state.ties.values().min()
          if hl != nil {
            write_melody(bar_notes(hit_text(hl.text, hl.line), hl.line, "the hits are"), 60, 0, hl.line, base, tempos, song_tempo, hits_state)
          } else {
            held_over(hits_state, "a tie holds the hits on into a bar that has none")
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
            swing: swing_at(r, song),
            hit_span: (hit_lo, hits_state.notes.size()),
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
      held_over(st, "{what}: a tie at the end of the song, with no note to tie to")
      fail(st.slur_line, "{what}: a slur the song never closes") if st.slur
      fail(st.swell_line, "{what}: a {st.swell_dir} that nothing ends") if st.swell
    }
    unfinished(voice_state, "the melody")
    unfinished(hits_state, "the hits")
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
    # A part is played by a voice, and a range is for a part the song writes.
    for pname, at in part_lines {
      fail(at, "no voice of {band.name} plays part {pname}: give one `plays part {pname}`") if players(pname).empty()
    }
    for pname, at in song.ranged {
      fail(at, "a range for part {pname}, which the song never writes") if !part_lines.has(pname)
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
      bar.parts.keys().any(|pname| voices[vname].parts.contains(pname))
    }
    let lead = voice_state.lead
    let chord_at = fn (bar, slot) {
      bar.chords[Math.min(slot * bar.chords.size() / bar.len, bar.chords.size() - 1)]
    }
    let bass_note = fn (bv, cd) {
      at_or_above(cd.bass, bv.low)
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

    # The band's hits in bar `hb` as (offset, struck, units, marks): each one
    # struck in the bar, or held on into it by a tie from the bar before (not
    # struck), for its units in the bar.
    let hits_in = fn (hb) {
      let (lo, hi) = hb.hit_span
      hits_state.notes.slice(lo, hi).map(fn (nt) {
        let begin = Math.max(nt.start, hb.at)
        (begin - hb.at, nt.start >= hb.at, Math.min(nt.start + nt.len, hb.at + hb.len) - begin, nt.marks)
      })
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
    # the next `-` holds on, if any, and `last` the last one struck.
    let row_voice = fn (voice) {
      {voice: voice, notes: [], mut open: nil, mut last: nil}
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
    let strike = fn (rv, in_bar, offset, units, pitches, hit_marks) {
      let o = {at: in_bar.at + offset, mut len: 0, pitches: pitches, marks: hit_marks}
      rv.notes.push(o)
      rv.open = o
      rv.last = o
      hold(rv, in_bar, offset, units)
      # A staccato hit is let go: what follows does not hold it on.
      rv.open = nil if hit_marks.contains("staccato")
    }
    # A hit a tie holds on over the bar line is the note it struck,
    # sounding on, a staccato one too.
    let resume = fn (rv, in_bar, offset, units) {
      rv.open = rv.last
      hold(rv, in_bar, offset, units)
    }
    let bass_rows = bass_vs.map(row_voice)
    let chord_rows = chord_vs.map(row_voice)
    # Drum hits: a noise drum's by unit (the channel sounds one at a time), and
    # the external drums', which sound together.
    mut noise_hits = {}
    mut external_hits = []
    let hit_drum = fn (dn, hit_bar, offset, line, hit_marks) {
      let step = hit_bar.at + offset
      if band.drums[dn].has("external") {
        external_hits.push({at: step, name: dn, marks: hit_marks})
      } else {
        let beat = 1 + offset * 1.0 / hit_bar.beat
        fail(line, "{noise_hits[step].name} and {dn} both hit at beat {beat} of the bar at {hit_bar.line}, on the noise channel") if noise_hits.has(step)
        noise_hits[step] = {name: dn, marks: hit_marks}
      }
    }
    # A derived part needs its voice.
    for b in bars {
      let w = b.get("with", nil)
      if w != nil && !derived.any(|v| v.has(w)) {
        fail(b.with_line, "`with {w}` needs a voice that {w == "echo" ? "echoes" : "harmonizes"} the melody")
      }
    }
    mut grooves_checked = {}
    for bi, b in bars.enumerate() {
      # The band does not play under a pickup that starts the song.
      let gname = b.has("lead_in") ? nil : groove_at(b, s.grooves, song)
      let g = gname == nil ? {rows: {}, first: {}, last: {}} : s.grooves.get(gname, nil) ?? fail(b.named_groove ? b.line : song.line, "no groove {gname}")
      # A row for something the band does not have would never be read; say so
      # instead of leaving it in the file doing nothing.
      let gkey = g.get("name", "")
      if !grooves_checked.has(gkey) {
        grooves_checked[gkey] = true
        for table in [g.rows, g.first, g.last] {
          for rname in table.keys() {
            let own = table[rname]
            fail(own.line, "the band {band.name} has no drum or voice {rname} to play this row") if !band.drums.has(rname) && !band.voices.has(rname)
            let row_role = band.voices.get(rname, nil)?.role
            fail(own.line, "{rname} plays neither roots nor chords, so it has no groove row to play") if band.voices.has(rname) && row_role != "bass" && row_role != "chords"
          }
        }
      }
      # In a bar with hits the band plays them instead of its grooves: each
      # voice that plays roots the chord's bass note, each that plays chords
      # the whole chord, and the drums that play hits strike; every other drum
      # rests.
      let hit_plan = b.has("band_hits") && b.band_hits != nil ? hits_in(b) : nil
      if hit_plan != nil {
        for (offset, struck, _, hm) in hit_plan {
          continue if !struck
          for dn in band.drums.keys() {
            continue if !band.drums[dn].on_hits
            hit_drum(dn, b, offset, b.band_hits.line, hm)
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
            hit_drum(dn, b, k * row.size, row_line, [])
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
      # What voice `pv` plays in the bar, as (slot, token, units, marks): its
      # groove row, token by token; or the hits, each held for its length, nil
      # (which no row can hold) for one a tie holds on from the bar before; or
      # nothing, where it plays a part.
      let pattern = fn (pv) {
        return [] if busy(b, pv.name)
        if hit_plan != nil {
          # A chord voice that is not poly takes the chord's top tone.
          let tok = pv.role == "bass" ? "1" : (pv.poly ? "x" : "99")
          return hit_plan.map(|(off, hit_struck, hit_units, hm)| (off, hit_struck ? tok : nil, hit_units, hm))
        }
        let voice_row = row_steps(b, g, pv.name, false)
        return [] if voice_row == nil
        let (row, row_line) = voice_row
        if pv.role == "chords" {
          let odd = row.toks.find(|t| t != "x" && t != "-" && t != "." && !is_count(t))
          fail(row_line, "{pv.name} plays `{odd}` under the bar at {b.line}: a chord row is places 1 2 3 ..., x, - and .") if odd != nil
          fail(row_line, "{pv.name} plays x, the whole chord, under the bar at {b.line}, and is not poly") if !pv.poly && row.toks.contains("x")
        }
        row.toks.enumerate().map(|(k, t)| (k * row.size, t, row.size, []))
      }
      # What voice `pv` strikes for token `tok` over chord `cd`: a root voice
      # one note, a chord voice the chord or one place of it.
      let pitches_for = fn (pv, cd, tok) {
        if pv.role == "bass" {
          let bass_low = bass_note(pv, cd)
          return [match tok {
            "1" => bass_low,
            "8" => bass_low + 12,
            ">" => next == nil ? bass_low : bass_note(pv, next.chords[0] ?? fail(b.line, "> leads into N.C.")) - 1,
            "<" => next == nil ? bass_low : bass_note(pv, next.chords[0] ?? fail(b.line, "< leads into N.C.")) + 1,
            _ => at_or_above(cd.root + degree(cd, tok, b.line), bass_low),
          }]
        }
        let tones = voiced(pv, cd)
        let picked = tok == "x" ? tones : [tones[Math.min(to_long(tok) - 1, tones.size() - 1)]]
        pv.poly ? picked : [picked[0]]
      }
      for rv in [...bass_rows, ...chord_rows] {
        for (slot, tok, units, hit_marks) in pattern(rv.voice) {
          let c = chord_at(b, slot)
          cond {
            tok == "." || c == nil => rv.open = nil,
            tok == "-" => hold(rv, b, slot, units),
            tok == nil => resume(rv, b, slot, units),
            _ => strike(rv, b, slot, units, pitches_for(rv.voice, c, tok), hit_marks),
          }
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
          # The chord as the band's first voice that plays chords voices it.
          let tones = voiced(chord_vs.empty() ? {rootless: false, from: 60} : chord_vs[0], c)
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
      # Every bar the song plays, in order, from unit `at` for `len` units.
      bars: bars.map(|b| ({at: b.at, len: b.len})),
      melody: {voice: melody_v, notes: voice_state.notes},
      parts: part_states.keys().map(|pname| (pname, {
        notes: part_states[pname].notes,
        players: players(pname),
      })).to_object(),
      # What the groove's voices and the derived parts play, {voice, notes}
      # with each note {at, len, pitches, marks, level}, and the drums' hits,
      # {at, name, marks}; in units.
      rows: [...second, ...bass_rows, ...chord_rows],
      hits: [
        ...noise_hits.keys().map(|u| ({at: u, ...noise_hits[u]})),
        ...external_hits,
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

  # --- the score as written -----------------------------------------------------

  fn gcd(a, b) {
    b == 0 ? a : gcd(b, a % b)
  }

  # A note name read in range `low`: its letter, its accidentals in semitones,
  # and the octave its letter is in, so `cb` just under `c` in a range from D4
  # is C flat 5.
  fn spelled_note(name, low, line) {
    let (letter, shift, _, _) = note_parts(name, line)
    let p = melody_note(name, low, line)
    (LETTERS.slice(letter, letter + 1).to_string(), shift, (p - shift) / 12 - 1)
  }

  # Items of a bar (as items_from reads them) as the score gives them: notes
  # with their pitches spelled in range `low`, rests, and with `hit` the
  # band's hits, which have no pitch; tuplets, slurs, directions and
  # parameters in their places. A jazz articulation written before a note is
  # one of its marks, as the note plays it; `bends` holds those not yet
  # placed.
  fn score_items(items, low, line, hit, bends) {
    items.flat_map(fn (it) {
      cond {
        it.has("group") => {
          let (heard, notated) = (sounding(it), written_len(it.group))
          let count = it.group.filter(|g| g.has("length") || g.has("group")).size()
          let d = gcd(notated, heard)
          let ratio = cond {
            it.p != nil => {actual: it.p, normal: it.q},
            count * heard % notated == 0 => {actual: count, normal: count * heard / notated},
            _ => {actual: notated / d, normal: heard / d},
          }
          [{tuplet: ratio, items: score_items(it.group, low, line, hit, bends)}]
        },
        it.has("slur") => [{slur: it.slur == "(" ? "start" : "stop"}],
        it.has("param") => [{param: it.param, value: it.value}],
        it.has("direct") && BENDS.contains(it.direct) => {
          bends.held.push(it.direct)
          []
        },
        it.has("direct") => [{direction: it.direct, tempo: it.written}],
        _ => {
          let v = value_of(it.length)
          let marks = [...it.marks, ...bends.held]
          bends.held = []
          let grace = fn (nm) {
            let (step, alter, octave) = spelled_note(nm, low, line)
            {step: step, alter: alter, octave: octave}
          }
          let struck = fn (k) {
            let (step, alter, octave) = spelled_note(it.names[k], low, line)
            {step: step, alter: alter, octave: octave, tie: it.tie || it.ties[k]}
          }
          cond {
            it.rest => [{rest: true, value: v.value, dots: v.dots, marks: marks}],
            hit => [{hit: true, tie: it.tie, value: v.value, dots: v.dots, marks: marks}],
            _ => [{pitches: iota(it.names.size()).map(struck), value: v.value, dots: v.dots, marks: marks, graces: it.graces.map(grace), arpeggio: it.arpeggio}],
          }
        },
      }
    })
  }

  # A block as its lines again, as they were read (comments left out).
  fn block_lines(b, indent) {
    return ["{indent}{b.text}"] if !b.has("head")
    ["{indent}{b.head.join(" ")} \{", ...b.body.flat_map(|x| block_lines(x, indent + "  ")), "{indent}}"]
  }

  # The song parse() read, as a score: its sections and endings the song
  # plays, each bar's chords (one a beat), melody, parts and hits, its notes
  # spelled; the song's line and its plays; its band, grooves and voicings as
  # written; and what the band plays, `played`.
  fn score_of(s, played) {
    let sg = s.songs[0]
    let meter_of = |m| ({beats: m.beats, value: WHOLE / m.beat})
    let low_of = |r| r.low ?? sg.low
    let notes = fn (text, line, beat, low, hit = false) {
      let (items, _, _) = items_from(lex(hit ? hit_text(text, line) : text), 0, beat, line)
      score_items(items, low, line, hit, {mut held: []})
    }
    # Each chord a record of its own, its intervals a copy of the table's.
    let chord_of = fn (c) {
      return nil if c == nil
      let w = c.spelled
      {symbol: w.symbol, root: w.root, quality: w.quality, bass: w.bass, intervals: [...c.shape]}
    }
    let per_beat = |chords, beats| iota(beats).map(|k| chord_of(chords[k * chords.size() / beats]))
    # What a bar plays, apart from where it stands: its chords, its notes and
    # its parts', its hits, and what a later time changes (nil for what it
    # keeps, [] for a part or hits it leaves out).
    let content_of = fn (r) {
      let m = r.meter ?? sg.meter
      let melody_of = |text, line| text.empty() ? [] : notes(text, line, m.beat, low_of(r))
      let parts_of = |parts| parts.keys().map(|pn| ({name: pn, notes: parts[pn].notes == "." ? [] : notes(parts[pn].notes, parts[pn].line, m.beat, part_low_at(r, pn, sg, low_of(r)))}))
      let hits_of = |h| h.text == "." ? [] : notes(h.text, h.line, m.beat, low_of(r), hit: true)
      let nths = [...[...r.again.keys(), ...r.parts_again.keys(), ...r.hits.keys()].filter(|k| k != "all").to_set()]
      let base_hits = r.hits.get("all", nil)
      {
        chords: per_beat(r.chords, m.beats),
        melody: melody_of(r.notes, r.line),
        parts: parts_of(r.parts.keys().filter(|pn| r.parts[pn].notes != ".").map(|pn| (pn, r.parts[pn])).to_object()),
        hits: base_hits == nil || base_hits.text == "." ? nil : hits_of(base_hits),
        times: nths.sorted_by(|k| to_long(k)).map(fn (nth) {
          let a = r.again.get(nth, nil)
          {
            time: to_long(nth),
            chords: a == nil ? nil : per_beat(a.chords, m.beats),
            melody: a == nil ? nil : melody_of(a.notes, a.line),
            parts: r.parts_again.has(nth) ? parts_of(r.parts_again[nth]) : [],
            hits: r.hits.has(nth) ? hits_of(r.hits[nth]) : nil,
          }
        }),
      }
    }
    # A run of rows (a section's body or an ending's) as bars. A `%` or `%%`
    # is one where its bars are the ones before them again, with what is under
    # them; a part block can write a part over it, and then the bars are their
    # own.
    let bars_of = fn (rows) {
      let contents = rows.map(content_of)
      let repeat_at = fn (i) {
        let r = rows[i]
        return nil if !r.has("repeat")
        let (n, head) = (r.repeat.size(), i - r.repeat_index)
        iota(n).all(|k| contents[head + k] == contents[head - n + k]) ? {bars: n, index: r.repeat_index} : nil
      }
      iota(rows.size()).map(fn (i) {
        let (r, c) = (rows[i], contents[i])
        {
          meter: meter_of(r.meter ?? sg.meter),
          swing: swing_at(r, sg),
          groove: {name: groove_at(r, s.grooves, sg), bar: r.groove_bar, named: r.named_groove},
          chords: c.chords,
          melody: c.melody,
          parts: c.parts,
          hits: c.hits,
          repeat: repeat_at(i),
          times: c.times,
        }
      })
    }
    let plays = sg.plays.filter(|p| !p.has("mark"))
    let sections = s.sections.keys().filter(|sn| plays.any(|p| p.section == sn)).map(fn (sn) {
      let sec = s.sections[sn]
      let endings = sec.endings.keys().filter(|n| plays.any(|p| p.section == sn && p.ending == n))
      let pk = sec.pickup
      # A pickup's first note without a value is a beat of the bar it leads into.
      let pickup_of = fn () {
        let rows = [...sec.body, ...endings.flat_map(|n| sec.endings[n])]
        {melody: notes(pk.notes, pk.line, ((rows.empty() ? nil : rows[0].meter) ?? sg.meter).beat, low_of(pk))}
      }
      {
        name: sn,
        key: sec.key,
        pickup: pk == nil ? nil : pickup_of(),
        bars: bars_of(sec.body),
        endings: endings.map(|n| ({number: n, as_written: sec.written_endings.contains(n), bars: bars_of(sec.endings[n])})),
      }
    })
    let said = s.about ?? {}
    {
      about: {
        title: said.get("title", nil),
        composer: said.get("composer", nil),
        lyricist: said.get("lyricist", nil),
        arranger: said.get("arranger", nil),
        year: said.get("year", nil),
        source: said.get("source", nil),
        license: said.get("license", nil),
        note: said.get("note", nil),
      },
      song: {
        name: sg.name,
        tempo: sg.tempo_written,
        meter: meter_of(sg.meter),
        key: sg.key,
        swing: sg.get("swing", nil),
        band: sg.band,
        groove: sg.get("groove", nil),
        loop: sg.loop,
        room: sg.get("room", nil),
        plays: sg.plays.map(|p| p.has("mark") ? {mark: p.mark} : {section: p.section, times: p.times, ending: p.ending, with: p.with, key: p.key}),
      },
      sections: sections,
      arrangement: s.arrangement.map(|b| block_lines(b, "").join("\n")).join("\n\n"),
      played: played,
    }
  }

  # --- playing -------------------------------------------------------------------

  # A voice's `vol` is its level at mf.
  let MF = DYNAMICS["mf"]

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
  # (nil for a drum) and `vol` its level on the tones' 0 to 100. A tone
  # carries what `Audio.tone` needs; external_sounds makes the rest.
  fn render(x) {
    let time = clock(x)
    let at = |unit| time(x.swung(unit))
    mut out = []
    let band_level = x.level_at
    # Voice `v`'s note, heard from `t0` to `t1` seconds, as `how` says: its
    # dynamic where it starts and where it ends (`level`, `level_end`), the
    # `marks` written on it, whether a slur joins it to the next (`legato`),
    # its `phrase` (the notes it sounds with; nil for none), its `rank`
    # there, the line it moves in from the lowest, and the `params` its line
    # set for an external instrument.
    let note = fn (v, pitch, t0, t1, how) {
      let marks = how.marks
      let level = how.level
      let loud = marks.contains("accent") || marks.contains("marcato") ? louder(level) : level
      let len = t1 - t0
      let sounded = cond {
        marks.contains("staccato") => len / 2,
        marks.contains("marcato") => len * 3 / 4,
        _ => len,
      }
      if v.external != nil {
        # The rest is for an instrument that plays a phrase at a time.
        out.push({
          at: t0, len: len, by: v.name, pitch: pitch, mut vol: v.vol * loud / MF,
          level: loud / MF, level_end: how.level_end / MF, sounded: sounded,
          phrase: how.phrase, marks: marks, rank: how.rank,
          params: how.get("params", {}),
        })
        return nil
      }
      let vol = v.vol * loud / MF
      let joined = how.legato || marks.contains("tenuto")
      let sustain = Math.max(0.0, sounded - (joined ? 0 : v.gap) / 60.0)
      let f = hz(pitch)
      out.push({
        at: t0, len: len, by: v.name, pitch: pitch, vol: vol,
        tone: [v.wave, f, f, v.attack / 60.0, v.decay / 60.0, sustain, v.release / 60.0, vol, vol, v.duty],
      })
    }
    # A note's last unit: it ends at the dynamic there, before one written
    # after it.
    let last_unit = |start, len| start + Math.max(0, len - 1)
    # A written note as heard, `shift` units later than it is written, at the
    # dynamic `level_at` gives for a unit.
    let written = fn (v, nt, shift, level_at) {
      let h = x.heard({...nt, start: nt.start + shift, span: nt.span == nil ? nil : nt.span.map(|k| k + shift)})
      note(v, h.pitch, time(h.start), time(h.start + h.len), {
        level: level_at(nt.start),
        level_end: level_at(last_unit(nt.start, nt.len)),
        marks: nt.marks,
        legato: nt.legato != nil && nt.legato.on,
        phrase: nt.phrase == nil ? nil : "{v.name} {nt.phrase} {shift}",
        rank: nt.rank,
        params: directed(v, nt.params, x.voices, nt.line),
      })
    }
    let melody = x.melody
    for nt in melody.notes {
      written(melody.voice, nt, 0, band_level)
      # A song that loops plays the pickup it starts with again over its end.
      if x.song.loop && nt.start < x.opening {
        written(melody.voice, nt, x.steps - x.opening, band_level)
      }
    }
    for pname, part in x.parts {
      for vname in part.players {
        for nt in part.notes {
          written(x.voices[vname], nt, 0, |u| x.part_level_at(pname, u))
        }
      }
    }
    # The groove's voices and the derived part, and the drums; a chord a row
    # strikes is a phrase.
    mut chords = 0
    for row in x.rows {
      for n in row.notes {
        let from = at(n.at)
        let until = at(n.at + n.len)
        let level = n.get("level", 1.0)
        chords += 1 if n.pitches.size() > 1
        let how = {
          level: band_level(n.at) * level,
          level_end: band_level(last_unit(n.at, n.len)) * level,
          marks: n.get("marks", []),
          legato: false,
          phrase: n.pitches.size() > 1 ? "{row.voice.name} chord {chords}" : nil,
        }
        let low_first = n.pitches.sorted()
        for tone_pitch in n.pitches {
          note(row.voice, tone_pitch, from, until, {...how, rank: low_first.index_of(tone_pitch)})
        }
      }
    }
    for hit in x.hits {
      let d = x.drums[hit.name]
      # A hit of the band's that is accented strikes a level louder, as a note does.
      let level = hit.marks.contains("accent") || hit.marks.contains("marcato") ? louder(band_level(hit.at)) : band_level(hit.at)
      let hit_vol = d.vol * level / MF
      if d.has("external") {
        out.push({
          at: at(hit.at), len: 0.0, by: hit.name, pitch: nil, mut vol: hit_vol,
          level: level / MF, marks: hit.marks,
        })
      } else {
        out.push({
          at: at(hit.at), len: d.dur / 60.0, by: hit.name, pitch: nil, vol: hit_vol,
          tone: [CHIP_DRUMS.noise, d.freq, d.end_freq, d.attack / 60.0, d.decay / 60.0, d.dur / 60.0, d.release / 60.0, hit_vol, hit_vol, 2],
        })
      }
    }
    out.sorted_by(|e| e.at)
  }

  # What an instrument may hear of a note: a voice its length, its dynamic,
  # its marks, its line in the phrase, when the voice plays next and the
  # parameters it is played with; a drum its dynamic, the marks on the band's
  # hit it strikes, and when it, or its choke group, hits next.
  let VOICE_HEARS = ["len", "level", "line", "marks", "next", "params"]
  let DRUM_HEARS = ["level", "marks", "next"]
  # What an entry of `voices:` or `drums:` calls to make a sound: a voice's
  # `note` (a note at a time) or `phrase` (a phrase at a time, hearing what it
  # asks for); a drum's `hit`.
  let VOICE_KIND = {kwarg: "voices", calls: ["note", "phrase"], short: "note", may: VOICE_HEARS}
  let DRUM_KIND = {kwarg: "drums", calls: ["hit"], short: "hit", may: DRUM_HEARS}

  # An entry of `voices:` or `drums:` as Kauai reads it, `key` its name:
  # {call, make, hears, knobs, weigh}, `call` the name of what makes its sound.
  # It is a function, which stands for `note` (a voice's, called with the
  # pitch and the knobs) or `hit` (a drum's, called with what the hit hears,
  # nothing, and the knobs) and takes no knobs, or an object: the call that
  # makes its sound (`note`, `phrase` or `hit`),
  # `hears` (a phrase's or a hit's: what of the notes it asks for), `knobs`
  # (its parameters, each {default, min, max}: a line's `name=value` is held to
  # them, and all are handed to it, the defaults too) and `weigh` (false to
  # play at its own level, not matched to a pulse's: the default is true).
  # `hears` is nil for what makes its sound a note at a time (a voice's function
  # too); a drum's function hears nothing, `[]`.
  fn instrument_of(kind, key, f) {
    let bad = |why| __raise("ValueError", "Kauai: {kind.kwarg}: `{key}` {why}")
    return {call: kind.short, make: f, hears: kind.short == "hit" ? [] : nil, knobs: {}, weigh: true} if f is Function
    let calls = kind.calls.map(|c| "`{c}`").join(" or ")
    bad("is a function or an object with {calls}, a function") if type_of(f) != "Object"
    let made = kind.calls.filter(|c| f.get(c, nil) is Function)
    bad("has one of {calls}, a function") if made.size() != 1
    let hears = f.get("hears", nil)
    bad("hears an Array of names") if hears != nil && hears !is Array
    bad("makes a sound a note at a time, which hears nothing: no `hears`") if made[0] == "note" && hears != nil
    let heard = (hears ?? []).filter(|h| !kind.may.contains(h))
    bad("hears {heard[0]}; it can hear {kind.may.join(", ")}") if !heard.empty()
    let weigh = f.get("weigh", true)
    bad("weighs or not, true or false") if weigh !is Bool
    let declared = f.get("knobs", {})
    bad("has knobs as an Object, each of default, min and max") if type_of(declared) != "Object"
    mut knobs = {}
    for name, d in declared {
      bad("has a knob `{name}`: a word") if !is_word(name)
      bad("has a knob `{name}`: an Object of default, min and max") if type_of(d) != "Object" || !["default", "min", "max"].all(|n| d.get(n, nil) is Long || d.get(n, nil) is Float)
      let (lo, init, hi) = (to_float(d["min"]), to_float(d["default"]), to_float(d["max"]))
      bad("has a knob `{name}`: the default is from min to max") if init < lo || init > hi
      knobs[name] = {default: init, min: lo, max: hi}
    }
    {call: made[0], make: f[made[0]], hears: made[0] == "note" ? nil : (hears ?? []), knobs: knobs, weigh: weigh}
  }

  # How external sound `name` is made: the instrument `resolve_band` read for
  # `player` (the voice or drum, with the line it is defined on), `instrument_of`'s
  # with its key under `voices:` or `drums:`. One the table does not have is
  # the program's ValueError, which names the keys it has.
  fn instrument(kind, player, what) {
    return player.ins if player.ins != nil
    let name = player.external
    mut names = []
    for k, _ in kind.table ?? {} {
      names.push(k)
    }
    __raise("ValueError", "Kauai: {player.line}: {what} plays {name}, which the mixer has not, and {kind.kwarg}: has no `{name.lower()}` (it has {names.empty() ? "none" : names.join(", ")})")
  }

  # The external sounds the song needs: `build`, key -> how to build one;
  # `plays`, [{at, until, sound, by, vol, external, args}] in seconds, `until`
  # when the notes it plays end (a hit's when it strikes), `external` the
  # instrument's key in `voices:` or `drums:` and `args` what it is handed;
  # and `weighed`, for each voice
  # or drum, the sound it is weighed by (`sound`, its key in `build`) and the
  # pitch of the tone it is weighed against. A function makes a voice's sound
  # for each pitch it plays, and a drum's once. An instrument makes a voice's
  # sound for each phrase it hears differently (the notes a slur joins or
  # that are struck together, or a note alone), played from its first note,
  # and a drum's for each hit; a dynamic it hears is its own to sound, so the
  # notes' `vol` leaves it out. A voice's parameters are the band's with what
  # its line set over them: a function's as they stand at the note, an
  # instrument's as they stand at the phrase's first note (and at each note,
  # for one that hears them).
  fn external_sounds(x, events, voices, drums) {
    let VOICE = {...VOICE_KIND, table: voices}
    let DRUM = {...DRUM_KIND, table: drums}
    let ms = |t| Math.round(t * 1000) / 1000.0
    let hundredths = |v| Math.round(v * 100) / 100.0
    mut build = {}
    mut plays = []
    mut kinds = {}
    mut phrases = {}
    mut hits = []
    # When each voice starts a phrase and each choke group is hit, in order.
    mut starts = {}
    # A sound `by` plays from `at` for notes that end at `until`, which
    # instrument `ins` makes from `args` unless it is made already.
    let sound = fn (key, by, at, until, vol, ins, args) {
      if !build.has(key) {
        let make = ins.make
        build[key] = args.size() == 1 ? || make(args[0]) : || make(args[0], args[1])
      }
      plays.push({at: at, until: until, sound: key, by: by, vol: vol, external: ins.key, args: args})
    }
    for i, e in events.enumerate() {
      continue if e.has("tone")
      let drum = e.pitch == nil
      let player = drum ? x.drums[e.by] : x.voices[e.by]
      let ins = kinds.get_or_put(
        e.by,
        || instrument(drum ? DRUM : VOICE, player, "{drum ? "drum" : "voice"} {e.by}"),
      )
      let params = player.params
      cond {
        ins.hears == nil => sound(
          "{e.by} {e.pitch} {params_key({...params, ...e.params})}",
          e.by,
          e.at,
          e.at + e.len,
          e.vol,
          ins,
          [e.pitch, {...params, ...e.params}],
        ),
        drum => {
          e.vol = player.vol if ins.hears.contains("level")
          let group = "drums {player.choke}"
          starts.get_or_put(group, || []).push(e.at)
          hits.push({e: e, ins: ins, params: params, group: group})
        },
        _ => {
          e.vol = player.vol if ins.hears.contains("level")
          phrases.get_or_put(e.phrase ?? "{e.by} #{i}", || []).push(e)
        },
      }
    }
    let heads = phrases.values().map(|heard| heard.sorted_by(|e| (e.at, e.pitch))).collect()
    for members in heads {
      starts.get_or_put("voice {members[0].by}", || []).push(members[0].at)
    }
    # Seconds from `t` until the next start in `starts[key]`, nil for none.
    let next = fn (key, t) {
      let later = starts[key].find(|a| a > t)
      later == nil ? nil : ms(later - t)
    }
    for h in hits {
      let e = h.e
      let hears = h.ins.hears
      let n = {
        ...(hears.contains("level") ? {level: hundredths(e.level)} : {}),
        ...(hears.contains("marks") ? {marks: e.marks} : {}),
        ...(hears.contains("next") ? {next: next(h.group, e.at)} : {}),
      }
      sound("{e.by} {n}", e.by, e.at, e.at, e.vol, h.ins, [n, h.params])
    }
    for members in heads {
      let head = members[0]
      let hears = kinds[head.by].hears
      let after = next("voice {head.by}", head.at)
      let band = x.voices[head.by].params
      let notes = members.map(|e| ({
        pitch: e.pitch,
        at: ms(e.at - head.at),
        ...(hears.contains("len") ? {len: ms(e.sounded)} : {}),
        ...(hears.contains("level") ? {level: hundredths(e.level), level_end: hundredths(e.level_end)} : {}),
        ...(hears.contains("line") ? {line: e.rank} : {}),
        ...(hears.contains("marks") ? {marks: e.marks} : {}),
        ...(hears.contains("next") ? {next: after} : {}),
        ...(hears.contains("params") ? {params: {...band, ...e.params}} : {}),
      }))
      let params = {...band, ...head.params}
      let until = members.map(|e| e.at + e.len).max()
      sound("{head.by} {notes} {params_key(params)}", head.by, head.at, until, head.vol, kinds[head.by], [notes, params])
    }
    # What each is weighed by: a voice's note at the middle of the pitches it
    # plays, a second long at mf, and a drum's hit at mf, each as the song
    # would ask for it with the band's parameters (so one the song plays
    # already is not built again).
    mut weighed = {}
    mut unweighed = []
    for by in kinds.keys() {
      let ins = kinds[by]
      let drum = x.drums.has(by)
      let params = drum ? x.drums[by].params : x.voices[by].params
      let make = ins.make
      # One that says not to be weighed plays at its own level.
      if !ins.weigh {
        unweighed.push(by)
        continue
      }
      if drum {
        let hears = ins.hears
        let n = {
          ...(hears.contains("level") ? {level: 1.0} : {}),
          ...(hears.contains("marks") ? {marks: []} : {}),
          ...(hears.contains("next") ? {next: nil} : {}),
        }
        let key = "{by} {n}"
        build[key] = || make(n, params) if !build.has(key)
        weighed[by] = {sound: key, pitch: 69}
        continue
      }
      let pitches = events.filter(|e| e.by == by && !e.has("tone")).map(|e| e.pitch).sorted()
      let pitch = pitches[pitches.size() / 2]
      if ins.hears == nil {
        let key = "{by} {pitch} {params_key(params)}"
        build[key] = || make(pitch, params) if !build.has(key)
        weighed[by] = {sound: key, pitch: pitch}
        continue
      }
      let hears = ins.hears
      let notes = [{
        pitch: pitch,
        at: 0.0,
        ...(hears.contains("len") ? {len: 1.0} : {}),
        ...(hears.contains("level") ? {level: 1.0, level_end: 1.0} : {}),
        ...(hears.contains("line") ? {line: 0} : {}),
        ...(hears.contains("marks") ? {marks: []} : {}),
        ...(hears.contains("next") ? {next: nil} : {}),
        ...(hears.contains("params") ? {params: params} : {}),
      }]
      let key = "{by} {notes} {params_key(params)}"
      build[key] = || make(notes, params) if !build.has(key)
      weighed[by] = {sound: key, pitch: pitch}
    }
    {build: build, plays: plays, weighed: weighed, unweighed: unweighed}
  }

  # How loud a tone of pitch `pitch` is at vol 100 on a pulse at a half's
  # duty (in LUFS): what an external sound at vol 100 is made as loud as.
  fn tone_loudness(pitch) {
    _Audio.tone_loudness(hz(pitch))
  }

  # Samples a second, as the mixer renders a song.
  let RATE = 44100
  # The numbers in a row of the schedule, which the engine defines, and a row
  # held to that width: a row of any other is read from the wrong place.
  let STRIDE = _Audio.schedule_stride()
  fn schedule_row(fields) {
    __raise("RuntimeError", "Kauai: a schedule row is {fields.size()} numbers, the engine reads {STRIDE}") if fields.size() != STRIDE
    fields
  }
  # Seconds a part of a song is heard from before it starts.
  let LEAD = 1.0

  # A song, read and checked, ready to play.
  class Song {
    new(x, voices, drums) {
      @_events = render(x)
      let time = clock(x)
      @_length = time(x.steps)
      @_loop_at = x.song.loop ? time(x.loop_at) : -1.0
      @_room = x.song.get("room", 0.0)
      @_marks = x.marks.keys().map(|k| (k, time(x.marks[k]))).to_object()
      @_about = x.about
      let external = external_sounds(x, @_events, voices, drums)
      @_build = external.build
      @_plays = external.plays
      @_weighed = external.weighed
      @_unweighed = external.unweighed
      @_weights = nil
      # Each sound's voice or drum, and when it first plays.
      @_by = {}
      @_first = {}
      for p in @_plays.sorted_by(|p| p.at) {
        @_by[p.sound] = p.by
        @_first.get_or_put(p.sound, || p.at)
      }
      for by, w in @_weighed {
        @_by[w.sound] = by
      }
      let weigh = @_weighed.values().map(|w| w.sound).collect()
      # What prepare() builds, the next last: the sounds the others are
      # weighed by, then each in the order it first plays.
      @_todo = [...weigh, ...@_first.keys().filter(|k| !weigh.contains(k))]
      @_todo.reverse()
      # Seconds the sounds built so far took, and how many, by voice or drum.
      @_took = {}
      @_now = || _Time.monotonic()
      @_sounds = {}
      @_schedule = nil
      @_slots = nil
      @_started = nil
      @_volume = 1.0
    }

    # External sound `key`, built the first time it is asked for, and given to
    # the song playing early when it is one of its sounds.
    _sound(key) {
      return @_sounds[key] if @_sounds.has(key)
      let began = @_now()
      let s = @_build[key]()
      if type_of(s) != "Sound" {
        __raise(
          "TypeError",
          "Kauai: an external instrument answers an Audio.Sound, not {type_of(s)}",
        )
      }
      let (took, n) = @_took.get(@_by[key], (0.0, 0))
      @_took[@_by[key]] = (took + @_now() - began, n + 1)
      @_sounds[key] = s
      _Audio.schedule_fill(@_schedule, @_slots[key], s._id) if @_slots != nil && @_slots.has(key)
      s
    }

    # Build one external sound the song still needs; answers how many remain.
    # One whose building throws is still to build.
    prepare() {
      if !@_todo.empty() {
        @_sound(@_todo[-1])
        @_todo.pop()
      }
      @_todo.size()
    }

    # The song as the mixer takes it, with `plays` of its external sounds and
    # its tones that start before `to` seconds (nil: every one), after
    # building what they need: each external sound at its vol on the tones'
    # scale. A voice or a drum is weighed by its sound (the loudness of the
    # loudest 400 ms), and played as loud as a pulse at a half's duty would be
    # at that vol and the pitch it was weighed at; one whose loudness is not
    # known (silence, or compressed audio not yet decoded) at vol / 100 of its
    # own level.
    _list(plays, to) {
      @_weigh()
      @_need(plays.map(|p| p.sound))
      @_rows(plays, to, |key| @_sounds[key]._id)
    }

    # The sounds `keys` built, in the order prepare() takes them.
    _need(keys) {
      let needed = keys.to_set()
      for i in 0..@_todo.size() {
        let key = @_todo[-1 - i]
        @_sound(key) if needed.contains(key)
      }
      # What prepare() has left is what is still not built.
      @_todo = @_todo.filter(|k| !@_sounds.has(k))
    }

    # Each voice's and drum's weight, from the sound it is weighed by.
    _weigh() {
      return nil if @_weights != nil
      @_need(@_weighed.values().map(|w| w.sound))
      let weighed = @_weighed.keys().map(fn (by) {
        let w = @_weighed[by]
        let l = _Audio.sound_loudness(@_sounds[w.sound]._id)
        (by, l == nil ? 1.0 : 10.0 ** ((tone_loudness(w.pitch) - l) / 20.0))
      }).to_object()
      @_weights = {...@_unweighed.map(|by| (by, 1.0)).to_object(), ...weighed}
    }

    # The rows of `plays` and of the tones that start before `to` (nil: every
    # one), a sound named by `id(key)`.
    _rows(plays, to, id) {
      let tones = @_events.filter(|e| e.has("tone") && (to == nil || e.at < to)).map(|e| schedule_row([e.at, ...e.tone]))
      let sounds = plays.map(|p| schedule_row([p.at, -1, id(p.sound), p.vol / 100.0 * @_weights[p.by], 0, 0, 0, 0, 0, 0, 0]))
      # In time order: the mixers read it from the head.
      [...tones, ...sounds].sorted_by(|e| e[0]).flat_map(|e| e)
    }

    # The schedule of a song played early: its external sounds in slots the
    # mixer waits on until each is built (those built already given), after
    # building the sounds the others are weighed by; nil where the build
    # cannot hold a song for a sound.
    _open() {
      @_weigh()
      let slots = {}
      for p in @_plays {
        slots.get_or_put(p.sound, || slots.size())
      }
      let id = _Audio.schedule_open(@_rows(@_plays, nil, |key| slots[key]), slots.size(), @_length, @_loop_at)
      return nil if id == nil
      @_slots = slots
      for key, slot in slots {
        _Audio.schedule_fill(id, slot, @_sounds[key]._id) if @_sounds.has(key)
      }
      id
    }

    # Whether the song can start: each sound still to build, built from now in
    # the order prepare() takes them, comes in half the time the song takes
    # to reach it. A sound is reckoned to take as long as its voice's or
    # drum's sounds have on average: the one it is weighed by is built first.
    _ahead() {
      mut ready = 0.0
      for i in 0..@_todo.size() {
        let key = @_todo[-1 - i]
        let (spent, n) = @_took[@_by[key]]
        ready += spent / n
        return false if 2.0 * ready > @_first[key]
      }
      true
    }

    # Seconds into the song at mark `name`.
    _mark(name) {
      let has = @_marks.empty() ? "none" : @_marks.keys().join(", ")
      @_marks.get(name, nil) ?? __raise("ValueError", "Kauai: the song has no mark {name} (it has {has})")
    }

    # Seconds into the song at `p`: seconds, or a String of a mark's name, of
    # seconds (83.5), or of minutes and seconds (1:23.5).
    _point(p) {
      return p * 1.0 if p !is String
      return @_mark(p) if !p.empty() && !"0123456789".contains(p.slice(0, 1).to_string())
      let (m, s) = halves(p, ":") ?? (nil, p)
      let secs = seconds_of(s)
      let whole = (halves(s, ".") ?? (s, ""))[0]
      if secs == nil || (m != nil && !(all_digits(m) && whole.size() == 2 && to_long(whole) < 60)) {
        __raise("ValueError", "Kauai: a point in a song is seconds (83.5), minutes and seconds (1:23.5) or a mark's name, not '{p}'")
      }
      (m == nil ? 0 : to_long(m) * 60) + secs
    }

    # Play from the top (again, if it is playing), after building what is
    # left; `early`, once what is left can be built ahead of the song, which
    # prepare() then builds as it plays: the song waits where a sound is not
    # built yet. Where the mixer cannot wait (the browser), it builds what is
    # left first all the same.
    play(early: Bool = false) {
      if @_schedule == nil {
        @_schedule = (early ? @_open() : nil) ?? _Audio.schedule_new(@_list(@_plays, nil), @_length, @_loop_at)
        _Audio.schedule_volume(@_schedule, @_volume)
        _Audio.schedule_room(@_schedule, @_room)
      }
      while !@_todo.empty() && !(early && @_ahead()) {
        @prepare()
      }
      _Audio.schedule_play(@_schedule)
      @_started = @_now()
    }

    # The song once through as play() sounds it, until its last sound and its
    # room's echoes end: mono samples, 44100 a second, from the mixer that
    # plays it, with no device. A song that loops is heard once. From `from`
    # until `to` (each a point, as seconds() reads one), it is that part of
    # the song, heard from LEAD seconds before `from` (rising from silence
    # over them) until what starts before `to` ends: of the external sounds,
    # only those that start before `to` and play notes LEAD seconds before
    # `from` or later are built.
    render(from: String | Long | Float | Nil = nil, to: String | Long | Float | Nil = nil) {
      let part = @_part("render", from, to)
      let (first, last, start, plays) = (part.first, part.last, part.start, part.plays)
      let list = @_list(plays, last)
      let used = Set()
      for p in plays {
        used.add(p.sound)
      }
      let sounds = []
      for key in used {
        let s = @_sounds[key]
        sounds.push(s._id)
        sounds.push(s.data())
      }
      let length = last == nil ? @_length : Math.min(last, @_length)
      let out = _Audio.schedule_render(list, sounds, length, @_room, @_volume, start)
      # The lead rises as half a cosine, to the level `from` is heard at.
      let rise = Math.round((first - start) * RATE)
      for i in 0..Math.min(rise, out.size()) {
        out[i] = out[i] * (0.5 - 0.5 * Math.cos(Math.pi * i / rise))
      }
      out
    }

    stop() {
      _Audio.schedule_stop(@_schedule) if @_schedule != nil
      @_started = nil
    }

    # The level the whole song plays at, 0.0..1.0, from its next note on.
    volume(v: Long | Float) {
      @_volume = v * 1.0
      _Audio.schedule_volume(@_schedule, v) if @_schedule != nil
    }

    # Whether it is playing: from play() until stop(), or its end.
    playing() {
      @_started != nil && (@_loop_at >= 0 || @_played() < @_length)
    }

    # Whether it has played as far as `mark NAME`.
    reached(name: String) {
      let t = @_mark(name)
      @_started != nil && @_played() >= t
    }

    # Seconds into the song since play(), less those it waited for a sound.
    _played() {
      @_now() - @_started - _Audio.schedule_held(@_schedule)
    }

    # Where each `mark NAME` is: {NAME: seconds into the song}, in its order.
    marks() {
      {...@_marks}
    }

    # Seconds into the song at `at`, a point as render() takes one.
    seconds(at: String | Long | Float) {
      @_point(at)
    }

    # The part from point `from` until `to` (nil for the top or the end) as
    # method `what` takes it: {first, last} in seconds, `start` LEAD seconds
    # before `first` (or the top), and `plays`, the external sounds that
    # start before `last` and play notes from `start` on.
    _part(what, from, to) {
      let first = from == nil ? 0.0 : @_point(from)
      let last = to == nil ? nil : @_point(to)
      if first < 0 || first >= @_length {
        __raise("ValueError", "Kauai: {what}(from:) is at {first} seconds, outside the song's 0 to {@_length}")
      }
      if last != nil && last <= first {
        __raise("ValueError", "Kauai: {what}(to:) is at {last} seconds, not after from: at {first}")
      }
      let start = Math.max(0.0, first - LEAD)
      {
        first: first,
        last: last,
        start: start,
        plays: @_plays.filter(|p| (last == nil || p.at < last) && p.until >= start),
      }
    }

    # The external sounds render(from:, to:) builds, in the order they start:
    # where each starts and its notes end, the voice or drum that plays it,
    # its instrument's key in `voices:` or `drums:`, and what the instrument
    # is handed for it.
    external_sounds(from: String | Long | Float | Nil = nil, to: String | Long | Float | Nil = nil) {
      @_part("external_sounds", from, to).plays.sorted_by(|p| p.at).map(|p| {
        at: p.at,
        until: p.until,
        by: p.by,
        external: p.external,
        args: copied(p.args),
      })
    }

    # Seconds once through.
    length() {
      @_length
    }

    # What its `about` says: {title, composer, ...}, the items it writes.
    about() {
      {...@_about}
    }

    # What it plays, in order: {at, len, by, pitch, vol}, in seconds from its
    # start; `by` names the voice or the drum, and a drum's pitch is nil.
    events() {
      @_events.map(|e| ({at: e.at, len: e.len, by: e.by, pitch: e.pitch, vol: e.vol}))
    }

    drop() {
      _Audio.schedule_free(@_schedule) if @_schedule != nil
    }
  }

  # `name` beside the file `from`; inside a Dir the separator is always `/`.
  fn beside(from, name, in_dir) {
    let dir = from == nil ? "" : FS.dirname(from)
    cond {
      dir == "" => name,
      in_dir => "{dir}/{name}",
      _ => FS.join(dir, name),
    }
  }
  # Song file `path`'s text, and a reader of the files it uses, relative to
  # it: from the disk, or from `dir` (a `Dir.embedded`, or anything with
  # `exists(name)` and `read(name)`).
  fn song_file(path, dir) {
    let exists = dir == nil ? |n| FS.exists(n) : |n| dir.exists(n)
    let read = dir == nil ? |n| FS.read(n) : |n| dir.read(n)
    __raise("IOError", "Kauai: no song file {path}") if !exists(path)
    let load_used = fn (name, from, line) {
      let file = beside(from, name, dir != nil)
      fail(line, "no file {file} to use") if !exists(file)
      (read(file), file)
    }
    (read(path), load_used)
  }

  # A song given as text uses no other files.
  fn no_files(name, from, line) {
    fail(line, "a song given as text uses no files: use '{name}'")
  }

  # What the band plays as expand() `x` gives it, bar by bar in the order the
  # song plays: the voices that play its grooves, echoes and harmonies, and its
  # drums, each in the band's order and named where it plays at all; and for
  # each bar its length, each of those voices' notes and the drums' hits, from
  # the bar's start in units. A note can last into the bars after it.
  fn played_of(x) {
    let notes = {}
    for row in x.rows {
      continue if row.voice.name == nil || row.notes.empty()
      notes.get_or_put(row.voice.name, || []).extend(row.notes)
    }
    let voices = x.voices.keys().filter(|vn| notes.has(vn))
    let drums = x.drums.keys().filter(|dn| x.hits.any(|h| h.name == dn))
    # The `at`s in bar order: each bar takes the ones before its end.
    let by_bar = fn (sorted, keep) {
      mut k = 0
      x.bars.map(fn (b) {
        let from = k
        while k < sorted.size() && sorted[k].at < b.at + b.len {
          k += 1
        }
        sorted.slice(from, k).map(|it| keep(it, it.at - b.at))
      })
    }
    let voice_bars = voices.map(|vn| by_bar(notes[vn].sorted_by(|n| n.at), |n, at| ({
      at: at,
      len: n.len,
      pitches: [...n.pitches],
      marks: [...n.get("marks", [])],
    })))
    let hit_bars = by_bar(x.hits.sorted_by(|h| h.at), |h, at| ({name: h.name, at: at, marks: [...h.marks]}))
    {
      voices: voices,
      drums: drums,
      bars: iota(x.bars.size()).map(|i| ({
        len: x.bars[i].len,
        voices: iota(voices.size()).map(|v| ({name: voices[v], notes: voice_bars[v][i]})),
        drums: hit_bars[i],
      })),
    }
  }

  # The song's score, checked as a song that plays is.
  fn score(src, file, load) {
    let s = parse(src, file, load)
    score_of(s, played_of(expand(s, file)))
  }
  {
    load: fn (path: String, voices = nil, drums = nil, dir = nil) {
      let (src, load_used) = song_file(path, dir)
      Song.new(expand(parse(src, path, load_used), path, voices, drums), voices, drums)
    },
    new: fn (text: String, voices = nil, drums = nil) {
      Song.new(expand(parse(text, nil, no_files), nil, voices, drums), voices, drums)
    },
    parse: fn (text: String) {
      score(text, nil, no_files)
    },
    parse_file: fn (path: String, dir = nil) {
      let (src, load_used) = song_file(path, dir)
      score(src, path, load_used)
    },
    # An entry of `voices:` (or `drums:`, with `drum: true`) as Kauai reads it:
    # {call, make, hears, knobs, weigh}. `name` is what a ValueError calls it.
    instrument: fn (entry, drum: Bool = false, name: String = "instrument") {
      instrument_of(drum ? DRUM_KIND : VOICE_KIND, name, entry)
    },
  }
}

let Kauai = _kauai_module()
)=culpre=";

inline constexpr const char* SYNTH_MODULE_SOURCE = R"=culpre=(# Synth: sound written as an expression. `sine(440.0) * 0.2` is a Part, a
# value that holds no graph; render walks it and builds an Audio.Graph, the
# part a value names made once however often the expression uses it
# (sharing by identity: `let a = sine(440.0)` then `a + a` is one wave, where
# `sine(440.0) + sine(440.0)` is two). Parts and envelopes never change once
# made, so an expression can be kept, reused and combined freely.
#
# A value a part reads (a wave's freq, a filter's cutoff, a gain) takes a
# number, a Part or an Envelope. A number is the value; an Envelope is its
# schedule from the start; a Part is the signal itself, not something added
# to a base (Audio.Graph's connect adds; here the base is kept at the value's
# floor and the floor taken off again, so the value is the signal). Ranges
# are checked by the graph when render builds it, where the rate is known,
# under the graph's own rules.
#
# A value or an input given as an Array is one channel per item: the part
# made of it is a bundle, a part per channel, and what is one channel goes
# to each (`sine([440.0, 660.0]) * 0.2` is two sines, each at 0.2). A bundle
# is a SynthPart of kind "bundle" whose inputs are its channels, so every
# method takes it as it takes a part, and `.mix()` adds the channels to one.
let _synth_module = fn () {
  # The number every Part is made with: render keeps the parts it built by
  # it, which is what makes a value used twice one part.
  let made = {mut count: 0}

  # How many straight lines a curved segment of an envelope becomes: the
  # graph's values only ramp in straight lines, and 20 keep a curve of -4
  # within 0.5% of its full swing.
  let CURVE_STEPS = 20

  let is_number = |v| v is Long | Float
  # Neither NaN (unequal to itself) nor an infinity.
  let finite = |x| x == x && x != Math.inf && x != -Math.inf
  let nonnegative = |x| finite(x) && x >= 0

  # The level a curved segment from `l0` to `l1` has at `x` of its way
  # (0..1): a straight line (curve 0), bent by a curve number c (fast first
  # below 0, slow first above), or "exp".
  let shape = fn (l0, l1, c, x) {
    if c == "exp" {
      l0 * Math.exp(Math.log(l1 / l0) * x)
    } else if c == 0 {
      l0 + (l1 - l0) * x
    } else {
      l0 + (l1 - l0) * (1.0 - Math.exp(c * x)) / (1.0 - Math.exp(c))
    }
  }

  # A shape a value follows in time: `levels` joined by segments `times`
  # long, each straight (curve 0), bent by a curve number, or
  # exponential ("exp"). It holds its last level after the end. A gated one
  # (adsr's) holds its last level until it is let go, then falls to 0 over
  # `release` seconds, `hold(secs)` from its start.
  class SynthEnvelope {
    new(levels: Array, times: Array, curve, who: String, release: Long | Float | Nil = nil) {
      let fail = |msg| __raise("ValueError", "Synth.{who}: {msg}")
      fail("an envelope has two levels or more") if levels.size() < 2
      fail("times has one fewer than levels ({levels.size() - 1})") if times.size() != levels.size() - 1
      fail("a level is a finite number") unless levels.all(|l| is_number(l) && finite(l))
      fail("a time is a finite number of seconds, 0 or more") unless times.all(|t| is_number(t) && nonnegative(t))
      let curves = curve is Array ? curve : repeat(times.size(), curve)
      fail("curve has one per segment ({times.size()})") if curves.size() != times.size()
      for i, c in curves.enumerate() {
        if c == "exp" {
          let (a, b) = (levels[i], levels[i + 1])
          fail("an \"exp\" segment runs between levels of one sign, never 0") if a == 0 || b == 0 || (a < 0) != (b < 0)
        } else {
          fail("a curve is a finite number or \"exp\"") if !is_number(c) || !finite(c)
        }
      }
      fail("release is a finite number of seconds, 0 or more") if release != nil && !nonnegative(release)
      @levels = levels
      @times = times
      @curves = curves
      @release = release
    }
    duration() {
      @times.sum()
    }
    # The same gated envelope let go `secs` seconds from its start: the
    # shape up to then (sustaining once it is done), and from the level it
    # has there, its release to 0.
    hold(secs: Long | Float) {
      __raise("ValueError", "Synth.hold: only a gated envelope (adsr) is held") if @release == nil
      __raise("ValueError", "Synth.hold: secs is a finite number, 0 or more") if !nonnegative(secs)
      let (levels, times, curves) = ([@levels[0]], [], [])
      mut start = 0.0
      for i, len in @times.enumerate() {
        if start >= secs && len > 0 {
          break
        }
        let upto = Math.min(start + len, secs)
        levels.push(upto == start + len ? @levels[i + 1] : self.at(upto))
        times.push(upto - start)
        curves.push(@curves[i])
        start = upto
      }
      if start < secs {
        levels.push(levels[levels.size() - 1])
        times.push(secs - start)
        curves.push(0.0)
      }
      levels.push(0.0)
      times.push(@release)
      curves.push(@curves[@curves.size() - 1])
      SynthEnvelope(levels, times, curves, "hold")
    }
    # The level at `t` seconds from the start.
    at(t: Long | Float) {
      mut start = 0.0
      for i, len in @times.enumerate() {
        if t < start + len {
          return t <= start ? @levels[i] : shape(@levels[i], @levels[i + 1], @curves[i], (t - start) / len)
        }
        start += len
      }
      t < 0 ? @levels[0] : @levels[@levels.size() - 1]
    }
  }

  # A value as a part reads it: a number, a Part (the signal) or an Envelope.
  let to_part = fn (v) { v is SynthPart ? v : SynthPart("level", [], {level: v}, {}) }
  fn negate(v) {
    match v {
      Array => channels_of(v).map(negate),
      SynthEnvelope => SynthEnvelope(v.levels.map(|l| -l), v.times, v.curves, "-", v.release),
      _ => -v,
    }
  }

  # The channels a value or an input gives: an Array's items or a bundle's
  # parts; nil for one channel.
  fn channels_of(v) {
    if v is Array {
      __raise("ValueError", "Synth: an Array of channels has one or more") if v.empty()
      if !v.all(|c| (c is Long | Float | SynthEnvelope) || (c is SynthPart && c.kind != "bundle")) {
        __raise("TypeError", "type error: a channel is one Long | Float | SynthPart | SynthEnvelope")
      }
      return v
    }
    v is SynthPart && v.kind == "bundle" ? v.inputs : nil
  }

  # A part of `kind` made of `inputs` and `values`, the one place a part is
  # made from what a caller gave: an Array or a bundle among them makes a
  # part per channel, as a bundle (multichannel expansion), what is one
  # channel going to each; one channel is the part itself. Channels of two
  # counts at once are a ValueError.
  fn node(kind, inputs, values, options) {
    mut count = nil
    for v in inputs + values.values().collect() {
      let channels = channels_of(v)
      if channels != nil {
        __raise("ValueError", "Synth: {count} channels and {channels.size()} at once") if count != nil && count != channels.size()
        count = channels.size()
      }
    }
    if count == nil {
      return SynthPart(kind, inputs.map(to_part), values, options)
    }
    # Channel `i` of what the loop above has checked.
    let pick = |v, i| v is Array ? v[i] : (v is SynthPart && v.kind == "bundle" ? v.inputs[i] : v)
    # A part that draws numbers draws its own on each channel, from its seed
    # plus the channel's number.
    let drawing = |i| options.has("seed") ? {...options, seed: (options.seed + i) % 2147483648} : options
    let parts = iota(count).map(|i| node(kind, inputs.map(|v| pick(v, i)), values.keys().map(|k| (k, pick(values[k], i))).to_object(), drawing(i)))
    count == 1 ? parts[0] : SynthPart("bundle", parts, {}, {})
  }

  # A sound or a signal, as a node of an expression: `kind` names the graph
  # part it becomes, `inputs` the parts it takes in, `values` what it reads
  # (each a number, a Part or an Envelope) and `options` its fixed settings.
  class SynthPart {
    new(kind: String, inputs: Array, values: Object, options: Object) {
      made.count += 1
      @id = made.count
      @kind = kind
      @inputs = inputs
      @values = values
      @options = options
    }
    filter(kind: String, cutoff: Long | Float | SynthPart | SynthEnvelope | Array = 1000.0, q: Long | Float = 0.707) {
      node("filter", [self], {cutoff: cutoff}, {kind: kind, q: q})
    }
    lowpass(cutoff: Long | Float | SynthPart | SynthEnvelope | Array = 1000.0, q: Long | Float = 0.707) {
      self.filter("low", cutoff, q)
    }
    highpass(cutoff: Long | Float | SynthPart | SynthEnvelope | Array = 1000.0, q: Long | Float = 0.707) {
      self.filter("high", cutoff, q)
    }
    bandpass(cutoff: Long | Float | SynthPart | SynthEnvelope | Array = 1000.0, q: Long | Float = 0.707) {
      self.filter("band", cutoff, q)
    }
    delay(time: Long | Float | SynthPart | SynthEnvelope | Array = 0.0) {
      node("delay", [self], {time: time}, {})
    }
    pluck(freq: Long | Float | SynthPart | SynthEnvelope | Array = 220.0, ring: Long | Float | SynthPart | SynthEnvelope | Array = 2.0) {
      node("pluck", [self], {freq: freq, ring: ring}, {})
    }
    resonate(modes: Array, dry: Long | Float = 0.0) {
      node("resonate", [self], {}, {modes: modes, dry: dry})
    }
    drive(gain: Long | Float | SynthPart | SynthEnvelope | Array = 1.0) {
      node("drive", [self], {gain: gain}, {})
    }
    limit(ceiling: Long | Float = 0.9, release: Long | Float = 0.2) {
      node("limit", [self], {}, {ceiling: ceiling, release: release})
    }
    # Followed by a one-pole filter that closes 60 dB of a step in `time`
    # seconds, from where it starts; 0 is the part itself.
    lag(time: Long | Float) {
      time == 0 ? self : node("lag", [self], {}, {time: time})
    }
    # Impulses as an envelope that rises in about `attack` seconds and falls
    # 60 dB in `decay`: two decays, the shorter taken off.
    decay2(attack: Long | Float, decay: Long | Float) {
      __raise("ValueError", "Synth.decay2: attack is above 0 and below decay") unless attack > 0 && attack < decay
      self._decay(decay) - self._decay(attack)
    }
    _decay(time) {
      node("decay", [self], {}, {time: time})
    }
    # The signal held within `lo`..`hi`.
    clip(lo: Long | Float, hi: Long | Float) {
      node("math", [self], {}, {kind: "clip", lo: lo, hi: hi})
    }
    # The signal's size.
    abs() {
      node("math", [self], {}, {kind: "abs", lo: -1.0, hi: 1.0})
    }
    # A signal from -1 to 1 taken to `lo`..`hi` along a curve that multiplies
    # by the same at each step, as a pitch or a cutoff is heard:
    # lo * (hi / lo) ^ ((x + 1) / 2).
    exprange(lo: Long | Float, hi: Long | Float) {
      if !finite(lo) || !finite(hi) || lo == 0 || hi == 0 || (lo < 0) != (hi < 0) {
        __raise("ValueError", "Synth.exprange: lo and hi are finite numbers of one sign, never 0")
      }
      node("math", [self.range(0.0, Math.log(hi / lo))], {}, {kind: "exp", lo: -1.0, hi: 1.0}) * lo
    }
    __add__(other: Long | Float | SynthPart | SynthEnvelope | Array) {
      node("mix", [self, other], {}, {})
    }
    __sub__(other: Long | Float | SynthPart | SynthEnvelope | Array) {
      self + negate(other)
    }
    __mul__(other: Long | Float | SynthPart | SynthEnvelope | Array) {
      node("gain", [self], {gain: other}, {})
    }
    __div__(other: Long | Float) {
      __raise("ValueError", "Synth: a part divided by 0") if other == 0
      self * (1.0 / other)
    }
    __neg__() {
      self * -1.0
    }
    # A signal from -1 to 1 taken to `lo`..`hi` in a straight line.
    range(lo: Long | Float, hi: Long | Float) {
      __raise("ValueError", "Synth.range: lo and hi are finite numbers") if !finite(lo) || !finite(hi)
      self * ((hi - lo) / 2.0) + (hi + lo) / 2.0
    }
    # A bundle's channels added to one part; a part is one already.
    mix() {
      @kind == "bundle" ? SynthPart("mix", @inputs, {}, {}) : self
    }
  }

  let wave = fn (
    kind: String = "sine",
    freq: Long | Float | SynthPart | SynthEnvelope | Array = 440.0,
    feedback: Long | Float | SynthPart | SynthEnvelope | Array = 0.0,
    phase: Long | Float | SynthPart | SynthEnvelope | Array = 0.0,
  ) {
    node("wave", [], {freq: freq, feedback: feedback, phase: phase}, {kind: kind})
  }
  let sine = fn (
    freq: Long | Float | SynthPart | SynthEnvelope | Array = 440.0,
    phase: Long | Float | SynthPart | SynthEnvelope | Array = 0.0,
    feedback: Long | Float | SynthPart | SynthEnvelope | Array = 0.0,
  ) {
    wave("sine", freq, feedback, phase)
  }
  let saw = fn (freq: Long | Float | SynthPart | SynthEnvelope | Array = 440.0, phase: Long | Float | SynthPart | SynthEnvelope | Array = 0.0) {
    wave("saw", freq, 0.0, phase)
  }
  let square = fn (freq: Long | Float | SynthPart | SynthEnvelope | Array = 440.0, phase: Long | Float | SynthPart | SynthEnvelope | Array = 0.0) {
    wave("square", freq, 0.0, phase)
  }
  let triangle = fn (freq: Long | Float | SynthPart | SynthEnvelope | Array = 440.0, phase: Long | Float | SynthPart | SynthEnvelope | Array = 0.0) {
    wave("triangle", freq, 0.0, phase)
  }
  let noise = fn (seed: Long = 1) {
    SynthPart("noise", [], {}, {seed: seed})
  }
  # 1 at the first sample of each period of `freq` Hz, 0 elsewhere:
  # `phase`, 0 to below 1, is the part of a period gone by at the
  # start, 0 firing at once. A freq of 0 fires once, at the start, with a
  # phase of 0, and never with another.
  let impulse = fn (freq: Long | Float | SynthPart | SynthEnvelope | Array = 1.0, phase: Long | Float = 0.0) {
    node("impulse", [], {freq: freq}, {phase: phase})
  }
  # Impulses from 0 to 1 at random samples, `density` a second on average.
  let dust = fn (density: Long | Float | SynthPart | SynthEnvelope | Array, seed: Long = 1) {
    node("dust", [], {density: density}, {seed: seed})
  }
  # A new number from -1 to 1 `freq` times a second, held ("step"), in a
  # straight line to the next ("linear") or along a curve ("cubic").
  let lfnoise = fn (freq: Long | Float | SynthPart | SynthEnvelope | Array, kind: String = "step", seed: Long = 1) {
    node("lfnoise", [], {freq: freq}, {kind: kind, seed: seed})
  }
  # A pulse wave from -1 to 1, high for `width` of each period, its jumps
  # smoothed as a square's: a saw `width` of a period behind, less a saw,
  # plus the step the width makes (2 width - 1). A width of 0.5 is a square.
  let pulse = fn (freq: Long | Float | SynthPart | SynthEnvelope | Array = 440.0, width: Long | Float | SynthPart | Array = 0.5) {
    let numbers = width is Array ? width.filter(is_number) : (is_number(width) ? [width] : [])
    __raise("ValueError", "Synth.pulse: a width is above 0 and below 1") unless numbers.all(|w| w > 0 && w < 1)
    # `v` times `k` plus `c`, channel by channel (nothing added for a `c` of 0).
    let affine = |w, k, c| c == 0.0 ? w * k : w * k + c
    let scaled = |v, k, c| v is Array ? v.map(|w| affine(w, k, c)) : affine(v, k, c)
    let saws = [saw(freq, phase: scaled(width, -2.0 * Math.pi, 0.0)), -saw(freq)]
    let step = scaled(width, 2.0, -1.0)
    node("mix", step == 0.0 ? saws : saws + [step], {}, {})
  }
  let mix = fn (parts: Array) {
    __raise("ValueError", "Synth.mix: nothing to mix") if parts.empty()
    __raise("TypeError", "type error: Synth.mix takes an Array of Long | Float | SynthPart | SynthEnvelope | Array") unless parts.all(|p| p is Long | Float | SynthPart | SynthEnvelope | Array)
    node("mix", parts, {}, {})
  }

  # A loop: `f` gets x, a stand-in for what the expression it returns
  # answers, and the expression hears x through a delay (x.delay(t) is that
  # answer t seconds back). Each feedback has its own x, so loops nest.
  let feedback_loop = fn (f: Function) {
    let x = SynthPart("loop", [], {}, {})
    let body = f(x)
    __raise("TypeError", "type error: Synth.feedback's function returns a SynthPart") unless body is SynthPart
    __raise("ValueError", "Synth.feedback: a loop is one channel; mix the channels first, .mix()") if body.kind == "bundle"
    if undelayed(body, x) {
      __raise("ValueError", "Synth.feedback: the expression hears x with no delay between (x.delay(t) hears it t seconds back)")
    }
    SynthPart("feedback", [body], {}, {loop: x})
  }

  let env = fn (levels: Array, times: Array, curve: Long | Float | String | Array = 0.0) {
    SynthEnvelope(levels, times, curve, "env")
  }
  let line = fn (lo: Long | Float, hi: Long | Float, dur: Long | Float, curve: Long | Float = 0.0) {
    __raise("ValueError", "Synth.line: dur is more than 0 seconds") if !(dur > 0)
    SynthEnvelope([lo, hi], [dur], curve, "line")
  }
  let xline = fn (lo: Long | Float, hi: Long | Float, dur: Long | Float) {
    __raise("ValueError", "Synth.xline: dur is more than 0 seconds") if !(dur > 0)
    SynthEnvelope([lo, hi], [dur], "exp", "xline")
  }

  # An attack to `peak`, a decay to `sustain` of it, held there until the
  # envelope is let go, then a release to 0: gated, so it plays as
  # `.hold(secs)`, which says when.
  let adsr = fn (
    attack: Long | Float,
    decay: Long | Float,
    sustain: Long | Float,
    release: Long | Float,
    peak: Long | Float = 1.0,
    curve: Long | Float = -4.0,
  ) {
    __raise("ValueError", "Synth.adsr: sustain is a finite number, 0 or more") if !nonnegative(sustain)
    SynthEnvelope([0.0, peak, sustain * peak], [attack, decay], curve, "adsr", release)
  }
  # An attack to `peak`, then a release to 0: a struck sound's envelope.
  let perc = fn (attack: Long | Float, release: Long | Float, peak: Long | Float = 1.0, curve: Long | Float = -4.0) {
    SynthEnvelope([0.0, peak, 0.0], [attack, release], curve, "perc")
  }

  # A value named `name`, `init` until a handle play gave sets it (clamped to
  # lo..hi when they are given). Two controls of one name in an expression are
  # one value.
  let control = fn (name: String, init: Long | Float, lo: Long | Float | Nil = nil, hi: Long | Float | Nil = nil) {
    let fail = |msg| __raise("ValueError", "Synth.control: {msg}")
    fail("a name is not empty") if name.empty()
    fail("init is a finite number") if !finite(init)
    fail("lo and hi are finite, lo at most hi") if (lo != nil && !finite(lo)) || (hi != nil && !finite(hi)) || (lo != nil && hi != nil && lo > hi)
    fail("init is within lo..hi") if (lo != nil && init < lo) || (hi != nil && init > hi)
    SynthPart("control", [], {}, {name: name, init: init, lo: lo, hi: hi})
  }

  # Whether `p` hears `x` with no delay between: through its inputs (but a
  # delay's, which answer from before) and the parts its values read (a
  # delay's time too, which it needs to answer).
  fn undelayed(p, x) {
    let known = {}
    fn walk(q) {
      known[q.id] ??= q.id == x.id || (q.kind == "delay" ? [] : q.inputs).any(walk) || q.values.values().any(|v| v is SynthPart && walk(v))
    }
    walk(p)
  }

  # The loops `p` hears whose feedback is not within it, by their x's
  # number: a feedback closes its own loop only for what is inside it.
  fn open_loops(p) {
    let known = {}
    fn walk(q) {
      known[q.id] ??= match q.kind {
        "loop" => [q.id],
        "feedback" => walk(q.inputs[0]).filter(|id| id != q.options.loop.id),
        _ => (q.inputs + q.values.values().filter(|v| v is SynthPart).collect()).iter().flat_map(walk).distinct().collect(),
      }
    }
    walk(p)
  }

  # ---- for a part or for samples

  # The functions below take a part or samples: a part takes the method of
  # their name, samples (an Array of numbers, which Audio's function checks)
  # Audio's function, so with the name in scope `samples.lowpass(800.0)` reads
  # as `part.lowpass(800.0)` does. A part's rate is render's or play's.
  fn part_rate(x, rate, who) {
    __raise("ValueError", "Synth.{who}: a part's rate is the one render or play gives") if x is SynthPart && rate != nil
    rate ?? 44100
  }
  let lowpass = fn (x: SynthPart | Array, cutoff = 1000.0, q: Long | Float = 0.707, rate: Long | Float | Nil = nil) {
    let r = part_rate(x, rate, "lowpass")
    x is SynthPart ? x.lowpass(cutoff, q) : Audio.filter(x, "low", cutoff, q, r)
  }
  let highpass = fn (x: SynthPart | Array, cutoff = 1000.0, q: Long | Float = 0.707, rate: Long | Float | Nil = nil) {
    let r = part_rate(x, rate, "highpass")
    x is SynthPart ? x.highpass(cutoff, q) : Audio.filter(x, "high", cutoff, q, r)
  }
  let bandpass = fn (x: SynthPart | Array, cutoff = 1000.0, q: Long | Float = 0.707, rate: Long | Float | Nil = nil) {
    let r = part_rate(x, rate, "bandpass")
    x is SynthPart ? x.bandpass(cutoff, q) : Audio.filter(x, "band", cutoff, q, r)
  }
  let resonate = fn (x: SynthPart | Array, modes: Array, dry: Long | Float = 0.0, rate: Long | Float | Nil = nil) {
    let r = part_rate(x, rate, "resonate")
    x is SynthPart ? x.resonate(modes, dry) : Audio.resonate(x, modes, dry, r)
  }
  # Samples times an envelope of (at, value) points, as Audio.envelope; a
  # part times its gain, as `*`.
  let envelope = fn (x: SynthPart | Array, gain, rate: Long | Float | Nil = nil) {
    let r = part_rate(x, rate, "envelope")
    x is SynthPart ? x * gain : Audio.envelope(x, gain, r)
  }

  let hz = fn (note: Long | Float) {
    __raise("ValueError", "Synth.hz: note is a finite number") if !finite(note)
    440.0 * Math.exp(Math.log(2.0) * (note - 69) / 12.0)
  }
  let db = fn (x: Long | Float) {
    __raise("ValueError", "Synth.db: x is a finite number") if !finite(x)
    Math.exp(Math.log(10.0) * x / 20.0)
  }

  # ---- from an expression to an Audio.Graph

  # One segment of an envelope on a graph value, from `l0` at `t` to `l1`
  # `len` later: a ramp when straight, CURVE_STEPS ramps when curved (a
  # segment 0 long jumps, as a ramp to the time of the change before does).
  let segment = fn (value, t, l0, l1, len, c) {
    if c == 0 {
      value.ramp(t + len, l1)
    } else {
      for j in iota(CURVE_STEPS) {
        let x = (j + 1) / (CURVE_STEPS + 0.0)
        value.ramp(t + len * x, shape(l0, l1, c, x))
      }
    }
  }

  # An envelope on a graph value from `t0`, a gated one up to its sustain.
  let run = fn (value, e, t0) {
    value.set(t0, e.levels[0])
    mut t = t0
    for i, len in e.times.enumerate() {
      segment(value, t, e.levels[i], e.levels[i + 1], len, e.curves[i])
      t += len
    }
  }

  # An envelope on a graph value from the start; a gated one has no time to
  # be let go here.
  let schedule = fn (value, e) {
    if e.release != nil {
      __raise("ValueError", "Synth: an adsr envelope is held until it is let go: give it the time, adsr(...).hold(secs)")
    }
    run(value, e, 0.0)
  }

  # A graph being built: the graph, the parts built so far by the number
  # of the SynthPart they stand for, the controls by name, the one constant
  # source every level is a gain of (made when the first level is), and the
  # voice of a Voices being built, if one is.
  fn build_of(g) {
    {g: g, parts: {}, controls: {}, mut one: nil, mut voice: nil}
  }

  # What a value is made with in the graph: a number as itself, an envelope
  # from its first level, a signal from the value's floor (taken off again
  # in feed).
  let base_of = |spec, floor| spec is SynthPart ? floor : (spec is SynthEnvelope ? spec.levels[0] : spec)

  # Whether an envelope on `owner` starts at each of a voice's notes: the
  # owner is a part the voice's builder made (numbered from the voice's
  # freq on, the builder having made nothing before it). An envelope on a
  # part every voice shares runs from the start, as elsewhere.
  fn owned(b, owner) {
    b.voice != nil && owner != nil && owner.id >= b.voice.pitch_id
  }

  # An envelope on a graph value of `owner`: kept for a voice's notes to
  # start, or run from the start.
  fn envelope_on(b, owner, value, e) {
    if owned(b, owner) {
      b.voice.triggers.push({value: value, envelope: e})
    } else {
      schedule(value, e)
    }
  }

  # A constant `v`: a square wave at 0 Hz answers 1 at every sample.
  fn level(b, v) {
    b.one ??= b.g.wave("square", freq: 0.0)
    b.g.gain(b.one, v)
  }

  # A number, an envelope or a part (of `owner`), as a part of the graph that
  # answers it.
  fn signal(b, v, owner) {
    match v {
      SynthPart => part(b, v),
      SynthEnvelope => {
        let p = level(b, v.levels[0])
        envelope_on(b, owner, p.gain, v)
        p
      },
      _ => level(b, v),
    }
  }

  # A graph value of `owner` read as `spec` says, its base already base_of's.
  fn feed(b, owner, value, spec, floor) {
    if spec is SynthEnvelope {
      envelope_on(b, owner, value, spec)
    } else if spec is SynthPart {
      part(b, spec).connect(value)
      level(b, -floor).connect(value) if floor != 0.0
    }
  }

  # The graph part a SynthPart stands for, built once.
  fn part(b, p) {
    b.parts[p.id] ??= lower(b, p)
  }

  # A control is a level whose gain a handle sets; the controls of one name
  # are one.
  fn control_part(b, o) {
    let known = b.controls.get(o.name, nil)
    if known == nil {
      let p = level(b, o.init)
      b.controls[o.name] = {part: p, value: p.gain, init: o.init, lo: o.lo, hi: o.hi}
      return p
    }
    if (known.init, known.lo, known.hi) != (o.init, o.lo, o.hi) {
      __raise("ValueError", "Synth.control: `{o.name}` is made two ways, ({known.init}, {known.lo}, {known.hi}) and ({o.init}, {o.lo}, {o.hi})")
    }
    known.part
  }

  # A voice's freq: a level each note sets, heard only in the voice whose
  # builder was given it. It starts at the voice's first note made before
  # the graph is built (440 Hz with none), so what follows it (a lag) starts
  # there too.
  fn pitch_part(b, p) {
    if b.voice == nil || b.voice.pitch_id != p.id {
      __raise("ValueError", "Synth.Voices: a voice's freq is heard only in what its builder returns")
    }
    let q = level(b, b.voice.first)
    b.voice.pitch = q.gain
    q
  }

  # A feedback's x is a mix its expression's answer is connected back into,
  # made before the expression so the expression can read it.
  fn close_loop(b, p) {
    let x = b.g.mix([])
    b.parts[p.options.loop.id] = x
    let answer = part(b, p.inputs[0])
    answer.connect(x)
    answer
  }

  fn lower(b, p) {
    return close_loop(b, p) if p.kind == "feedback"
    let (g, v, o) = (b.g, p.values, p.options)
    let input = p.inputs.size() == 1 ? part(b, p.inputs[0]) : nil
    match p.kind {
      "wave" => {
        let w = g.wave(o.kind, freq: base_of(v.freq, 0.0), feedback: base_of(v.feedback, 0.0), phase: base_of(v.phase, 0.0))
        feed(b, p, w.freq, v.freq, 0.0)
        feed(b, p, w.feedback, v.feedback, 0.0)
        feed(b, p, w.phase, v.phase, 0.0)
        w
      },
      "noise" => g.noise(o.seed),
      "level" => signal(b, v.level, p),
      "control" => control_part(b, o),
      "pitch" => pitch_part(b, p),
      # close_loop builds x; graph_of has refused an x outside its loop.
      "loop" => __raise("ValueError", "Synth.feedback: x is heard only in the expression its function returns"),
      "mix" => g.mix(p.inputs.map(|i| part(b, i))),
      "gain" => {
        let q = g.gain(input, base_of(v.gain, 0.0))
        feed(b, p, q.gain, v.gain, 0.0)
        q
      },
      "filter" => {
        # A cutoff is more than 0 Hz: a signal's base is 1 Hz, taken off again.
        let q = g.filter(o.kind, input, cutoff: base_of(v.cutoff, 1.0), q: o.q)
        feed(b, p, q.cutoff, v.cutoff, 1.0)
        q
      },
      "delay" => {
        let q = g.delay(input, base_of(v.time, 0.0))
        feed(b, p, q.time, v.time, 0.0)
        q
      },
      "pluck" => {
        # A string's freq is 20 Hz or more: a signal's base is 20 Hz.
        let q = g.pluck(input, freq: base_of(v.freq, 20.0), ring: base_of(v.ring, 0.0))
        feed(b, p, q.freq, v.freq, 20.0)
        feed(b, p, q.ring, v.ring, 0.0)
        q
      },
      "resonate" => g.resonate(input, o.modes, dry: o.dry),
      "drive" => {
        let q = g.drive(input, base_of(v.gain, 0.0))
        feed(b, p, q.gain, v.gain, 0.0)
        q
      },
      "limit" => g.limit(input, ceiling: o.ceiling, release: o.release),
      "impulse" => {
        let q = g.impulse(base_of(v.freq, 0.0), o.phase)
        feed(b, p, q.freq, v.freq, 0.0)
        q
      },
      "dust" => {
        let q = g.dust(base_of(v.density, 0.0), o.seed)
        feed(b, p, q.density, v.density, 0.0)
        q
      },
      "lfnoise" => {
        let q = g.lfnoise(o.kind, base_of(v.freq, 0.0), o.seed)
        feed(b, p, q.freq, v.freq, 0.0)
        q
      },
      "lag" => g.lag(input, o.time),
      "decay" => g.decay(input, o.time),
      "math" => match o.kind {
        "abs" => g.abs(input),
        "clip" => g.clip(input, o.lo, o.hi),
        _ => g.exp(input),
      },
      _ => __raise("ValueError", "Synth: no graph part for a {p.kind}"),
    }
  }

  # An expression that plays as one sound: one channel, its loops closed.
  let check_playable = fn (expr) {
    if expr is SynthPart && expr.kind == "bundle" {
      __raise("ValueError", "Synth: {expr.inputs.size()} channels play mixed to one, .mix()")
    }
    if expr is SynthPart && !open_loops(expr).empty() {
      __raise("ValueError", "Synth.feedback: x is heard only in the expression its function returns")
    }
  }

  # The graph an expression stands for, its output set: the graph, its
  # controls and its output part.
  fn graph_of(expr, rate) {
    check_playable(expr)
    let g = Audio.Graph(rate)
    let b = build_of(g)
    let out = signal(b, expr, nil)
    g.output(out)
    {g: g, controls: b.controls, out: out}
  }

  # The first `seconds` of `expr`, as samples: what play would play.
  let render = fn (expr: SynthPart | SynthEnvelope | SynthVoices, seconds: Long | Float, rate: Long | Float = 44100) {
    (expr is SynthVoices ? expr._built(rate) : graph_of(expr, rate)).g.render(seconds)
  }

  # ---- when an expression ends

  # The seconds after which `expr` is silent for good, Math.inf when never:
  # a product ends with its first factor to, a sum with its last; an
  # envelope ending at 0 ends there; a delay and a string ring on after their
  # input (the longest delay, the string's 60 dB ring), as a resonance does
  # (60 dB of a mode `width` Hz wide is 6.91 / (pi * width) seconds), a
  # filter's `cutoff / q` wide at its lowest cutoff (one a signal moves adds
  # none: the input's end decides), and a lag and a decay for their time; a
  # size, and a clip that holds 0, end with their input; an impulse and a
  # dust end when their freq or density is 0. A gated envelope ends once it is let go,
  # `held` seconds from its start, and never while it is held.
  fn ending(expr, held = nil) {
    let known = {}
    fn envelope_end(e) {
      if e.release != nil {
        return held == nil ? Math.inf : envelope_end(e.hold(held))
      }
      e.levels[e.levels.size() - 1] == 0 ? e.duration() : Math.inf
    }
    fn value_end(v) {
      match v {
        SynthPart => tail(v),
        SynthEnvelope => envelope_end(v),
        _ => v == 0 ? 0.0 : Math.inf,
      }
    }
    fn longest(v) {
      match v {
        SynthPart => Math.inf,
        SynthEnvelope => v.levels.max(),
        _ => v,
      }
    }
    let ring = |width| 6.91 / (Math.pi * width)
    fn filter_ring(p) {
      let cutoff = p.values.cutoff
      match cutoff {
        SynthPart => 0.0,
        SynthEnvelope => ring(cutoff.levels.min() / p.options.q),
        _ => ring(cutoff / p.options.q),
      }
    }
    fn tail(p) {
      known[p.id] ??= match p.kind {
        "level" => value_end(p.values.level),
        "mix" => p.inputs.map(|i| tail(i)).max(),
        "gain" => Math.min(tail(p.inputs[0]), value_end(p.values.gain)),
        "filter" => tail(p.inputs[0]) + filter_ring(p),
        "drive" | "limit" => tail(p.inputs[0]),
        "delay" => tail(p.inputs[0]) + Math.min(longest(p.values.time), 1.0),
        "pluck" => tail(p.inputs[0]) + longest(p.values.ring),
        "resonate" => tail(p.inputs[0]) + p.options.modes.reduce(0.0, |t, m| Math.max(t, ring(m.width))),
        # An impulse at 0 Hz fires once, at the start; a dust at 0 never.
        "impulse" => value_end(p.values.freq),
        "dust" => value_end(p.values.density),
        "lag" | "decay" => tail(p.inputs[0]) + p.options.time,
        "math" => match p.options.kind {
          "abs" => tail(p.inputs[0]),
          "clip" => p.options.lo <= 0 && p.options.hi >= 0 ? tail(p.inputs[0]) : Math.inf,
          _ => Math.inf,
        },
        _ => Math.inf,
      }
    }
    value_end(expr)
  }

  # ---- playing

  # A sound playing: stop() silences it, set() moves a control, and the
  # graph's clock says where it is.
  class SynthHandle {
    new(graph, controls: Object, ends: Long | Float | Nil) {
      @_graph = graph
      @_controls = controls
      @_ends = ends
      @_stopped = false
    }
    # When it ends (at play's secs, or else on its own), in seconds from its
    # start; nil for never.
    ends() {
      @_ends
    }
    now() {
      @_graph.now()
    }
    playing() {
      !@_stopped && (@_ends == nil || @_graph.now() < @_ends)
    }
    stop() {
      @_graph.stop()
      @_stopped = true
    }
    # The control `name` at `v` (within its lo..hi), from `at` (now when nil),
    # in a straight line over `secs` when more than 0.
    set(name: String, v: Long | Float, secs: Long | Float = 0.0, at: Long | Float | Nil = nil) {
      let c = @_controls.get(name, nil)
      __raise("ValueError", "Synth.set: no control named `{name}` in what plays") if c == nil
      __raise("ValueError", "Synth.set: secs is a finite number, 0 or more") if !nonnegative(secs)
      let to = Math.clamp(v, c.lo ?? -Math.inf, c.hi ?? Math.inf)
      c.value.cancel(at ?? @_graph.now())
      # A playing graph's cancel lands at its clock at the earliest; from there.
      let t = Math.max(at ?? 0.0, @_graph.now())
      if secs > 0 {
        c.value.ramp(t + secs, to)
      } else {
        c.value.set(t, to)
      }
    }
    # Its first `seconds`, as what plays them works them out, the changes
    # made to it so far included.
    render(seconds: Long | Float) {
      @_graph.render(seconds)
    }
    _wait() {
      while @_graph.now() < @_ends {
        Time.sleep(Math.max(0.005, @_ends - @_graph.now()))
      }
    }
  }

  # What plays without being waited for, kept until it ends (a graph stops
  # with the last reference to it), and let go at the next play.
  let unwaited = {mut handles: []}
  let let_go_ended = fn () {
    unwaited.handles = unwaited.handles.filter(fn (h) {
      if h.playing() {
        return true
      }
      h.stop()
      false
    })
  }

  # ---- voices

  # How long a voice taken from the note it plays fades out first.
  let STEAL_FADE = 0.005

  # A note's change to a Voices' graph: a note on starts the voice's
  # envelopes and sets its freq (after fading the voice out, when the note
  # takes it from another), a note off lets its gated envelopes go as
  # `hold` does, and a cut fades the voice out until its next note. On a
  # playing graph the cancels land at its clock at the earliest, so what
  # follows them starts from there.
  fn apply_note(built, e) {
    let v = built.voices[e.voice]
    if e.kind == "cut" {
      v.gate.cancel(e.at)
      v.gate.ramp(Math.max(e.until, built.g.now()), 0.0)
    } else if e.kind == "on" {
      v.gate.cancel(e.at)
      v.pitch.cancel(e.at) if v.pitch != nil
      v.triggers.for_each(|tr| tr.value.cancel(e.at))
      let start = Math.max(e.at, built.g.now()) + (e.start - e.at)
      v.gate.ramp(start, 0.0) if e.start > e.at
      v.gate.set(start, 1.0)
      v.pitch.set(start, hz(e.note)) if v.pitch != nil
      v.triggers.for_each(|tr| run(tr.value, tr.envelope, start))
    } else {
      v.gated.for_each(|tr| tr.value.cancel(e.at))
      let t = Math.max(e.at, built.g.now())
      for tr in v.gated {
        let held = tr.envelope.hold(e.held + t - e.at)
        let last = held.times.size() - 1
        segment(tr.value, t, held.levels[last], 0.0, held.times[last], held.curves[last])
      }
    }
  }

  # How many of `xs`, in order of `key`, come before `x`: those whose key is
  # below it, or at most it with `through` (a binary search).
  fn count_before(xs, key, x, through) {
    mut lo = 0
    mut hi = xs.size()
    while lo < hi {
      let mid = (lo + hi) / 2
      let k = key(xs[mid])
      if k < x || (through && k == x) {
        lo = mid + 1
      } else {
        hi = mid
      }
    }
    lo
  }

  # A voice's notes never overlap (a note sounds until the next starts at the
  # latest), and are kept in order of their start: so the note sounding at
  # `t` is the last to start by then, and the voice is free from `from` to
  # `to` when the last to start before `to` has ended by `from`.
  let start_of = |n| n.start
  let at_of = |e| e.at
  # A note number as the key of the notes held: 60 and 60.0 are one note.
  let held_key = |note| "{note + 0.0}"
  fn sounding_at(v, t) {
    let i = count_before(v.notes, start_of, t, true)
    i > 0 && v.notes[i - 1].free > t ? v.notes[i - 1] : nil
  }
  fn free_between(v, from, to) {
    let i = count_before(v.notes, start_of, to, false)
    i == 0 || v.notes[i - 1].free <= from
  }
  fn next_start(v, t) {
    let i = count_before(v.notes, start_of, t, true)
    i < v.notes.size() ? v.notes[i].start : Math.inf
  }

  # `count` voices, each the expression `builder` returns for its freq (a
  # signal in Hz each note sets): a note goes to a voice free for as long as
  # it sounds, or else takes the voice whose sounding note began first. A
  # Voices keeps its notes, by voice and in time order, so render and play
  # build its graph with them; a note made while it plays is heard there too.
  class SynthVoices {
    new(count: Long, builder: Function) {
      __raise("ValueError", "Synth.Voices: count is 1 or more") if count < 1
      @_voices = iota(count).map(fn (i) {
        let pitch = SynthPart("pitch", [], {}, {})
        let expr = builder(pitch)
        __raise("TypeError", "type error: Synth.Voices' builder returns a SynthPart or a SynthEnvelope") unless expr is SynthPart | SynthEnvelope
        let sound = to_part(expr)
        check_playable(sound)
        {index: i, pitch_id: pitch.id, expr: sound, natural: ending(sound), notes: []}
      })
      @_events = []
      @_playing = []
      # The notes held, by note number, each with its voice, to let go.
      @_held = {}
    }
    # Where a note made now starts: the clock of the last play of it still
    # playing, or 0.
    now() {
      let live = self._live()
      live.empty() ? 0.0 : live[live.size() - 1].handle.now()
    }
    # `note` (a note number) from `at` (now when nil): on a voice free for as
    # long as it sounds; or else on one silent then, until that voice's next
    # note (fading out over the STEAL_FADE before it); or else on the voice
    # whose sounding note began first, which fades out first.
    note_on(note: Long | Float, at: Long | Float | Nil = nil) {
      __raise("ValueError", "Synth.Voices: note is a finite number") if !finite(note)
      let t = self._time(at)
      mut voice = @_voices.find(|v| free_between(v, t, t + v.natural))
      mut start = t
      mut cut = nil
      if voice == nil {
        let silent = @_voices.filter(|v| sounding_at(v, t) == nil)
        if !silent.empty() {
          voice = silent.max_by(|v| next_start(v, t))
          cut = next_start(voice, t)
        } else {
          voice = @_voices.min_by(|v| sounding_at(v, t).start)
          let taken = sounding_at(voice, t)
          # The fade ends by the voice's next note at the latest, so the notes
          # stay in order however close they come.
          start = Math.min(t + STEAL_FADE, next_start(voice, t))
          taken.free = start
          taken.held = false
        }
      }
      let placed = {note: note, at: t, start: start, mut free: Math.min(start + voice.natural, next_start(voice, t)), mut held: true}
      # After those starting by `t`: nothing else starts before `placed` ends.
      voice.notes.insert(count_before(voice.notes, start_of, t, true), placed)
      @_held.get_or_put(held_key(note), []).push({voice: voice, note: placed})
      self._add({kind: "on", voice: voice.index, at: t, start: start, note: note})
      self._add({kind: "cut", voice: voice.index, at: Math.max(t, cut - STEAL_FADE), until: cut}) if cut != nil
    }
    # The note `note` held longest let go from `at` (now when nil): its
    # gated envelopes fall to 0 over their release.
    note_off(note: Long | Float, at: Long | Float | Nil = nil) {
      let t = self._time(at)
      # Those let go or taken over are held no more; of the rest made for `t`
      # or before, the one that started first (on the first voice of a tie)
      # is let go.
      let held = @_held.get(held_key(note), []).filter(|h| h.note.held)
      @_held[held_key(note)] = held
      let ready = held.filter(|h| h.note.at <= t)
      return nil if ready.empty()
      let first = ready.min_by(|h| (h.note.start, h.voice.index))
      let (voice, chosen) = (first.voice, first.note)
      chosen.held = false
      let from = Math.max(t, chosen.start)
      # A note another has taken the voice from has nothing left to let go.
      return nil if from >= chosen.free
      chosen.free = Math.min(chosen.free, chosen.start + ending(voice.expr, from - chosen.start))
      self._add({kind: "off", voice: voice.index, at: from, held: from - chosen.start})
    }
    # `notes` one `step` apart from `at` (now when nil), each `len` long (a
    # step when nil); a nil note is a rest.
    sequence(notes: Array, step: Long | Float, len: Long | Float | Nil = nil, at: Long | Float | Nil = nil) {
      __raise("ValueError", "Synth.Voices.sequence: step is more than 0") if !(finite(step) && step > 0)
      __raise("ValueError", "Synth.Voices.sequence: len is more than 0") if len != nil && !(finite(len) && len > 0)
      let t0 = self._time(at)
      for i, note in notes.enumerate() {
        if note != nil {
          self.note_on(note, at: t0 + step * i)
          self.note_off(note, at: t0 + step * i + (len ?? step))
        }
      }
    }
    # When the notes made so far have ended, Math.inf while one is held.
    ends() {
      @_voices.reduce(0.0, |m, v| v.notes.empty() ? m : Math.max(m, v.notes[v.notes.size() - 1].free))
    }
    _time(at) {
      __raise("ValueError", "Synth.Voices: at is a finite number of seconds, 0 or more") if at != nil && !nonnegative(at)
      at ?? self.now()
    }
    # The plays not waited for that still play; the others are let go.
    _live() {
      @_playing = @_playing.filter(|p| p.handle.playing())
      @_playing
    }
    # A note's event kept in time order. A playing graph hears it, and then
    # its voice's events made for later, which the event's cancels dropped.
    _add(e) {
      let i = count_before(@_events, at_of, e.at, true)
      @_events.insert(i, e)
      let live = self._live()
      return nil if live.empty()
      let later = @_events.slice(i + 1, @_events.size()).filter(|x| x.voice == e.voice)
      for p in live {
        apply_note(p.built, e)
        let from = Math.max(e.at, p.built.g.now())
        later.filter(|x| x.at > from).for_each(|x| apply_note(p.built, x))
      }
    }
    # The graph at `rate`, with the notes made so far: a gain per voice (0
    # until its first note, its gate) and their mix the output.
    _built(rate) {
      let g = Audio.Graph(rate)
      let b = build_of(g)
      let voices = @_voices.map(fn (v) {
        let first = v.notes.empty() ? 440.0 : hz(v.notes[0].note)
        b.voice = {pitch_id: v.pitch_id, first: first, mut pitch: nil, triggers: []}
        let gate = g.gain(part(b, v.expr), 0.0)
        let triggers = b.voice.triggers
        let voice = {part: gate, gate: gate.gain, pitch: b.voice.pitch, triggers: triggers, gated: triggers.filter(|tr| tr.envelope.release != nil)}
        b.voice = nil
        voice
      })
      let out = g.mix(voices.map(|v| v.part))
      g.output(out)
      let built = {g: g, controls: b.controls, out: out, voices: voices}
      @_events.for_each(|e| apply_note(built, e))
      built
    }
    _played(built, handle) {
      @_playing.push({built: built, handle: handle})
    }
  }

  # `expr` played: until `secs`, or else until it ends on its own. With
  # `wait`, play returns when it has ended (at once when no device plays);
  # without, it returns the handle and the sound plays on (a Voices until it
  # is stopped, as notes may come).
  let play = fn (expr: SynthPart | SynthEnvelope | SynthVoices, wait: Bool = true, secs: Long | Float | Nil = nil) {
    let_go_ended()
    __raise("ValueError", "Synth.play: secs is a finite number, 0 or more") if secs != nil && !nonnegative(secs)
    let voices = expr is SynthVoices
    let built = voices ? expr._built(44100) : graph_of(expr, 44100)
    if secs != nil {
      # Silent from `secs` on, on the graph's own clock.
      let muted = built.g.gain(built.out, 1.0)
      schedule(muted.gain, env([1.0, 1.0, 0.0], [secs, 0.0]))
      built.g.output(muted)
    }
    let natural = voices ? (wait ? expr.ends() : Math.inf) : ending(expr)
    let ends = secs ?? (natural == Math.inf ? nil : natural)
    if wait && ends == nil {
      __raise("ValueError", "Synth.play: the sound does not end on its own (an oscillator, a loop or a held note plays on); give it secs, or play it with wait: false")
    }
    built.g.play()
    let h = SynthHandle(built.g, built.controls, ends)
    expr._played(built, h) if voices && !wait
    if !wait {
      unwaited.handles.push(h)
    } else if Audio.available() {
      h._wait()
      h.stop()
    }
    h
  }

  {
    wave: wave,
    sine: sine,
    saw: saw,
    square: square,
    triangle: triangle,
    noise: noise,
    impulse: impulse,
    dust: dust,
    lfnoise: lfnoise,
    pulse: pulse,
    env: env,
    line: line,
    xline: xline,
    adsr: adsr,
    perc: perc,
    control: control,
    mix: mix,
    feedback: feedback_loop,
    Voices: SynthVoices,
    lowpass: lowpass,
    highpass: highpass,
    bandpass: bandpass,
    resonate: resonate,
    envelope: envelope,
    hz: hz,
    db: db,
    render: render,
    play: play,
  }
}
let Synth = _synth_module()
)=culpre=";

inline constexpr const char* MIDI_MODULE_SOURCE = R"=culpre=(# MIDI: input from a MIDI controller and output to one (docs/stdlib.md),
# through _Audio's midi_* natives (vendor/cpp-midilib on the audio device's
# backend).
let _midi_module = fn () {
  # A channel message's kind, by the top half of its status byte (8 to 14).
  let KINDS = ["note_off", "note_on", "poly_pressure", "cc", "program", "pressure", "bend"]

  # The inputs (or, `output`, the outputs) there are, each {id, name}.
  fn ports(output) {
    let xs = _Audio.midi_ports(output)
    iota(xs.size() / 2).map(|i| {id: xs[i * 2], name: xs[i * 2 + 1]})
  }
  fn inputs() {
    ports(false)
  }
  fn outputs() {
    ports(true)
  }

  # "there are a, b" for the ports in `ps`, or "there are none".
  fn there_are(ps) {
    let names = ps.map(|p| "{p.id} ({p.name})")
    let has = names.empty() ? "none" : names.join(", ")
    "there are {has}"
  }

  # One message from the six numbers _Audio.midi_read gives each (seconds
  # ago, status, two data bytes, size, a note on's 14-bit velocity), `now`
  # the clock when they were read.
  fn message(xs, k, now) {
    let status = to_long(xs[k + 1])
    let (d1, d2) = (to_long(xs[k + 2]), to_long(xs[k + 3]))
    let channel = status & 15
    let at = now - xs[k]
    let bytes = to_long(xs[k + 4]) == 2 ? [status, d1] : [status, d1, d2]
    # A note on at velocity 0 is a note off, as MIDI 1.0 says.
    let kind = status >> 4 == 9 && d2 == 0 ? "note_off" : KINDS[(status >> 4) - 8]
    match kind {
      "note_on" => {kind: kind, channel: channel, at: at, bytes: bytes, note: d1, velocity: xs[k + 5] / 16383.0},
      "note_off" => {kind: kind, channel: channel, at: at, bytes: bytes, note: d1, velocity: d2 / 127.0},
      "poly_pressure" => {kind: kind, channel: channel, at: at, bytes: bytes, note: d1, value: d2 / 127.0},
      "cc" => {kind: kind, channel: channel, at: at, bytes: bytes, cc: d1, value: d2 / 127.0},
      "program" => {kind: kind, channel: channel, at: at, bytes: bytes, program: d1},
      "pressure" => {kind: kind, channel: channel, at: at, bytes: bytes, value: d1 / 127.0},
      _ => {kind: kind, channel: channel, at: at, bytes: bytes, bend: (d2 * 128 + d1 - 8192) / 8192.0},
    }
  }

  # An input, open: its messages wait from when it opens until they are
  # read, in the order they came. The controls (control()) are as the
  # messages read so far have set them. With `sysex`, a sysex comes too,
  # whole, as a message of its own.
  class Input {
    new(id: String, sysex: Bool = false) {
      @id = id
      @_id = _Audio.midi_open(id, sysex)
      if @_id < 0 {
        @_id = nil
        __raise("ValueError", "MIDI.Input: no input {id}; {there_are(inputs())}")
      }
    }
    # What came since the last read, all of it; [] when nothing has. A sysex
    # (status 240) has its size where a message's data bytes are, then its
    # bytes.
    read() {
      let xs = _Audio.midi_read(@_id)
      let now = _Time.monotonic()
      mut out = []
      mut k = 0
      while k < xs.size() {
        if xs[k + 1] == 240.0 {
          let n = to_long(xs[k + 2])
          out.push({kind: "sysex", at: now - xs[k], bytes: xs.slice(k + 6, k + 6 + n).map(|b| to_long(b))})
          k += 6 + n
        } else {
          out.push(message(xs, k, now))
          k += 6
        }
      }
      out
    }
    # Waits until a message has come, for `seconds` at most (for ever
    # without); whether one has.
    wait(seconds: Long | Float | Nil = nil) {
      return _Audio.midi_wait(@_id, seconds) if seconds != nil
      while !_Audio.midi_wait(@_id, 3600.0) {}
      true
    }
    # Each message as it comes, waiting for the next: a loop over it plays.
    messages() {
      while true {
        self.wait()
        for m in self.read() {
          yield m
        }
      }
    }
    # Controller `cc` on `channel` (0 to 15), 0.0 to 1.0: at 14 bits where
    # the controller sends them so (0 to 31 with 32 to 63), at 7 otherwise.
    control(cc: Long, channel: Long = 0) {
      _Audio.midi_control(@_id, channel, cc) / 16383.0
    }
    # Messages dropped because they came faster than they were read.
    dropped() {
      _Audio.midi_dropped(@_id)
    }
    # Closes it; a wait or a read after it is a TypeError.
    drop() {
      _Audio.midi_free(@_id) if @_id != nil
      @_id = nil
    }
  }

  # An output, open: send() sends it whole messages.
  class Output {
    new(id: String) {
      @id = id
      @_id = _Audio.midi_out_open(id)
      if @_id < 0 {
        @_id = nil
        __raise("ValueError", "MIDI.Output: no output {id}; {there_are(outputs())}")
      }
    }
    # Sends `bytes` (Longs, 0 to 255): channel messages and sysexes, whole,
    # one after another, with no running status. Anything else is a
    # ValueError, and none of it is sent.
    send(bytes: Array) {
      let wrong = _Audio.midi_send(@_id, bytes)
      __raise("ValueError", "MIDI.Output.send: {wrong}") if !wrong.empty()
    }
    # Closes it; a send after it is a TypeError.
    drop() {
      _Audio.midi_free(@_id) if @_id != nil
      @_id = nil
    }
  }

  {
    # The inputs there are, each {id, name}: an id to open, which stays the
    # same while the device is plugged in.
    inputs: inputs,
    # The outputs there are, the same way (a device's output has an id of
    # its own).
    outputs: outputs,
    Input: Input,
    Output: Output,
  }
}
let MIDI = _midi_module()
)=culpre=";

inline constexpr const char* ARGS_MODULE_SOURCE = R"=culpre=(let _args_module = fn () {
  # A spec that only routes to `subcommands` has no `args` of its own.
  let _args_of = fn (spec) {
    spec.get("args", [])
  }
  let _number = fn (convert, raw, type, name) {
    try {
      convert(raw)
    } catch _ {
      __raise("ArgParseError", "argument '{name}' expects {type}, got '{raw}'")
    }
  }
  let _coerce = fn (raw, type, name) {
    return raw if type == "String"
    return _number(to_long, raw, type, name) if type == "Long"
    return _number(to_float, raw, type, name) if type == "Float"
    if type == "Bool" {
      return true if raw == "true" || raw == "1"
      return false if raw == "false" || raw == "0"
      __raise("ArgParseError", "argument '{name}' expects Bool, got '{raw}'")
    }
    __raise("ArgParseError", "argument '{name}' has unknown type '{type}'")
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
  let _name_of = fn (spec) {
    spec.get("name", "program")
  }
  # A help section: each row's name padded to the longest, then its doc.
  let _section = fn (title, rows) {
    let w = rows.map(|r| _Term.width(r[0])).max()
    let lines = rows.map(|r| "  {r[0]:<{w}}  {r[1]}".trim_end())
    "\n{title}:\n{lines.join("\n")}\n"
  }
  let _format_help = fn (spec) {
    let name = _name_of(spec)
    let doc = spec.get("doc", "")
    let mut parts = []
    if doc != "" {
      parts.push("{name} - {doc}\n\n")
    }
    let pos_args = _args_of(spec).filter(_is_positional)
    let opt_args = _args_of(spec).filter(_is_option)
    let subs = spec.get("subcommands", [])
    parts.push("Usage: {name}")
    parts.push(" [options]") if !opt_args.empty()
    for a in pos_args {
      if _is_repeated(a) {
        parts.push(" [<{a.name}>...]")
      } else if _is_optional(a) {
        parts.push(" [<{a.name}>]")
      } else {
        parts.push(" <{a.name}>")
      }
    }
    parts.push(" <command>") if !subs.empty()
    parts.push("\n")
    if !subs.empty() {
      parts.push(_section("Commands", subs.map(|c| [c.name, c.get("doc", "")])))
    }
    if !pos_args.empty() {
      parts.push(_section(
        "Arguments",
        pos_args.map(|a| [a.name, a.get("doc", "")]),
      ))
    }
    let opts = opt_args.map(fn (a) {
      let short = a.has("short") ? "-{a.short}, " : "    "
      ["{short}--{a.name}", a.get("doc", "")]
    })
    parts.push(_section(
      "Options",
      [...opts, ["-h, --help", "show this help and exit"]],
    ))
    parts.join("")
  }
  # `--name[=v]` and `-n[=v]` differ only in how many dashes they carry, and
  # `-h`/`--help` throws the help. Puts what it read into `result`, and answers
  # the index of the last token it took — one past `i` when the value was the
  # next token rather than after the `=`.
  let _take_option = fn (result, argv, i, spec) {
    if argv[i] == "-h" || argv[i] == "--help" {
      throw {kind: "ArgParseHelp", help: _format_help(spec)}
    }
    let dash = argv[i].starts_with("--") ? "--" : "-"
    let parts = argv[i].slice(dash.size(), argv[i].size()).split("=")
    let name = parts[0]
    let has_value = parts.size() > 1
    let spec_a = _find_by_name(_args_of(spec), name)
    if spec_a == nil {
      __raise("ArgParseError", "unknown option '{dash}{name}'")
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
        __raise("ArgParseError", "option '{dash}{name}' expects a value")
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
  # The positionals, then what was left out: fills `result` from them as
  # `spec` says.
  let _fill = fn (result, positionals, spec) {
    # Positionals are filled in spec order, but one that may be omitted only
    # takes a token when more are left than the required ones after it still
    # need. That is what makes `<files>... <dest>` hand `dest` the last token
    # instead of swallowing it, and `[from] <to>` fill `to` from a lone token.
    let pos_specs = _args_of(spec).filter(_is_positional)
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
      __raise(
        "ArgParseError",
        "unexpected positional argument '{positionals[taken]}'",
      )
    }
    for a in _args_of(spec) {
      if !result.has(a.name) {
        if !_is_optional(a) {
          __raise("ArgParseError", "missing required argument '{a.name}'")
        }
        result[a.name] = _missing_value(a)
      }
    }
    result
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
      } else if tok.starts_with("-") && tok.size() > 1 {
        i = _take_option(result, argv, i, spec)
      } else {
        positionals.push(tok)
      }
      i += 1
    }
    _fill(result, positionals, spec)
  }
  # The program's options come before the subcommand, as git's do, and the
  # subcommand's after it; the result has both, and `subcommand`.
  let _parse_routed = fn (argv, spec) {
    # Its first positional token is the subcommand, so it has none of its own.
    let own_positional = _args_of(spec).find(_is_positional)
    if own_positional != nil {
      __raise(
        "ArgParseError",
        "argument '{own_positional.name}' is positional, and a spec with subcommands takes options only",
      )
    }
    let mut result = {}
    let mut i = 0
    while i < argv.size() {
      let tok = argv[i]
      if tok.starts_with("-") && tok.size() > 1 {
        i = _take_option(result, argv, i, spec)
      } else {
        let sub = spec.subcommands.find(|c| c.name == tok)
        __raise("ArgParseError", "unknown subcommand '{tok}'") if sub == nil
        _fill(result, [], spec)
        # Its help is named as it is run, the program's name and then its own.
        let named = {...sub, name: "{_name_of(spec)} {sub.name}"}
        let own = _parse_impl_flat(argv.slice(i + 1, argv.size()), named)
        let both = own.keys().find(|k| result.has(k))
        if both != nil {
          __raise(
            "ArgParseError",
            "argument '{both}' is both the program's and {sub.name}'s",
          )
        }
        if result.has("subcommand") || own.has("subcommand") {
          __raise(
            "ArgParseError",
            "argument 'subcommand' is the name the result gives the subcommand",
          )
        }
        return {...result, ...own, subcommand: sub.name}
      }
      i += 1
    }
    __raise("ArgParseError", "expected subcommand")
  }
  let _parse_impl = fn (argv, spec) {
    spec.has("subcommands")
      ? _parse_routed(argv, spec)
      : _parse_impl_flat(argv, spec)
  }
  {
    try_parse: fn (argv, spec) {
      _parse_impl(argv, spec)
    },
    parse: fn (argv, spec) {
      # Nothing to run: what there is to run, as git and clap show it.
      if argv.empty() && spec.has("subcommands") {
        IO.eprint(_format_help(spec))
        Sys.exit(2)
      }
      try {
        _parse_impl(argv, spec)
      } catch e {
        if e.has("kind") && e.kind == "ArgParseHelp" {
          IO.print(e.help)
          Sys.exit(0)
        }
        let msg = e.get("message", to_string(e))
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
// `message:` is keyword-only: a third positional elsewhere is a label.
let assert_throws = fn (kind, f, *, message: String? = nil) {
  if f.params.size() != 0 {
    __raise(
      "ArityError",
      "assert_throws: fn must take 0 parameters (got {f.params.size()})",
    )
  }
  let mut threw = false
  let mut thrown = nil
  try {
    f()
  } catch e {
    threw = true
    thrown = e
  }
  if !threw {
    __raise(
      "AssertionError",
      "assert_throws('{kind}', fn): expected throw but fn returned normally",
    )
  }
  let actual_kind = if type_of(thrown) == "Object" && thrown.has("kind") {
    thrown.kind
  } else {
    type_of(thrown)
  }
  if actual_kind != kind {
    __raise(
      "AssertionError",
      "assert_throws: expected kind '{kind}' but got '{actual_kind}'",
    )
  }
  if message != nil {
    // A scalar's property read throws, while anything else (a class instance
    // included) answers nil for a missing one; catching instead would also
    // hide a TypeError from a user `get message()`.
    let actual = if ["Long", "Float", "Bool", "Nil"].contains(type_of(thrown)) {
      nil
    } else {
      thrown.message
    }
    if actual != message {
      let shown = if actual == nil { "nil" } else { "'{actual}'" }
      __raise(
        "AssertionError",
        "assert_throws: expected message '{message}' but got {shown}",
      )
    }
  }
  thrown
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
      @_pat = pattern
      _Regex.check(pattern)
    }
    test(s) {
      _Regex.test(@_pat, s)
    }
    find(s) {
      _Regex.find(@_pat, s)
    }
    match(s) {
      _Regex.match(@_pat, s)
    }
    find_all(s) {
      _Regex.find_all(@_pat, s)
    }
    find_all_str(s) {
      _Regex.find_all_str(@_pat, s)
    }
    find_all_index(s) {
      _Regex.find_all_index(@_pat, s)
    }
    count(s) {
      _Regex.count(@_pat, s)
    }
    find_iter(s) {
      _regex_find_iter(@_pat, s)
    }
    replace_all(s, repl) {
      if repl !is Function {
        return _Regex.replace_all(@_pat, s, repl)
      }
      let mut out = ""
      let mut last = 0
      for m in _Regex.find_all(@_pat, s) {
        out = out + s.slice(last, m.start) + repl(m)
        last = m.end
      }
      out + s.slice(last, s.size())
    }
    replace_first(s, repl) {
      if repl !is Function {
        return _Regex.replace_first(@_pat, s, repl)
      }
      let m = _Regex.find(@_pat, s)
      return s if m == nil
      s.slice(0, m.start) + repl(m) + s.slice(m.end, s.size())
    }
    split(s) {
      _Regex.split(@_pat, s)
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
fn _peg_ast_to_s_at(node, level) {
  let pad = "  ".repeat(level)
  let mut out = if node.is_token {
    pad + "- " + node.name + " (" + node.token + ")\n"
  } else {
    pad + "+ " + node.name + "\n"
  }
  for c in node.nodes {
    out += _peg_ast_to_s_at(c, level + 1)
  }
  out
}
fn _peg_ast_to_s(node) {
  _peg_ast_to_s_at(node, 0)
}

# cpp-peglib's AstOptimizer over a tree `parse` handed back: a node with one
# child gives way to that child, which takes the node's extent. `no_ast_opt`
# is the node names of the grammar's rules marked so. With `opt_mode` those
# are the nodes that stay; without it they are the only ones that go. A node
# of such a rule keeps its own extent wherever it ends up.
fn _peg_optimize(node, no_ast_opt, opt_mode) {
  let copy = fn (n, nodes, position, length) {
    {
      name: n.name,
      token: n.token,
      nodes: nodes,
      line: n.line,
      column: n.column,
      position: position,
      length: length,
      is_token: n.is_token,
      choice: n.choice,
    }
  }
  if no_ast_opt.contains(node.name) != opt_mode && node.nodes.size() == 1 {
    let child = _peg_optimize(node.nodes[0], no_ast_opt, opt_mode)
    return child if no_ast_opt.contains(child.name)
    return copy(child, child.nodes, node.position, node.length)
  }
  copy(
    node,
    node.nodes.map(|c| _peg_optimize(c, no_ast_opt, opt_mode)),
    node.position,
    node.length,
  )
}

# The number a token starts with, as cpp-peglib's `token_to_number<T>()`
# reads one: the digits it begins with, and 0 when it begins with none. The
# type is the caller's there; here the token says it, a Float when a fraction
# or an exponent follows the digits and a Long otherwise.
fn _peg_number(token) {
  let b = token.bytes().collect()
  let n = b.size()
  let digits = fn (from) {
    let mut i = from
    while i < n && b[i] >= 48 && b[i] <= 57 {
      i += 1
    }
    i
  }
  let start = n > 0 && b[0] == 45 ? 1 : 0
  let mut end = digits(start)
  return 0 if end == start
  let mut float = false
  if end + 1 < n && b[end] == 46 && digits(end + 1) > end + 1 {
    end = digits(end + 1)
    float = true
  }
  if end < n && (b[end] == 101 || b[end] == 69) {
    let sign = end + 1 < n && (b[end + 1] == 43 || b[end + 1] == 45) ? 1 : 0
    if digits(end + 1 + sign) > end + 1 + sign {
      end = digits(end + 1 + sign)
      float = true
    }
  }
  float ? to_float(token.slice(0, end)) : to_long(token.slice(0, end))
}

# The functions the native side calls for a parse (bindings.h), each handing
# the program's own what cpp-peglib would hand it, in cpp-peglib's order.
# `dt` is the user data the parse was called with. They sit out here,
# reaching the module through its name, so that a parser object closes over
# none of them and crosses an isolate boundary as its grammar text alone.

# A callback leaves its trailing parameters out: an action `dt` and
# `predicate_data`, the others `dt`, a logger the rule. cpp-peglib picks the
# call by the function's own parameter count, and so does this. `verdict` is
# what the rule's predicate answered, nil without one.
fn _peg_wrap_action(f, path, dt) {
  let n = f.params.size()
  cond {
    n < 2 => |raw, verdict| f(PEG.SemanticValues.new(raw, path)),
    n < 3 => |raw, verdict| f(PEG.SemanticValues.new(raw, path), dt),
    _ => |raw, verdict| f(PEG.SemanticValues.new(raw, path), dt, verdict?[2]),
  }
}
fn _peg_wrap_log(f) {
  f.params.size() < 4 ? |line, col, msg, rule| f(line, col, msg) : f
}
# cpp-peglib's two out parameters come back as returned values: a predicate
# answers `ok`, `(ok, msg)` or `(ok, msg, predicate_data)`.
fn _peg_wrap_predicate(f, path, dt) {
  let ask = f.params.size() < 2 ? f : |vs| f(vs, dt)
  fn (raw) {
    let r = ask(PEG.SemanticValues.new(raw, path))
    let (ok, msg, data) = match r {
      (a, b, c) => (a, b, c),
      (a, b) => (a, b, nil),
      _ => (r, "", nil),
    }
    let said = type_of(msg)
    if ok !is Bool || said != "String" && said != "StringView" {
      __raise(
        "TypeError",
        "PEG: a predicate answers Bool, (Bool, String) or (Bool, String, Any), got {r}",
      )
    }
    (ok, msg, data)
  }
}
# cpp-peglib's Context, down to its constant members: the subject and its
# name. A handler's own `s` and `n` are the rest of the subject from where
# the rule was entered, and its length.
fn _peg_context(text, path) {
  {path: path, s: text, l: text.size()}
}
fn _peg_wrap_enter(f, text, path, dt) {
  let tell = f.params.size() < 4 ? f : |c, s, n| f(c, s, n, dt)
  fn (pos) {
    tell(_peg_context(text, path), text.slice(pos, text.size()), text.size() - pos)
  }
}
fn _peg_wrap_leave(f, text, path, dt) {
  let tell = f.params.size() < 6 ? f : |c, s, n, matchlen, value| f(c, s, n, matchlen, value, dt)
  fn (pos, matchlen, value) {
    tell(
      _peg_context(text, path),
      text.slice(pos, text.size()),
      text.size() - pos,
      matchlen,
      value,
    )
  }
}

# What one parse runs with, as the Object `_PEG.run` reads. With an action
# on any rule the parse reduces to the start rule's value. nil when there is
# nothing to run.
fn _peg_hooks(rules, log, text, path, dt) {
  let acts = {}
  let preds = {}
  let enters = {}
  let leaves = {}
  for name, d in rules {
    for member, f in d {
      if f != nil && f !is Function {
        __raise("TypeError", "PEG: rule '{name}': {member} is {type_of(f)}, not a Function")
      }
    }
    if d.action != nil {
      acts[name] = _peg_wrap_action(d.action, path, dt)
    }
    if d.predicate != nil {
      preds[name] = _peg_wrap_predicate(d.predicate, path, dt)
    }
    if d.enter != nil {
      enters[name] = _peg_wrap_enter(d.enter, text, path, dt)
    }
    if d.leave != nil {
      leaves[name] = _peg_wrap_leave(d.leave, text, path, dt)
    }
  }
  if acts.empty() && log == nil && preds.empty() && enters.empty() && leaves.empty() {
    return nil
  }
  {
    actions: acts,
    predicates: preds,
    enter: enters,
    leave: leaves,
    log: log == nil ? nil : _peg_wrap_log(log),
  }
}

# The parser object is cpp-peglib's `peg::parser`, method for method:
# `load_grammar` answers whether the grammar loaded, `parser["RULE"]` is the
# rule, whose `action`, `predicate`, `enter` and `leave` a program assigns,
# and `parse` answers `(ok, val)` -- cpp-peglib's `bool parse(s, val)` with
# its out parameter as one more returned value.
let _peg_module = fn () {
  # What a rule's action or predicate is handed: cpp-peglib's SemanticValues.
  # `raw` is the Object the native side builds for the match (bindings.h).
  class SemanticValues {
    new(raw, path) {
      @_raw = raw
      @tokens = raw.tokens
      @path = path
    }
    sv() {
      @_raw.sv
    }
    name() {
      @_raw.name
    }
    line_info() {
      (@_raw.line, @_raw.column)
    }
    choice() {
      @_raw.choice
    }
    choice_count() {
      @_raw.choice_count
    }
    token(id = 0) {
      @tokens.empty() ? @_raw.sv : @tokens[id]
    }
    token_to_string(id = 0) {
      @token(id)
    }
    token_to_number() {
      _peg_number(@token())
    }
    transform(beg = 0, end = -1) {
      @_raw.values.slice(beg, end < 0 ? @_raw.values.size() : end)
    }
    size() {
      @_raw.values.size()
    }
    empty() {
      @_raw.values.empty()
    }
    __index__(i) {
      @_raw.values[i]
    }
    iter() {
      for v in @_raw.values {
        yield v
      }
    }
  }

  class PEG {
    new(grammar = nil, start = "") {
      @_log = nil
      @_reset(nil, "", [])
      @load_grammar(grammar, start) if grammar != nil
    }
    # What is set on a grammar's rules goes with the grammar, as in
    # cpp-peglib: the callbacks, enable_ast() and enable_packrat_parsing().
    _reset(src, start, no_ast_opt) {
      @_src = src
      @_start = start
      @_no_ast_opt = no_ast_opt
      @_ast = 0
      @_packrat = false
      @_rules = {}
    }
    # The logger is the parser's, and is told what peglib says of a grammar:
    # why it did not load, or what it warns of in one that did.
    load_grammar(grammar, start = "") {
      let (ok, diags, no_ast_opt) = _PEG.load(grammar, start)
      @_reset(ok ? grammar : nil, start, no_ast_opt)
      if @_log != nil {
        let log = _peg_wrap_log(@_log)
        for (line, col, msg, rule) in diags {
          log(line, col, msg, rule)
        }
      }
      ok
    }
    # A rule of the grammar: cpp-peglib's Definition, down to the four
    # members a program assigns.
    __index__(rule) {
      @_rules[rule] ??= {
        mut action: nil,
        mut predicate: nil,
        mut enter: nil,
        mut leave: nil,
      }
      @_rules[rule]
    }
    __setindex__(rule, action) {
      self[rule].action = action
    }
    set_logger(log) {
      @_log = log
    }
    # `_ast` is what `_PEG.run` reads: 1 the tree as it is built, 2 and 3
    # that tree through optimize_ast() with `opt_mode` true and false.
    enable_ast(collapse_mode = false, opt_mode = true) {
      @_ast = cond {
        !collapse_mode => 1,
        opt_mode => 2,
        _ => 3,
      }
      self
    }
    optimize_ast(ast, opt_mode = true) {
      _peg_optimize(ast, @_no_ast_opt, opt_mode)
    }
    enable_packrat_parsing() {
      @_packrat = true
      nil
    }
    parse(text, path = "", dt = nil) {
      return (false, nil) if @_src == nil
      _PEG.run(
        @_src,
        text,
        @_start,
        @_ast,
        @_packrat,
        _peg_hooks(@_rules, @_log, text, path, dt),
      )
    }
  }
  {
    parser: fn (grammar = nil, start = "") {
      PEG.new(grammar, start)
    },
    walk: _peg_walk,
    find: _peg_find,
    find_all: _peg_find_all,
    ast_to_s: _peg_ast_to_s,
    PEG: PEG,
    SemanticValues: SemanticValues,
  }
}
let PEG = _peg_module()
)=culpre=";

inline constexpr const char* FST_MODULE_SOURCE = R"=culpre=(let _fst_module = fn () {
  class Set {
    new(bytecode) {
      @_bc = bytecode
      _FST.set_check(bytecode)
    }
    contains(key) {
      _FST.set_contains(@_bc, key)
    }
    common_prefix_search(text) {
      _FST.set_common_prefix_search(@_bc, text)
    }
    longest_common_prefix_search(text) {
      _FST.set_longest_common_prefix_search(@_bc, text)
    }
    predictive_search(prefix) {
      _FST.set_predictive_search(@_bc, prefix)
    }
    edit_distance_search(
      word,
      max_edits,
      insert_cost = 1,
      delete_cost = 1,
      replace_cost = 1,
    ) {
      _FST.set_edit_distance_search(
        @_bc,
        word,
        max_edits,
        insert_cost,
        delete_cost,
        replace_cost,
      )
    }
    suggest(word) {
      _FST.set_suggest(@_bc, word)
    }
  }
  class Map {
    new(bytecode) {
      @_bc = bytecode
      _FST.map_check(bytecode)
    }
    get(key) {
      _FST.map_get(@_bc, key)
    }
    common_prefix_search(text) {
      _FST.map_common_prefix_search(@_bc, text)
    }
    longest_common_prefix_search(text) {
      _FST.map_longest_common_prefix_search(@_bc, text)
    }
    predictive_search(prefix) {
      _FST.map_predictive_search(@_bc, prefix)
    }
    edit_distance_search(
      word,
      max_edits,
      insert_cost = 1,
      delete_cost = 1,
      replace_cost = 1,
    ) {
      _FST.map_edit_distance_search(
        @_bc,
        word,
        max_edits,
        insert_cost,
        delete_cost,
        replace_cost,
      )
    }
    suggest(word) {
      _FST.map_suggest(@_bc, word)
    }
  }
  class IndexMap {
    new(bytecode) {
      @_bc = bytecode
      _FST.index_check(bytecode)
    }
    get(key) {
      _FST.index_get(@_bc, key)
    }
    common_prefix_search(text) {
      _FST.index_common_prefix_search(@_bc, text)
    }
    longest_common_prefix_search(text) {
      _FST.index_longest_common_prefix_search(@_bc, text)
    }
    predictive_search(prefix) {
      _FST.index_predictive_search(@_bc, prefix)
    }
    edit_distance_search(
      word,
      max_edits,
      insert_cost = 1,
      delete_cost = 1,
      replace_cost = 1,
    ) {
      _FST.index_edit_distance_search(
        @_bc,
        word,
        max_edits,
        insert_cost,
        delete_cost,
        replace_cost,
      )
    }
    suggest(word) {
      _FST.index_suggest(@_bc, word)
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
  if pat is String || pat is StringView {
    s.split(pat).join(repl)
  } else {
    pat.replace_all(s, repl)
  }
}

let replace_first = fn (s, pat, repl) {
  if pat is String || pat is StringView {
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
    __raise("ValueError", "Log.set_level: unknown level '{level}'") if n < 0
    _threshold = n
    nil
  }
  let _set_format = fn (format) {
    if format != "text" && format != "json" {
      __raise("ValueError", "Log.set_format: unknown format '{format}'")
    }
    _format = format
    nil
  }
  {..._methods({}), set_level: _set_level, set_format: _set_format}
}
let Log = _log_module()
)=culpre=";

inline constexpr const char* DESKTOP_MODULE_SOURCE = R"=culpre=(let _desktop_module = fn () {
  # A Tauri-shaped desktop facade: local HTTP server + native WebView + assets,
  # in one call.
  # The server is the window's own: the app's routes (and /__quit) are for the
  # WebView on this machine, not for the network. listen_async binds every
  # interface unless it is told a host.
  let LOOPBACK = "127.0.0.1"

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
      srv.listen_async(config["port"], LOOPBACK, workers:)
    } else {
      try {
        srv.listen_async(8731, LOOPBACK, workers:)
      } catch _ {
        # Default port taken (most likely another culebra desktop app): retry
        # once on an OS-assigned free port — a failed bind leaves the handle
        # unbound and its routes recorded, so the same server serves the retry.
        # A non-bind startup error reproduces identically here and propagates.
        srv.listen_async(0, LOOPBACK, workers:)
      }
    }
    let base = "http://" + LOOPBACK + ":" + port.to_string()

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
      __raise("TypeError", "type error: expected String|Path, got {t}")
    }
  }
  class Path {
    new(p) {
      @_path = _s(p)
    }

    # --- display / conversion ---
    __str__() {
      @_path
    }  # "{p}" and to_string(p) yield the raw path
    str() {
      @_path
    }  # explicit String escape hatch
    # equal only to a String/StringView or another Path with the same path;
    # any other type is simply not-equal (never a TypeError).
    __eq__(o) {
      if o is String || o is StringView {
        @_path == o
      } else if _is_path(o) {
        @_path == to_string(o)
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
      @_path < _s(o)
    }

    # --- joining: `base / "sub" / "leaf"` ---
    join(other) {
      Path.new(FS.join(@_path, _s(other)))
    }
    __div__(o) {
      @join(o)
    }

    # --- path components (String, mirroring FS.*) ---
    # Pure, total, O(1) string derivations, so they read as properties
    # (`p.name`, `p.parent`) — no parens. `p.name()` still works too. The
    # filesystem ops below stay methods: they do I/O and can throw.
    get name() {
      FS.basename(@_path)
    }  # final component, e.g. "content.js"
    get stem() {
      FS.stem(@_path)
    }  # final component without suffix
    get suffix() {
      FS.extension(@_path)
    }  # extension incl. dot, e.g. ".js"
    get parent() {
      Path.new(FS.dirname(@_path))
    }

    # --- queries ---
    exists() {
      FS.exists(@_path)
    }
    is_file() {
      FS.is_file(@_path)
    }
    is_dir() {
      FS.is_dir(@_path)
    }

    # --- filesystem ops (delegate to FS) ---
    read() {
      FS.read(@_path)
    }
    write(content) {
      FS.write(@_path, content)
    }
    mkdir() {
      FS.mkdir(@_path)
    }  # creates parents (FS.mkdir does)
    remove(recursive = false) {
      FS.remove(@_path, recursive:)
    }
    rename(dst) {
      FS.rename(@_path, _s(dst))
      Path.new(_s(dst))
    }

    # --- normalization ---
    resolve() {
      Path.new(FS.abspath(@_path))
    }

    # --- directory listing / globbing (return Path, so chains stay Path) ---
    list() {
      FS.list_dir(@_path).map(fn (e) {
        Path.new(FS.join(@_path, e))
      })
    }
    glob(pattern) {
      FS.glob(FS.join(@_path, pattern)).map(fn (p) {
        Path.new(p)
      })
    }
    walk() {
      FS.walk(@_path).map(fn (p) {
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
      @x = to_float(x)
      @y = to_float(y)
    }

    __str__() {
      "({@x}, {@y})"
    }

    __add__(o) {
      Vector2.new(@x + o.x, @y + o.y)
    }
    __sub__(o) {
      Vector2.new(@x - o.x, @y - o.y)
    }
    __mul__(k: Long | Float) {
      Vector2.new(@x * k, @y * k)
    }
    __neg__() {
      Vector2.new(-@x, -@y)
    }

    # Each of these spells its arithmetic out rather than delegating
    # (length -> length_squared -> dot): a dynamic call costs more than the
    # two products it would wrap, and these are what a per-frame loop calls.
    dot(other) {
      @x * other.x + @y * other.y
    }
    length_squared() {
      let x = @x
      let y = @y
      x * x + y * y
    }
    length() {
      let x = @x
      let y = @y
      Math.sqrt(x * x + y * y)
    }
    # A zero vector's normalized() divides by 0.0 like any other Float
    # division in culebra — raises ZeroDivisionError. No silent fallback.
    normalized() {
      let x = @x
      let y = @y
      let len = Math.sqrt(x * x + y * y)
      Vector2.new(x / len, y / len)
    }
    # Component-wise, not `(self - o).length_squared()` — avoids allocating a
    # throwaway Vector2 just to measure it. Ranking or thresholding distances
    # never needs the square root, so distance_squared_to is the form a hot
    # path wants.
    distance_to(other) {
      let dx = @x - other.x
      let dy = @y - other.y
      Math.sqrt(dx * dx + dy * dy)
    }
    distance_squared_to(other) {
      let dx = @x - other.x
      let dy = @y - other.y
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
      @x = to_float(x)
      @y = to_float(y)
      @z = to_float(z)
    }

    __str__() {
      "({@x}, {@y}, {@z})"
    }

    __add__(o) {
      Vector3.new(@x + o.x, @y + o.y, @z + o.z)
    }
    __sub__(o) {
      Vector3.new(@x - o.x, @y - o.y, @z - o.z)
    }
    __mul__(k: Long | Float) {
      Vector3.new(@x * k, @y * k, @z * k)
    }
    __neg__() {
      Vector3.new(-@x, -@y, -@z)
    }

    # Each of these spells its arithmetic out rather than delegating
    # (length -> length_squared -> dot): a dynamic call costs more than the
    # three products it would wrap, and these are what a per-frame loop calls.
    dot(other) {
      @x * other.x + @y * other.y + @z * other.z
    }
    length_squared() {
      let x = @x
      let y = @y
      let z = @z
      x * x + y * y + z * z
    }
    length() {
      let x = @x
      let y = @y
      let z = @z
      Math.sqrt(x * x + y * y + z * z)
    }
    normalized() {
      let x = @x
      let y = @y
      let z = @z
      let len = Math.sqrt(x * x + y * y + z * z)
      Vector3.new(x / len, y / len, z / len)
    }
    # Component-wise, not `(self - o).length_squared()` — avoids allocating a
    # throwaway Vector3 just to measure it. Ranking or thresholding distances
    # never needs the square root, so distance_squared_to is the form a hot
    # path wants.
    distance_to(other) {
      let dx = @x - other.x
      let dy = @y - other.y
      let dz = @z - other.z
      Math.sqrt(dx * dx + dy * dy + dz * dz)
    }
    distance_squared_to(other) {
      let dx = @x - other.x
      let dy = @y - other.y
      let dz = @z - other.z
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
      @_buf = []
      @_head = 0
      @_count = 0
    }

    size() {
      @_count
    }
    empty() {
      @_count == 0
    }

    _grow() {
      let old_cap = @_buf.size()
      let new_cap = old_cap == 0 ? 8 : old_cap * 2
      let mut new_buf = range(@_count)
        .map(|i| @_buf[(@_head + i) % old_cap])
        .collect()
      new_buf.extend(repeat(new_cap - @_count, nil))
      @_buf = new_buf
      @_head = 0
    }

    push_back(x) {
      if @_count == @_buf.size() {
        @_grow()
      }
      @_buf[(@_head + @_count) % @_buf.size()] = x
      @_count += 1
    }

    push_front(x) {
      if @_count == @_buf.size() {
        @_grow()
      }
      @_head = (@_head - 1 + @_buf.size()) % @_buf.size()
      @_buf[@_head] = x
      @_count += 1
    }

    pop_front() {
      return nil if @empty()
      let x = @_buf[@_head]
      @_buf[@_head] = nil
      @_head = (@_head + 1) % @_buf.size()
      @_count -= 1
      x
    }

    pop_back() {
      return nil if @empty()
      let i = (@_head + @_count - 1) % @_buf.size()
      let x = @_buf[i]
      @_buf[i] = nil
      @_count -= 1
      x
    }

    peek_front() {
      return nil if @empty()
      @_buf[@_head]
    }

    peek_back() {
      return nil if @empty()
      @_buf[(@_head + @_count - 1) % @_buf.size()]
    }

    to_array() {
      _deque_iter(@_buf, @_head, @_count).collect()
    }

    iter() {
      _deque_iter(@_buf, @_head, @_count)
    }

    __str__() {
      "Deque({@to_array()})"
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
      @_heap = []
      @_key = key
      @_reverse = reverse
    }

    size() {
      @_heap.size()
    }
    empty() {
      @_heap.empty()
    }

    # Heap-array order is not priority order, so echoing `_heap` here
    # (the default formatter's fallback) would mislead more than help.
    __str__() {
      "PriorityQueue(size={@size()})"
    }

    _less(a, b) {
      let ka = @_key == nil ? a : @_key(a)
      let kb = @_key == nil ? b : @_key(b)
      @_reverse ? kb < ka : ka < kb
    }

    _swap(i, j) {
      (@_heap[i], @_heap[j]) = (@_heap[j], @_heap[i])
    }

    push(x) {
      @_heap.push(x)
      let mut i = @_heap.size() - 1
      while i > 0 {
        let parent = (i - 1) / 2
        if @_less(@_heap[i], @_heap[parent]) {
          @_swap(i, parent)
          i = parent
        } else {
          break
        }
      }
    }

    peek() {
      return nil if @empty()
      @_heap[0]
    }

    pop() {
      return nil if @empty()
      let top = @_heap[0]
      let last = @_heap.pop()
      if !@_heap.empty() {
        @_heap[0] = last
        @_sift_down(0)
      }
      top
    }

    _sift_down(start) {
      let mut i = start
      let n = @_heap.size()
      while true {
        let l = i * 2 + 1
        let r = i * 2 + 2
        let mut smallest = i
        if l < n && @_less(@_heap[l], @_heap[smallest]) {
          smallest = l
        }
        if r < n && @_less(@_heap[r], @_heap[smallest]) {
          smallest = r
        }
        if smallest == i {
          break
        }
        @_swap(i, smallest)
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
    __raise("StateMachineError", msg)
  }
  fn _want(what, got, where) {
    __raise(
      "TypeError",
      "type error: expected {what} for {where}, got {type_of(got)}",
    )
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
    return v if v is Function
    _want("Function|String", v, "{kind} at {where}") if v !is String
    let f = table.get(v, nil)
    _fail("{where}: no {kind} named '{v}'") if f == nil
    f
  }

  fn _candidates(spec, guards, actions, where) {
    let mut out = []
    for c in spec is Array ? spec : [spec] {
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
  # `sv` is the native side's own Object for a match, the one the `PEG`
  # module wraps in its SemanticValues.
  let ACTIONS = {
    Ident: |sv| sv.tokens[0],
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
    new(desc, *, @name = "", guards = {}, actions = {}, @context = nil) {
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
      @_states = states
      @_boot = _path(states, ROOT, _leaf(states, ROOT))
      # Standing at the root, `reset` is exactly "enter the initial one".
      @_current = ROOT
      @reset()
    }

    # Build from the text DSL. A malformed machine raises `PEGError`, whose
    # message carries `path` when one is given.
    static parse(text, *, guards = {}, actions = {}, context = nil, path = "") {
      # _PEG.parse(grammar, text, start, optimize, packrat, path, hooks)
      let m = _PEG.parse(GRAMMAR, text, "", true, true, path, {actions: ACTIONS})
      StateMachine.new(m.states, name: m.name, guards:, actions:, context:)
    }

    state() {
      @_current
    }

    # True for the active leaf and for every composite state containing it.
    in_state(name) {
      let mut s = @_current
      while s != ROOT {
        return true if s == name
        s = @_states[s].parent
      }
      false
    }

    fire(event, payload = nil) {
      let ev = {name: event, payload: payload}
      let t = @_select(event, ev)
      return false if t == nil
      if t.enter == nil {
        # An internal transition: the action runs, the configuration stands.
        t.action(@context, ev) if t.action != nil
        return true
      }
      @_exit(t.domain, ev)
      t.action(@context, ev) if t.action != nil
      @_enter(t.enter, ev)
      true
    }

    # Whether `fire` would take a transition. Guards see the same event, so
    # they must be free of side effects for this to mean anything.
    can_fire(event, payload = nil) {
      @_select(event, {name: event, payload: payload}) != nil
    }

    # Leave the active configuration and enter the initial one again. The
    # context is the caller's data and is left alone.
    reset() {
      let ev = {name: nil, payload: nil}
      @_exit(ROOT, ev)
      @_enter(@_boot, ev)
      nil
    }

    __str__() {
      @name == ""
        ? "StateMachine(state={@_current})"
        : "StateMachine({@name}, state={@_current})"
    }

    _exit(domain, ev) {
      let mut s = @_current
      while s != domain {
        let ex = @_states[s].exit
        ex(@context, ev) if ex != nil
        s = @_states[s].parent
      }
    }

    _enter(path, ev) {
      for n in path {
        let en = @_states[n].entry
        en(@context, ev) if en != nil
      }
      @_current = path[-1]
    }

    # Walk out from the active leaf; the innermost state that both names the
    # event and passes its guard wins. A state that names it but whose guards
    # all fail has not handled it, so the search carries on to the parent.
    _select(name, ev) {
      let mut s = @_current
      while s != nil {
        let list = @_states[s].on.get(name, nil)
        if list != nil {
          for t in list {
            return t if t.guard == nil || t.guard(@context, ev)
          }
        }
        s = @_states[s].parent
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
  # A path argument that names a directory on disk: a String or a Path.
  let _disk_path = fn (who, p) {
    let t = type_of(p)
    if t != 'String' && t != 'StringView' && t != 'Path' {
      __raise('TypeError', "type error: {who} expects String|Path, got {t}")
    }
    to_string(p)
  }
  let _string_arg = fn (who, v) {
    let t = type_of(v)
    if t != 'String' && t != 'StringView' {
      __raise('TypeError', "type error: {who} expects String, got {t}")
    }
    to_string(v)
  }

  # The two kinds of IOError every Dir owes, worded once for all of them.
  let _need_file = fn (d, op, path) {
    if !d.is_file(path) {
      __raise(
        'IOError',
        d.is_dir(path)
          ? "Dir.{op}: '{path}' is a directory"
          : "Dir.{op}: no such file '{path}'",
      )
    }
  }
  let _need_dir = fn (d, op, path) {
    if !d.is_dir(path) {
      __raise(
        'IOError',
        d.is_file(path)
          ? "Dir.{op}: '{path}' is a file"
          : "Dir.{op}: no such directory '{path}'",
      )
    }
  }

  # A symlink is followed while it leads to somewhere inside the root; one
  # that leads out, or nowhere, is not there. The rule is the C++ RootDir's
  # (vfs.h), which `srv.static` serves from too; `_real` is the root's real
  # path, taken here.
  class DiskDir {
    new(path) {
      _Dir.mark(self, 'disk')
      let p = _disk_path('Dir.disk', path)
      @root = FS.abspath(p)
      if !FS.is_dir(@root) {
        __raise('IOError', "Dir.disk: no such directory '{p}'")
      }
      @_real = FS.realpath(@root)
    }
    read(path) {
      _need_file(self, 'read', path)
      _Dir.disk_read(@_real, path)
    }
    list_dir(path) {
      _need_dir(self, 'list_dir', path)
      _Dir.disk_list_dir(@_real, path)
    }
    exists(path) {
      @is_file(path) || @is_dir(path)
    }
    is_file(path) {
      _Dir.disk_is_file(@_real, path)
    }
    is_dir(path) {
      _Dir.disk_is_dir(@_real, path)
    }
    size(path) {
      _need_file(self, 'size', path)
      _Dir.disk_size(@_real, path)
    }
    files() {
      _Dir.disk_files(@_real)
    }
  }

  # The contents are read only by subscript, and which paths are files or
  # directories lives in Sets: on an Object a key named like a method
  # ('keys', 'has') would shadow it.
  class MemoryDir {
    new(files) {
      _Dir.mark(self, 'memory')
      if type_of(files) != 'Object' {
        __raise(
          'TypeError',
          "type error: Dir.memory expects Object, got {type_of(files)}",
        )
      }
      mut contents = {}
      mut paths = [].to_set()
      for key, value in files {
        if key !is String {
          __raise(
            'TypeError',
            "Dir.memory: a path is a String, got {type_of(key)}",
          )
        }
        let t = type_of(value)
        if t != 'String' && t != 'StringView' {
          __raise(
            'TypeError',
            "Dir.memory: '{key}' holds {t}, not String (a file in a subdirectory is a key like 'sub/name.txt')",
          )
        }
        let rel = _Dir.normalize(key)
        if rel == nil || rel == '' {
          __raise(
            'ValueError',
            "Dir.memory: '{key}' is not a file inside the directory",
          )
        }
        if !paths.add(rel) {
          __raise(
            'ValueError',
            "Dir.memory: '{key}' names a file another key already names",
          )
        }
        contents[rel] = to_string(value)
      }
      # Every directory a file lies under, so asking about one is a lookup.
      mut dirs = [''].to_set()
      for rel in paths {
        let parts = rel.split('/')
        for i in range(1, parts.size()) {
          let parent = parts.slice(0, i).join('/')
          if paths.contains(parent) {
            __raise(
              'ValueError',
              "Dir.memory: '{parent}' is both a file and a directory",
            )
          }
          dirs.add(parent)
        }
      }
      @contents = contents
      @_paths = paths
      @_dirs = dirs
    }
    read(path) {
      _need_file(self, 'read', path)
      @contents[_Dir.normalize(path)]
    }
    list_dir(path) {
      _need_dir(self, 'list_dir', path)
      let rel = _Dir.normalize(path)
      let prefix = rel == '' ? '' : rel + '/'
      self
        ._paths
        .iter()
        .filter(|k| k.starts_with(prefix))
        .map(|k| to_string(k.slice(prefix.size(), k.size()).split('/')[0]))
        .distinct()
        .collect()
        .sorted()
    }
    exists(path) {
      let rel = _Dir.normalize(path)
      rel != nil && (@_paths.contains(rel) || @_dirs.contains(rel))
    }
    is_file(path) {
      let rel = _Dir.normalize(path)
      rel != nil && @_paths.contains(rel)
    }
    is_dir(path) {
      let rel = _Dir.normalize(path)
      rel != nil && @_dirs.contains(rel)
    }
    files() {
      @_paths.to_array().sorted()
    }
  }

  # A directory next to the file that names it, baked into the binary by
  # `culebra build` when `name` is a string literal; read live from disk
  # otherwise. The name is held as the entry script's directory sees it (the
  # loader rebases another module's: frontend/embed_paths.h), so it crosses
  # an Isolate as it is.
  class EmbedDir {
    new(name) {
      _Dir.mark(self, 'embedded')
      @name = _string_arg('Dir.embedded', name)
    }
    read(path) {
      _need_file(self, 'read', path)
      _Dir.embedded_read(@name, path)
    }
    list_dir(path) {
      _need_dir(self, 'list_dir', path)
      _Dir.embedded_list_dir(@name, path)
    }
    exists(path) {
      @is_file(path) || @is_dir(path)
    }
    is_file(path) {
      _Dir.embedded_is_file(@name, path)
    }
    is_dir(path) {
      _Dir.embedded_is_dir(@name, path)
    }
    size(path) {
      _need_file(self, 'size', path)
      _Dir.embedded_size(@name, path)
    }
  }

  # The entries of a ZIP archive: a file, read lazily off its central
  # directory, or an archive already in memory, read in place: the String
  # stays the program's, held here as `bytes` and lent to each read (nothing
  # is copied). Both fields are there either way, one of them nil. The open
  # archive is the Runtime's (`_id` names it in the natives' table, which the
  # natives reach through `self`); sent to an Isolate, a ZipArchive opens
  # again there from its `path` or `bytes` when it is first used. `close()`
  # frees it early (`_id` becomes nil), the last reference frees it
  # otherwise, and any use after closing is a ClosedError.
  class ZipArchive {
    new(path, bytes) {
      _Dir.mark(self, 'zip')
      if (path == nil) == (bytes == nil) {
        __raise(
          'TypeError',
          "type error: Dir.zip takes a path or bytes:, one of the two",
        )
      }
      @path = nil
      @bytes = nil
      if path != nil {
        @path = FS.abspath(_disk_path('Dir.zip', path))
        @_id = _Dir.zip_open(@path)
      } else {
        let t = type_of(bytes)
        if t != 'String' && t != 'StringView' {
          __raise(
            'TypeError',
            "type error: Dir.zip bytes: expects String, got {t}",
          )
        }
        @bytes = bytes
        @_id = _Dir.zip_open_bytes(bytes)
      }
    }
    read(path) {
      _need_file(self, 'read', path)
      _Dir.zip_read(self, path)
    }
    list_dir(path) {
      _need_dir(self, 'list_dir', path)
      _Dir.zip_list_dir(self, path)
    }
    exists(path) {
      @is_file(path) || @is_dir(path)
    }
    is_file(path) {
      _Dir.zip_is_file(self, path)
    }
    is_dir(path) {
      _Dir.zip_is_dir(self, path)
    }
    size(path) {
      _need_file(self, 'size', path)
      _Dir.zip_size(self, path)
    }
    files() {
      _Dir.zip_files(self)
    }
    close() {
      _Dir.zip_close(@_id)
      @_id = nil
      @bytes = nil
    }
    drop() {
      _Dir.zip_close(@_id) if @_id != nil
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
    # What a `Dir.embedded(...)` whose name is not a literal becomes in a
    # module other than the entry script, as the module is read: the
    # directory `name` next to the module, which is at `module_dir` from the
    # entry script's directory.
    _embedded_in: fn (module_dir, name) {
      EmbedDir(module_dir + '/' + _string_arg('Dir.embedded', name))
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

  fn _find(op, at) {
    let st = _op_stacks.get(_key(op), nil)
    if st != nil {
      let n = st.size()
      return st[n - 1] if n > 0
    }
    # `at` is the original position of the `perform`, packed (carried on the
    # computation object by the transform), so the error points at the caller.
    __raise_at("EffectError", "no handler for effect '{op}'", at)
  }

  fn _full_control_error(op, at) {
    __raise_at(
      "EffectError",
      "effect '{op}' reached a full-control handler (non-tail `resume`) from outside an `effect fn`; declare the performing fn as `effect fn` so the continuation can be captured",
      at,
    )
  }

  # A fork of the frames a computation suspended in: each frame's native
  # shallow copy, then the frame's own chance to finish it. A body local some
  # closure reads lives in a box (so the closure captures the box rather than
  # the frame), and the shallow copy aliases that box — every fork would then
  # write the same one, where the scalar it replaced was copied per fork.
  # `_eff_refork` re-boxes them, so a fork's locals are its own whether or not
  # a closure reads them, and `__eff_recapture` hands the fork copies of the
  # closures it holds that still reach the boxes replaced. One list for the
  # whole stack, so a closure passed down to a delegated call follows the
  # boxes of the frame that made it.
  fn _fork(stack) {
    let frames = stack.map(|c| __eff_copy(c))
    let old_boxes = []
    let new_boxes = []
    for f in frames {
      f._eff_refork(old_boxes, new_boxes)
    }
    __eff_recapture(frames, old_boxes, new_boxes)
    frames
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
          _find(comp._eff_op, comp._eff_at)
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
          _drive(_fork(snapshot), v, ret)
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
  fn _perform_direct(op, args, at) {
    let h = _find(op, at)
    let hf = h.f
    _full_control_error(op, at) if h.t == "f"
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
            _perform_direct(c._eff_op, c._eff_args, c._eff_at)
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
    if args[0] !is String {
      __raise("TypeError", "test(name, fn): name must be a String")
    }
    if args[1] !is Function {
      __raise("TypeError", "test(name, fn): fn must be a Function")
    }
    __test_add(args[0], args[1], nil)
    return nil
  }
  if args.size() == 1 {
    let f = args[0]
    if f !is Function {
      __raise("TypeError", "test(fn): argument must be a Function")
    }
    if f.name == "" {
      __raise(
        "ValueError",
        "@test requires a named function (got anonymous); use test(\"name\", fn) for anonymous bodies",
      )
    }
    __test_add(f.name, f, nil) if !__test_has_cases(f.name)
    return f
  }
  __raise("ArityError", "test() expects 1 or 2 arguments (got {args.size()})")
}

# `@parametrize(cases)` — one entry per case, named `<fn>[i]`. A case that is
# a Tuple or an Array spreads across the parameters; anything else is a single
# argument.
let parametrize = fn (cases) {
  if cases !is Array {
    __raise("TypeError", "parametrize(cases): cases must be an Array")
  }
  fn (f) {
    if f !is Function {
      __raise(
        "TypeError",
        "parametrize(cases): the decorated value must be a Function",
      )
    }
    if f.name == "" {
      __raise("ValueError", "@parametrize requires a named function")
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

inline constexpr const char* LLM_MODULE_SOURCE = R"=culpre=(# LLM — a client for large language models, written over Http and JSON.
# docs/stdlib.md carries the reference; this header is the why.
#
# What it asks is a provider: anything with a `turn(request, on_text)` method.
# `LLM.turn` checks a request once and asks a provider one step; `LLM.chat` and
# `LLM.ask` are loops over it, and `LLM.request` sends a body as it is, so a new
# API feature does not wait for a release. `LLM.anthropic` is the Messages API
# as a provider and `LLM.openai_compatible` is Chat Completions. The names say
# what is done, not who serves it, so another format is another provider behind
# them.
#
# A history is messages of one shape whichever provider answers: parts of text,
# tool calls, tool results and reasoning, where a part a provider must have
# back as it wrote it (a thinking block with its signature) carries that
# provider's own block in `opaque`. During an `ask` the history is append-only,
# and an `ask` that fails takes back everything it added rather than leaving
# half a turn.
#
# A tool is an ordinary function. Its schema is read off its parameters'
# annotations, and the model's arguments arrive as keywords, so nothing about
# it is written twice.
let _llm_module = fn () {
  let API_VERSION = "2023-06-01"
  let DEFAULT_BASE = "https://api.anthropic.com"
  let MAX_TOKENS = 16000
  let MAX_TURNS = 10

  fn _fail(msg) {
    __raise("LLMError", msg)
  }

  # A wrong argument gets the TypeError a typed parameter raises.
  fn _type_error(name, want) {
    __raise("TypeError", "type error: parameter '{name}' expects {want}")
  }

  # A String argument as a String: a slice is copied, since JSON writes only
  # a String.
  fn _string(name, v) {
    let t = type_of(v)
    cond {
      t == "String" => v,
      t == "StringView" => to_string(v),
      _ => _type_error(name, "String"),
    }
  }

  fn _optional_string(name, v) {
    cond {
      v == nil => nil,
      v is String || v is StringView => _string(name, v),
      _ => _type_error(name, "String | Nil"),
    }
  }

  fn _long(name, v) {
    v is Long ? v : _type_error(name, "Long")
  }

  # --- tools ------------------------------------------------------------------

  # The JSON Schema for one annotation: `String | Nil` is a string that may be
  # left out, a union is any of its members, `Array<T>` says what it holds, and
  # no annotation takes any value.
  fn _schema_of(annotation) {
    let members = _union_members(annotation).filter(|m| m != "Nil")
    cond {
      members.empty() => {},
      members.size() > 1 => {anyOf: members.map(_schema_of)},
      _ => _schema_of_type(members[0]),
    }
  }

  fn _schema_of_type(t) {
    cond {
      t == "String" => {type: "string"},
      t == "Long" => {type: "integer"},
      t == "Float" => {type: "number"},
      t == "Bool" => {type: "boolean"},
      t == "Object" => {type: "object"},
      t == "Array" => {type: "array"},
      t.starts_with("Array<") && t.ends_with(">") =>
        {type: "array", items: _schema_of(t.slice(6, t.size() - 1))},
      _ => {},
    }
  }

  # The members of a top-level union; the `|` inside `Array<Long | Nil>`
  # belongs to the element type.
  fn _union_members(annotation) {
    mut out = []
    mut pending = ""
    for piece in annotation.split(" | ") {
      pending = pending.empty() ? to_string(piece) : "{pending} | {piece}"
      if pending.count("<") == pending.count(">") {
        out.push(pending) if !pending.empty()
        pending = ""
      }
    }
    out
  }

  class LLMTool {
    new(@name, @description, @input_schema, @f) {}

    # What the request names: everything but the function itself.
    spec() {
      {name: @name, description: @description,
       input_schema: @input_schema}
    }
  }

  fn tool(f, description, **params) {
    _type_error("f", "Function") if f !is Function
    let summary = _string("description", description)
    let name = f.name
    if name.empty() {
      __raise("ValueError",
        "LLM.tool: the function has no name; declare it with `fn name(...)`")
    }
    let declared = f.params.filter(|p| !p.kwargs_rest)
    for k in params.keys() {
      if !declared.any(|p| p.name == k) {
        __raise("ValueError", "LLM.tool: {name} has no parameter '{k}'")
      }
    }
    let properties = {}
    let required = []
    for p in declared {
      let schema = _schema_of(p.type)
      if params.has(p.name) {
        schema.description = _string(p.name, params[p.name])
      }
      properties[p.name] = schema
      required.push(p.name) if !p.has_default && !_union_members(p.type).contains("Nil")
    }
    let input_schema = {type: "object", properties: properties}
    input_schema.required = required if !required.empty()
    LLMTool(name, summary, input_schema, f)
  }

  # A tool whose schema is given, not read off a function's parameters: one that
  # arrives as data, or whose function takes `**args`.
  fn tool_from_schema(name, description, schema, f) {
    let id = _string("name", name)
    __raise("ValueError", "LLM.tool_from_schema: the name is empty") if id.empty()
    let summary = _string("description", description)
    _type_error("schema", "Object") if type_of(schema) != "Object"
    _type_error("f", "Function") if f !is Function
    LLMTool(id, summary, schema, f)
  }

  # What a tool's value becomes in a tool_result: a String as it is, anything
  # else as JSON.
  fn _content_of(v) {
    cond {
      v is String => v,
      v is StringView => to_string(v),
      _ => JSON.stringify(v),
    }
  }

  fn _describe(e) {
    type_of(e) == "Object" && e.has("kind") ? "{e.kind}: {e.message}" : "{e}"
  }

  # --- histories ----------------------------------------------------------------

  fn _role(m) {
    m.get("role", "")
  }

  # The parts of a message's content that are of one kind, with where they sit.
  fn _marks(m, kind) {
    mut out = []
    if m.get("content", nil) is Array {
      for p, part in m.content.enumerate() {
        if type_of(part) == "Object" && part.get("type", "") == kind {
          out.push({part: p, id: part.get("id", nil)})
        }
      }
    }
    out
  }

  # Every tool_call or tool_result part that has no partner. An assistant
  # message's calls are answered by the run of `tool` messages that follows it
  # directly: each call needs exactly one result in that run, and each result
  # needs a call of that message. A run of `tool` messages that no assistant
  # message heads answers nothing.
  fn _orphans(messages) {
    mut out = []
    mut i = 0
    while i < messages.size() {
      let role = _role(messages[i])
      if role != "assistant" && role != "tool" {
        i += 1
        continue
      }
      let calls = role == "assistant" ? _marks(messages[i], "tool_call") : []
      let lo = role == "assistant" ? i + 1 : i
      mut hi = lo
      while hi < messages.size() && _role(messages[hi]) == "tool" {
        hi += 1
      }
      mut results = []
      for k in lo..hi {
        for r in _marks(messages[k], "tool_result") {
          results.push({message: k, part: r.part, id: r.id})
        }
      }
      let asked = calls.map(|c| c.id).filter(|id| id != nil).to_set()
      let answered = results.map(|r| r.id).filter(|id| id != nil).to_set()
      let matched = asked.intersect(answered)
      for n, c in calls.enumerate() {
        let again = calls.slice(0, n).any(|e| e.id == c.id)
        if !matched.contains(c.id) || again {
          out.push({message: i, part: c.part, kind: "tool_call", id: c.id})
        }
      }
      for n, r in results.enumerate() {
        let again = results.slice(0, n).any(|e| e.id == r.id)
        if !matched.contains(r.id) || again {
          out.push({message: r.message, part: r.part, kind: "tool_result", id: r.id})
        }
      }
      i = hi
    }
    out
  }

  # The history without the tool calls and results that have no partner, and
  # without the messages that leaves empty. Nothing is added to it.
  fn repair(messages) {
    _type_error("messages", "Array") if messages !is Array
    for i, m in messages.enumerate() {
      if type_of(m) != "Object" {
        __raise("TypeError", "type error: messages[{i}] expects Object")
      }
    }
    let orphans = _orphans(messages)
    mut out = []
    for i, m in messages.enumerate() {
      let gone = orphans.filter(|o| o.message == i).map(|o| o.part)
      if gone.empty() {
        out.push(m)
        continue
      }
      mut kept = []
      for p, part in m.content.enumerate() {
        kept.push(part) if !gone.contains(p)
      }
      out.push({...m, content: kept}) if !kept.empty()
    }
    out
  }

  # --- turn ---------------------------------------------------------------------

  let ROLES = ["system", "user", "assistant", "tool"]

  # The roles a part may sit in.
  let PART_ROLES = {
    text: ["system", "user", "assistant"],
    tool_call: ["assistant"],
    tool_result: ["tool"],
    reasoning: ["assistant"],
    other: ["assistant"],
  }

  fn _check_message(who, i, m) {
    if type_of(m) != "Object" {
      __raise("TypeError", "type error: messages[{i}] expects Object")
    }
    let role = m.get("role", nil)
    if role !is String {
      __raise("TypeError", "type error: messages[{i}].role expects String")
    }
    if !ROLES.contains(role) {
      __raise("ValueError", "{who}: messages[{i}]: unknown role '{role}'")
    }
    let content = m.get("content", nil)
    if content is String {
      if role == "tool" {
        __raise("ValueError",
          "{who}: messages[{i}]: a tool message holds tool_result parts, not a String")
      }
      return nil
    }
    if content !is Array {
      __raise("TypeError", "type error: messages[{i}].content expects String | Array")
    }
    for j, part in content.enumerate() {
      if type_of(part) != "Object" {
        __raise("TypeError", "type error: messages[{i}].content[{j}] expects Object")
      }
      let kind = part.get("type", nil)
      if kind !is String {
        __raise("TypeError", "type error: messages[{i}].content[{j}].type expects String")
      }
      let roles = PART_ROLES.get(kind, nil)
      if roles == nil {
        __raise("ValueError",
          "{who}: messages[{i}].content[{j}]: unknown part type '{kind}'")
      }
      if !roles.contains(role) {
        __raise("ValueError",
          "{who}: messages[{i}].content[{j}]: a {kind} part is not allowed in a {role} message")
      }
    }
  }

  fn _check_pairing(who, messages) {
    let orphans = _orphans(messages)
    if !orphans.empty() {
      let o = orphans[0]
      let id = o.id == nil ? "without an id" : "'{o.id}'"
      let partner = o.kind == "tool_call"
        ? "a tool_result just after it"
        : "a tool_call just before it"
      _fail("{who}: messages[{o.message}]: the {o.kind} {id} is not paired with {partner} (see LLM.repair)")
    }
  }

  fn _system_text(content) {
    content is String ? content : content.map(|p| p.text).join("")
  }

  # The system prompt, and the messages without it. It is given as system: or as
  # the first message, once.
  fn _lift_system(who, system, messages) {
    let lead = !messages.empty() && messages[0].role == "system"
    let rest = lead ? messages.slice(1, messages.size()) : messages
    if rest.any(|m| m.role == "system") {
      _fail("{who}: a system message must be the first message, and only once")
    }
    if lead && system != nil {
      _fail("{who}: the system prompt is given twice: as system: and as a system message")
    }
    (system ?? (lead ? _system_text(messages[0].content) : nil), rest)
  }

  fn _is_provider(p) {
    try {
      p.turn is Function
    } catch _e {
      false
    }
  }

  fn _check_provider(p) {
    if !_is_provider(p) {
      _type_error("provider", "a provider (an object with a turn method)")
    }
  }

  fn _provider_name(p) {
    to_string(p.name ?? "provider")
  }

  # The model a call names, or else the provider's own.
  fn _model_for(provider, model) {
    let id = _optional_string("model", model) ?? provider.default_model
    _fail("LLM: model: is required for {_provider_name(provider)}") if id == nil
    id
  }

  # The request a provider is asked, checked once here so that a provider sees
  # one that is whole. `who` names the call in what is refused.
  fn _request_for(who, provider, messages, model, system, tools, max_tokens, stop,
                  temperature, extra) {
    _check_provider(provider)
    let name = _provider_name(provider)
    let caps = provider.capabilities ?? {}

    let model_id = _model_for(provider, model)
    let prompt = _optional_string("system", system)
    _type_error("messages", "Array") if messages !is Array
    _type_error("tools", "Array") if tools !is Array
    for t in tools {
      if type_of(t) != "LLMTool" {
        __raise("TypeError", "{who}: tools takes what LLM.tool makes, got {type_of(t)}")
      }
    }
    if max_tokens != nil && max_tokens !is Long {
      _type_error("max_tokens", "Long | Nil")
    }
    if stop != nil && (stop !is Array || stop.any(|s| s !is String)) {
      _type_error("stop", "Array<String> | Nil")
    }
    if temperature != nil && temperature !is Long && temperature !is Float {
      _type_error("temperature", "Float | Nil")
    }
    if extra != nil && type_of(extra) != "Object" {
      _type_error("extra", "Object | Nil")
    }
    if !tools.empty() && !caps.get("tools", false) {
      _fail("{who}: {name} does not take tools")
    }
    if extra != nil && !caps.get("extra", false) {
      _fail("{who}: {name} does not take extra:")
    }

    for i, m in messages.enumerate() {
      _check_message(who, i, m)
    }
    _check_pairing(who, messages)
    let (lifted, rest) = _lift_system(who, prompt, messages)
    _fail("{who}: no messages") if rest.empty()

    {model: model_id, system: lifted, messages: rest, tools: tools, max_tokens: max_tokens,
     stop: stop, temperature: temperature, extra: extra}
  }

  # One step: what the provider is asked is checked once, so a provider sees a
  # request that is whole, and runs no tool. A provider is anything with a
  # `turn(request, on_text)` method, `name`, `capabilities` and `default_model`
  # fields.
  fn turn(provider, *, messages, model = nil, system = nil, tools = [], max_tokens = nil,
          stop = nil, temperature = nil, extra = nil, on_text = nil) {
    if on_text != nil && on_text !is Function {
      _type_error("on_text", "Function | Nil")
    }
    let req = _request_for("LLM.turn", provider, messages, model, system, tools, max_tokens,
                           stop, temperature, extra)
    let reply = provider.turn(req, on_text)
    if type_of(reply) != "Object" {
      _fail("LLM.turn: {_provider_name(provider)} answered with a {type_of(reply)}, not an Object")
    }
    reply
  }

  # How many tokens the input of a turn comes to, as the provider counts them,
  # for a provider that can: that is a request to it each time, and it is not
  # asked to answer.
  fn count_tokens(provider, *, messages, model = nil, system = nil, tools = []) {
    _check_provider(provider)
    let name = _provider_name(provider)
    let caps = provider.capabilities ?? {}
    if !caps.get("count_tokens", false) || provider.count_tokens !is Function {
      _fail("LLM.count_tokens: {name} does not count tokens")
    }
    let req = _request_for("LLM.count_tokens", provider, messages, model, system, tools, nil,
                           nil, nil, nil)
    let count = provider.count_tokens(req)
    if count !is Long {
      _fail("LLM.count_tokens: {name} answered with a {type_of(count)}, not a Long")
    }
    count
  }

  # --- the wire -----------------------------------------------------------------

  # The key and the server of a hosted service: what the call gives, else the
  # service's variables, else its own server.
  fn _credentials(key_var, base_var, default_base, api_key, base_url) {
    let key = (_optional_string("api_key", api_key) ?? Sys.env(key_var)).presence()
    _fail("LLM: no API key: set {key_var} or pass api_key:") if key == nil
    (key, _optional_string("base_url", base_url) ?? Sys.env(base_var).presence() ?? default_base)
  }

  fn _connect(api_key, base_url, timeout = nil) {
    let (key, base) = _credentials("ANTHROPIC_API_KEY", "ANTHROPIC_BASE_URL", DEFAULT_BASE,
                                   api_key, base_url)
    let headers = {"x-api-key": key, "anthropic-version": API_VERSION}
    timeout == nil ? Http.client(base, headers:)
                   : Http.client(base, headers:, timeout:)
  }

  # A failed request explains itself in its body, `{error: {type, message}}`
  # in this format; a gateway's page that is not JSON is quoted as it is.
  fn _api_error(res, detail_of) {
    let parsed = try {
      JSON.parse(res.body)
    } catch _e: ValueError {
      nil
    }
    let err = type_of(parsed) == "Object" ? parsed.get("error", nil) : nil
    _fail("LLM: {res.status} {detail_of(err) ?? res.body.truncate(500)}")
  }

  fn _anthropic_detail(err) {
    type_of(err) == "Object" ? "{err.get('type', 'error')}: {err.get('message', '')}" : nil
  }

  fn _send(http, body, headers = nil, path = "/v1/messages", detail_of = _anthropic_detail) {
    let res = http.post(path, json: body, headers:)
    _api_error(res, detail_of) unless res.ok
    res.json()
  }

  # A streamed message, put back together into the body the non-streaming
  # request would have answered with, so one reader serves both. Pieces are
  # collected and joined once, not appended to a growing String.
  class LLMStream {
    new(@on_text) {
      @events = 0
      @message = nil
      @blocks = []
      @pieces = []
      @failure = nil
    }

    take(e) {
      @events += 1
      let ev = try {
        JSON.parse(e.data)
      } catch _e: ValueError {
        nil
      }
      return nil if type_of(ev) != "Object"
      let kind = ev.get("type", "")
      if kind == "message_start" {
        @message = ev.message
      } else if kind == "content_block_start" {
        while @blocks.size() <= ev.index {
          @blocks.push(nil)
          @pieces.push([])
        }
        @blocks[ev.index] = ev.content_block
      } else if kind == "content_block_delta" {
        @grow(ev.index, ev.delta)
      } else if kind == "content_block_stop" {
        @close(ev.index)
      } else if kind == "message_delta" {
        # A parsed Object's fields are fixed, so each change is a new Object.
        @message = {...@message,
          stop_reason: ev.delta.get("stop_reason", nil),
          stop_sequence: ev.delta.get("stop_sequence", nil),
          usage: {...@message.get("usage", {}), ...ev.get("usage", {})}}
      } else if kind == "error" {
        @failure = ev.get("error", {})
      }
    }

    grow(i, d) {
      let kind = d.get("type", "")
      if kind == "text_delta" {
        @pieces[i].push(d.text)
        @on_text(d.text) if @on_text != nil
      } else if kind == "thinking_delta" {
        @pieces[i].push(d.thinking)
      } else if kind == "input_json_delta" {
        @pieces[i].push(d.partial_json)
      } else if kind == "signature_delta" {
        @blocks[i] = {...@blocks[i], signature: d.signature}
      } else if kind == "citations_delta" {
        let b = @blocks[i]
        @blocks[i] = {...b, citations: [...b.get("citations", []), d.citation]}
      }
    }

    # A block's pieces become its field once it is whole. A tool's arguments
    # are one JSON document written a few characters at a time; one cut off
    # mid-document stays empty, and the stop reason says why.
    close(i) {
      let b = @blocks[i]
      let joined = @pieces[i].join("")
      let kind = b.get("type", "")
      if kind == "text" {
        @blocks[i] = {...b, text: joined}
      } else if kind == "thinking" {
        @blocks[i] = {...b, thinking: joined}
      } else if b.has("input") && !joined.empty() {
        let input = try {
          JSON.parse(joined)
        } catch _e: ValueError {
          {}
        }
        @blocks[i] = {...b, input: input}
      }
    }

    finish() {
      if @failure != nil {
        _fail("LLM: {@failure.get('type', 'error')}: {@failure.get('message', '')}")
      }
      _fail("LLM: the stream ended before the message began") if @message == nil
      {...@message, content: @blocks}
    }
  }

  fn _text_of(message) {
    message.content.filter(|b| b.get("type", "") == "text").map(|b| b.text).join("")
  }

  # A streamed request: `assembly` takes the events and puts the message back
  # together. A 2xx that did not stream is the whole answer, as a server that
  # ignores `stream` sends it; `text_of` reads its text for `on_text`.
  fn _stream(http, path, body, assembly, text_of, detail_of, on_text) {
    let res = http.post(path, json: body, events: |e| assembly.take(e))
    _api_error(res, detail_of) unless res.ok
    if assembly.events == 0 {
      _fail("LLM: {res.status}: the stream closed without an event") if res.body.empty()
      let message = res.json()
      on_text(text_of(message))
      return message
    }
    assembly.finish()
  }

  # --- the Messages API as a provider ---------------------------------------------

  # A part kept as the block it came from, by the format that made it.
  fn _kept(part) {
    part.get("format", nil) == "anthropic" && part.has("opaque")
  }

  # The block for a part of an assistant or user message, or nil when this
  # format has no place for it.
  fn _block_of(part) {
    if _kept(part) {
      return part.opaque
    }
    let kind = part.type
    cond {
      kind == "text" => {type: "text", text: part.text},
      kind == "tool_call" => {type: "tool_use", id: part.id, name: part.name,
                              input: part.get("args", {})},
      _ => nil,
    }
  }

  fn _result_block(part) {
    {type: "tool_result", tool_use_id: part.id, content: part.get("content", ""),
     is_error: part.get("is_error", false)}
  }

  # The messages the API takes. Consecutive tool messages are one user message
  # of tool_result blocks, and a message none of whose parts has a place here is
  # left out.
  fn _wire_messages(messages) {
    mut out = []
    mut after_tool = false
    for m in messages {
      if m.role == "tool" {
        let blocks = m.content.map(_result_block)
        if after_tool {
          let last = out[out.size() - 1]
          out[out.size() - 1] = {...last, content: last.content + blocks}
        } else {
          out.push({role: "user", content: blocks})
        }
        after_tool = true
        continue
      }
      after_tool = false
      if m.content is String {
        out.push({role: m.role, content: m.content})
      } else {
        let blocks = m.content.map(_block_of).filter(|b| b != nil)
        out.push({role: m.role, content: blocks}) if !blocks.empty()
      }
    }
    out
  }

  # The request body, in the order the API's own examples give it.
  fn _body_of(req) {
    mut body = {model: req.model, max_tokens: req.max_tokens ?? MAX_TOKENS,
                messages: _wire_messages(req.messages)}
    body.system = req.system if req.system != nil
    body.tools = req.tools.map(|t| t.spec()) if !req.tools.empty()
    body.stop_sequences = req.stop if req.stop != nil
    body.temperature = req.temperature if req.temperature != nil
    req.extra == nil ? body : {...body, ...req.extra}
  }

  # What counting the input asks of the API: the request without what only an
  # answer needs.
  fn _count_body_of(req) {
    mut body = {model: req.model, messages: _wire_messages(req.messages)}
    body.system = req.system if req.system != nil
    body.tools = req.tools.map(|t| t.spec()) if !req.tools.empty()
    body
  }

  # Whether a block says nothing a plain part would lose.
  fn _plain(block, keys) {
    block.keys().all(|k| keys.contains(k))
  }

  fn _part_of(b) {
    let kind = b.get("type", "")
    cond {
      kind == "text" && _plain(b, ["type", "text"]) => {type: "text", text: b.text},
      kind == "text" => {type: "text", text: b.get("text", ""), format: "anthropic", opaque: b},
      kind == "tool_use" && _plain(b, ["type", "id", "name", "input"]) =>
        {type: "tool_call", id: b.id, name: b.name, args: b.get("input", {})},
      kind == "tool_use" => {type: "tool_call", id: b.id, name: b.name, args: b.get("input", {}),
                             format: "anthropic", opaque: b},
      kind == "thinking" => {type: "reasoning", text: b.get("thinking", ""),
                             format: "anthropic", opaque: b},
      kind == "redacted_thinking" => {type: "reasoning", text: "", format: "anthropic", opaque: b},
      _ => {type: "other", format: "anthropic", opaque: b},
    }
  }

  # A call as `reply.tool_calls` lists it; one that could not be read says why.
  fn _call_of(p) {
    let call = {id: p.id, name: p.name, args: p.args}
    p.has("error") ? {...call, error: p.error} : call
  }

  # The reply a provider gives, whatever its format: `text`, `tool_calls` and
  # `reasoning` are read off the parts, so they cannot disagree with `message`.
  fn _reply_from(parts, stop, usage, raw) {
    {
      message: {role: "assistant", content: parts},
      text: parts.filter(|p| p.type == "text").map(|p| p.text).join(""),
      tool_calls: parts.filter(|p| p.type == "tool_call").map(_call_of),
      reasoning: parts.filter(|p| p.type == "reasoning").map(|p| p.text).join(""),
      stop: stop,
      usage: usage,
      raw: raw,
    }
  }

  let STOPS = {end_turn: "end", stop_sequence: "end", tool_use: "tool_calls",
               max_tokens: "length", refusal: "refusal", pause_turn: "pause"}

  # The API's answer, whole or put together from a stream, as the reply a
  # provider gives. A stop reason it does not know is nil; `raw` has it.
  fn _reply_of(message) {
    let u = message.get("usage", nil)
    _reply_from(message.get("content", []).map(_part_of),
                STOPS.get(message.get("stop_reason", ""), nil),
                u == nil ? nil : {input_tokens: u.get("input_tokens", 0),
                                  output_tokens: u.get("output_tokens", 0)},
                message)
  }

  class LLMAnthropic {
    new(@http) {
      @name = "anthropic"
      @capabilities = {tools: true, extra: true, count_tokens: true}
      @default_model = nil
    }

    turn(req, on_text) {
      let body = _body_of(req)
      _reply_of(on_text == nil ? _send(@http, body)
        : _stream(@http, "/v1/messages", {...body, stream: true}, LLMStream(on_text),
                  _text_of, _anthropic_detail, on_text))
    }

    # The body as it is, posted to /v1/messages, and the answer as it is.
    request(body, headers) {
      _send(@http, body, headers)
    }

    # The input tokens of the request, as /v1/messages/count_tokens counts them.
    count_tokens(req) {
      _send(@http, _count_body_of(req), nil, "/v1/messages/count_tokens").input_tokens
    }
  }

  fn anthropic(*, api_key = nil, base_url = nil, timeout = 120) {
    LLMAnthropic(_connect(api_key, base_url, _long("timeout", timeout)))
  }

  # --- Chat Completions as a provider ---------------------------------------------

  # A failed request explains itself in `{error: {message, type, code}}`; some
  # servers send `{error: "text"}`, and a gateway's page that is not JSON is
  # quoted as it is.
  fn _cc_detail(err) {
    cond {
      type_of(err) == "Object" =>
        err.get("type", nil) == nil ? "{err.get('message', '')}"
                                    : "{err.type}: {err.get('message', '')}",
      err == nil => nil,
      _ => to_string(err),
    }
  }

  # A streamed completion, put back together into the body the plain request
  # would have answered with. A call is known by its index; its id and name
  # come with the first piece, its arguments over the rest.
  class LLMChatStream {
    new(@on_text) {
      @events = 0
      @pieces = []
      @reasoning = []
      @calls = []
      @stopped = nil
      @usage = nil
      @failure = nil
    }

    take(e) {
      @events += 1
      let ev = try {
        JSON.parse(e.data)
      } catch _e: ValueError {
        nil
      }
      return nil if type_of(ev) != "Object"
      if ev.get("error", nil) != nil {
        @failure = ev.error
        return nil
      }
      @usage = ev.usage if ev.get("usage", nil) != nil
      let choices = ev.get("choices", [])
      return nil if choices.empty()
      let choice = choices[0]
      @stopped = choice.finish_reason if choice.get("finish_reason", nil) != nil
      @grow(choice.get("delta", {}))
    }

    grow(d) {
      let said = d.get("content", nil) ?? ""
      if !said.empty() {
        @pieces.push(said)
        @on_text(said) if @on_text != nil
      }
      let thought = d.get("reasoning_content", nil) ?? d.get("reasoning", nil) ?? ""
      @reasoning.push(thought) if !thought.empty()
      for c in d.get("tool_calls", []) {
        let i = c.get("index", 0)
        while @calls.size() <= i {
          @calls.push({mut id: nil, mut name: "", mut pieces: []})
        }
        let f = c.get("function", {})
        let call = @calls[i]
        call.id = c.get("id", nil) ?? call.id
        call.name = f.get("name", nil) ?? call.name
        call.pieces.push(f.get("arguments", nil) ?? "")
      }
    }

    finish() {
      if @failure != nil {
        _fail("LLM: {_cc_detail(@failure)}")
      }
      let calls = @calls.map(|c|
        {id: c.id, type: "function", function: {name: c.name, arguments: c.pieces.join("")}})
      mut message = {role: "assistant", content: @pieces.join("")}
      message.reasoning_content = @reasoning.join("") if !@reasoning.empty()
      message.tool_calls = calls if !calls.empty()
      {choices: [{message: message,
                  finish_reason: @stopped}],
       usage: @usage}
    }
  }

  fn _cc_message(body) {
    let choices = body.get("choices", [])
    choices.empty() ? {} : choices[0].get("message", {})
  }

  fn _cc_stream(http, body, on_text) {
    _stream(http, "/chat/completions",
            {...body, stream: true, stream_options: {include_usage: true}},
            LLMChatStream(on_text), |whole| _cc_message(whole).get("content", nil) ?? "",
            _cc_detail, on_text)
  }

  # The tools of a request, as this format names them.
  fn _cc_tool(t) {
    {type: "function", function: {name: t.name, description: t.description,
                                  parameters: t.input_schema}}
  }

  fn _cc_text_of(content) {
    content is String ? content
      : content.filter(|p| p.type == "text").map(|p| p.text).join("")
  }

  fn _cc_call(p) {
    {id: p.id, type: "function",
     function: {name: p.name, arguments: p.get("raw", nil) ?? JSON.stringify(p.get("args", {}))}}
  }

  # The messages the API takes. A system prompt is the first message, each
  # tool result is a message of its own, and what has no place here (reasoning,
  # another format's blocks) is left out; a message left with nothing is too.
  fn _cc_messages(system, messages) {
    mut out = system == nil ? [] : [{role: "system", content: system}]
    for m in messages {
      if m.role == "tool" {
        for r in m.content {
          out.push({role: "tool", tool_call_id: r.id, content: r.get("content", "")})
        }
        continue
      }
      let plain = m.content is String
      let msg = {role: m.role, content: _cc_text_of(m.content)}
      let calls = plain ? [] : m.content.filter(|p| p.type == "tool_call").map(_cc_call)
      msg.tool_calls = calls if !calls.empty()
      out.push(msg) if plain || !msg.content.empty() || !calls.empty()
    }
    out
  }

  # The request body; the ceiling goes by the name this server knows it by.
  fn _cc_body_of(req, max_tokens_field) {
    mut body = {model: req.model, messages: _cc_messages(req.system, req.messages)}
    body[max_tokens_field] = req.max_tokens if req.max_tokens != nil
    body.tools = req.tools.map(_cc_tool) if !req.tools.empty()
    body.stop = req.stop if req.stop != nil
    body.temperature = req.temperature if req.temperature != nil
    req.extra == nil ? body : {...body, ...req.extra}
  }

  let CC_STOPS = {stop: "end", tool_calls: "tool_calls", length: "length",
                  content_filter: "refusal"}

  # The part for one call. Its arguments are a JSON document in a String; text
  # that is not an Object does not fail the reply. The call keeps what the model
  # wrote (`raw`) and why it cannot be read (`error`), and `run_tools` tells the
  # model rather than run it.
  fn _cc_call_part(n, c) {
    let text = c.function.get("arguments", nil) ?? ""
    let part = {type: "tool_call", id: c.get("id", nil) ?? "call_{n + 1}", name: c.function.name,
                args: {}}
    return part if text.empty()
    let parsed = try {
      {value: JSON.parse(text)}
    } catch _e: ValueError {
      nil
    }
    let unread = |why| {...part, raw: text, error: why}
    cond {
      parsed == nil => unread("the arguments are not JSON"),
      type_of(parsed.value) != "Object" => unread("the arguments are not a JSON Object"),
      _ => {...part, args: parsed.value},
    }
  }

  # The API's answer, whole or put together from a stream, as the reply a
  # provider gives. A failure can arrive inside a 200. A call with no id gets
  # one, for the result to name.
  fn _cc_reply_of(body) {
    if body.get("error", nil) != nil {
      _fail("LLM: {_cc_detail(body.error)}")
    }
    let choices = body.get("choices", [])
    _fail("LLM: the API answered with no choices") if choices.empty()
    let choice = choices[0]
    if choice.get("error", nil) != nil {
      _fail("LLM: {_cc_detail(choice.error)}")
    }
    let m = choice.get("message", {})
    let said = m.get("content", nil) ?? ""
    let thought = m.get("reasoning_content", nil) ?? m.get("reasoning", nil) ?? ""
    mut calls = []
    for n, c in m.get("tool_calls", []).enumerate() {
      calls.push(_cc_call_part(n, c))
    }
    mut parts = []
    parts.push({type: "reasoning", text: thought}) if !thought.empty()
    parts.push({type: "text", text: said}) if !said.empty()
    parts += calls
    let u = body.get("usage", nil)
    _reply_from(parts, CC_STOPS.get(choice.get("finish_reason", ""), nil),
                u == nil ? nil : {input_tokens: u.get("prompt_tokens", 0),
                                  output_tokens: u.get("completion_tokens", 0)},
                body)
  }

  class LLMChatCompletions {
    new(@http, @name, @base_url, @max_tokens_field) {
      @capabilities = {tools: true, extra: true}
      @default_model = nil
    }

    turn(req, on_text) {
      let body = _cc_body_of(req, @max_tokens_field)
      _cc_reply_of(on_text == nil ? _send(@http, body, nil, "/chat/completions", _cc_detail)
                                  : _cc_stream(@http, body, on_text))
    }

    # The body as it is, posted to /chat/completions, and the answer as it is.
    request(body, headers) {
      _send(@http, body, headers, "/chat/completions", _cc_detail)
    }
  }

  # Any server that speaks Chat Completions. The key, when there is one, goes as
  # a bearer token; `max_tokens_field` is the name this server knows the
  # ceiling by.
  fn openai_compatible(*, base_url, api_key = nil, name = "openai_compatible",
                       max_tokens_field = "max_tokens", timeout = 120) {
    let base = _string("base_url", base_url)
    let key = _optional_string("api_key", api_key)
    let headers = key == nil ? {} : {Authorization: "Bearer {key}"}
    LLMChatCompletions(Http.client(base, headers:, timeout: _long("timeout", timeout)),
                       _string("name", name), base, _string("max_tokens_field", max_tokens_field))
  }

  # A hosted service on Chat Completions: the key from its variable unless one
  # is given, the server from its variable unless one is given.
  fn _hosted(who, key_var, base_var, default_base, max_tokens_field, api_key, base_url, timeout) {
    let (key, base) = _credentials(key_var, base_var, default_base, api_key, base_url)
    openai_compatible(base_url: base, api_key: key, name: who,
                      max_tokens_field:, timeout:)
  }

  fn openai(*, api_key = nil, base_url = nil, timeout = 120) {
    _hosted("openai", "OPENAI_API_KEY", "OPENAI_BASE_URL", "https://api.openai.com/v1",
            "max_completion_tokens", api_key, base_url, timeout)
  }

  fn openrouter(*, api_key = nil, base_url = nil, timeout = 120) {
    _hosted("openrouter", "OPENROUTER_API_KEY", "OPENROUTER_BASE_URL",
            "https://openrouter.ai/api/v1", "max_tokens", api_key, base_url, timeout)
  }

  # Ollama on this machine, which asks for no key.
  fn ollama(*, base_url = "http://localhost:11434/v1", timeout = 120) {
    openai_compatible(base_url:, name: "ollama", timeout:)
  }

  # --- a model run in this process ----------------------------------------------

  # What the model sees of a message: its text. Reasoning, and blocks that belong
  # to another format, are left out (a chat template has no place for them); a
  # tool call or result is refused, since a local model takes no tools.
  fn _local_text(m) {
    return m.content if m.content is String
    mut out = []
    for p in m.content {
      if p.type == "text" {
        out.push(p.text)
      } else if p.type == "tool_call" || p.type == "tool_result" {
        _fail("LLM: local does not take tool calls or results")
      }
    }
    out.join("")
  }

  fn _local_messages(req) {
    let msgs = req.messages.map(fn (m) { {role: m.role, content: _local_text(m)} })
    req.system == nil ? msgs : [{role: "system", content: req.system}, ...msgs]
  }

  let LOCAL_STOPS = {"end": "end", "stop": "end", "length": "length"}

  # The engine's result as the reply a provider gives. A generation that was
  # aborted has no stop reason.
  fn _local_reply(r) {
    _reply_from(r.text.empty() ? [] : [{type: "text", text: r.text}],
                LOCAL_STOPS.get(r.finish, nil),
                {input_tokens: r.prompt_tokens, output_tokens: r.completion_tokens}, r)
  }

  class LLMLocal {
    new(@engine) {
      self.name = "local"
      self.capabilities = {tools: false, extra: false, count_tokens: true}
      self.default_model = "local"
    }

    turn(req, on_text) {
      _local_reply(@engine.chat(_local_messages(req), max_tokens: req.max_tokens,
                                    stop: req.stop, temperature: req.temperature,
                                    cache: true, on_text:))
    }

    count_tokens(req) {
      @engine.count_tokens(@engine.format_chat(_local_messages(req)))
    }
  }

  # An engine is a loaded model: `Llama.load` makes one, and so can anything with
  # the same three methods.
  fn _is_engine(e) {
    try {
      e.chat is Function && e.format_chat is Function && e.count_tokens is Function
    } catch _e {
      false
    }
  }

  fn local(engine) {
    if !_is_engine(engine) {
      __raise("TypeError",
        "type error: parameter 'engine' expects an engine (what Llama.load returns)")
    }
    LLMLocal(engine)
  }

  # --- running the tools a model asks for ---------------------------------------

  # One call, answered. What a tool throws goes back to the model as an error it
  # can correct, and an interrupt is the user's, and goes on up. `approve` is
  # asked before a tool that exists is run: true runs it, false or a String (the
  # reason) does not. What it throws goes up, and it is not the model's to be
  # told of.
  fn _answer(tools, call, approve) {
    let chosen = tools.find(|t| t.name == call.name)
    let answer = if chosen == nil {
      {content: "no tool named '{call.name}'", is_error: true}
    } else if call.get("error", nil) != nil {
      {content: "the call was not run: {call.error}", is_error: true}
    } else {
      let verdict = approve == nil ? true : approve(call)
      cond {
        verdict == true => try {
          {content: _content_of(chosen.f(**call.get("args", {}))), is_error: false}
        } catch e: Interrupted {
          throw e
        } catch e {
          {content: _describe(e), is_error: true}
        },
        verdict == false => {content: "the call was not approved", is_error: true},
        verdict is String || verdict is StringView =>
          {content: to_string(verdict), is_error: true},
        _ => __raise("TypeError",
               "LLM: approve: must answer true, false or a String, not {type_of(verdict)}"),
      }
    }
    {type: "tool_result", id: call.id, ...answer}
  }

  # The tool message that answers calls, ready for a history.
  fn run_tools(tools, calls, *, approve = nil) {
    _type_error("tools", "Array") if tools !is Array
    for t in tools {
      if type_of(t) != "LLMTool" {
        __raise("TypeError", "LLM.run_tools: tools takes what LLM.tool makes, got {type_of(t)}")
      }
    }
    _type_error("calls", "Array") if calls !is Array
    if approve != nil && approve !is Function {
      _type_error("approve", "Function | Nil")
    }
    for i, c in calls.enumerate() {
      if type_of(c) != "Object" {
        __raise("TypeError", "type error: calls[{i}] expects Object")
      }
      if c.get("name", nil) !is String {
        __raise("TypeError", "type error: calls[{i}].name expects String")
      }
      if type_of(c.get("args", {})) != "Object" {
        __raise("TypeError", "type error: calls[{i}].args expects Object")
      }
      if c.get("error", nil) != nil && type_of(c.error) != "String" {
        __raise("TypeError", "type error: calls[{i}].error expects String")
      }
    }
    _fail("LLM.run_tools: no calls") if calls.empty()
    {role: "tool", content: calls.map(|c| _answer(tools, c, approve))}
  }

  # --- conversations ------------------------------------------------------------

  class LLMChat {
    new(@provider, @model, @system, @tools, @max_tokens, @max_turns, @approve) {
      @messages = []
      @last = nil
    }

    # The answer's text. The tools the model asks for run on the way there, up
    # to `max_turns` round trips; `last` holds the final reply whole.
    ask(prompt, on_text = nil) {
      let content = _string("prompt", prompt)
      let start = @messages.size()
      try {
        @messages.push({role: "user", content: content})
        for _step in 0..@max_turns {
          let reply = turn(@provider, model: @model, messages: @messages,
                           system: @system, tools: @tools,
                           max_tokens: @max_tokens, on_text:)
          @last = reply
          if reply.stop == "refusal" {
            # The Messages API says why in stop_details.
            let raw = reply.raw
            let details = type_of(raw) == "Object" ? raw.get("stop_details", nil) : nil
            let why = type_of(details) == "Object"
              ? details.get("explanation", nil) ?? details.get("category", nil)
              : nil
            _fail("LLM: the model declined to answer" + (why == nil ? "" : ": {why}"))
          }
          @messages.push(reply.message)
          if reply.stop == "length" && !reply.tool_calls.empty() {
            _fail("LLM: a tool call was cut off at max_tokens")
          }
          if !reply.tool_calls.empty() {
            @messages.push(run_tools(@tools, reply.tool_calls, approve: @approve))
          } else if reply.stop != "pause" {
            return reply.text
          }
        }
        _fail("LLM: no answer within max_turns ({@max_turns})")
      } catch e {
        # Take back this ask's turns, so the history stays one a provider accepts.
        @messages = @messages.slice(0, start)
        throw e
      }
    }
  }

  fn chat(*, provider, model = nil, system = nil, tools = [], max_tokens = nil,
          max_turns = MAX_TURNS, approve = nil) {
    _check_provider(provider)
    let model_id = _model_for(provider, model)
    let instructions = _optional_string("system", system)
    _type_error("tools", "Array") if tools !is Array
    for t in tools {
      if type_of(t) != "LLMTool" {
        __raise("TypeError",
          "LLM.chat: tools takes what LLM.tool makes, got {type_of(t)}")
      }
    }
    let ceiling = max_tokens == nil ? nil : _long("max_tokens", max_tokens)
    if approve != nil && approve !is Function {
      _type_error("approve", "Function | Nil")
    }
    LLMChat(provider, model_id, instructions, tools, ceiling, _long("max_turns", max_turns),
            approve)
  }

  fn ask(prompt, *, provider, model = nil, system = nil, max_tokens = nil, on_text = nil) {
    chat(provider:, model:, system:, max_tokens:).ask(prompt, on_text:)
  }

  # The body as it is, the response as it is: a failure is still an LLMError.
  # `headers` carries what a body cannot, such as a beta's `anthropic-beta`. A
  # provider takes this with a `request(body, headers)` method of its own; what
  # a body says is its format's, so it is only as portable as the provider.
  fn request(body, *, provider, headers = nil) {
    _check_provider(provider)
    _type_error("body", "Object") if type_of(body) != "Object"
    _type_error("headers", "Object | Nil") if headers != nil && type_of(headers) != "Object"
    let name = _provider_name(provider)
    if provider.request !is Function {
      _fail("LLM.request: {name} does not take a request as it is")
    }
    provider.request(body, headers)
  }

  {ask: ask, chat: chat, tool: tool, request: request, repair: repair, turn: turn,
   anthropic: anthropic, run_tools: run_tools, tool_from_schema: tool_from_schema,
   count_tokens: count_tokens, openai_compatible: openai_compatible,
   openai: openai, openrouter: openrouter, ollama: ollama, local: local}
}
let LLM = _llm_module()
)=culpre=";

