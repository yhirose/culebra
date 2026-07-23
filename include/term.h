#pragma once

// Value-neutral terminal-control primitives shared by the interpreter and
// JIT/AOT backends. These are the small OS-thin pieces a TUI needs that
// cannot be expressed in culebra itself: raw mode (termios), terminal size
// (ioctl), and a timed non-blocking key read (poll). Everything above this
// — the `Term` namespace, the `Screen`/`Key` types, colours and the render
// loop — lives in the culebra stdlib preamble (`TERM_MODULE_SOURCE`), so it
// stays automatically symmetric across backends.
//
// Underscore-prefixed `_Term` marks these as the wrapper's ABI, not a stable
// surface for direct use.

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#include <cstdio>
#elif !defined(_WIN32)
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

#include <os_compat.h>  // os_isatty + guarded <windows.h> (console size on Windows)
#include <unicodelib.h>
#include <unicodelib_encodings.h>

namespace culebra {
namespace _term_detail {

#if defined(__EMSCRIPTEN__)

// Browser TUI backend for the Playground. There is no real tty: raw mode is
// a no-op (the frontend terminal emulator is already "raw"), size/resize
// come from JS-side state the frontend keeps current, and read_key needs to
// suspend the wasm call until a key arrives or a timeout elapses — the
// browser equivalent of POSIX poll() on stdin.
//
// That suspend needs JSPI, which only the "full" wasm build (see
// playground/build.sh) links with -sJSPI. The "basic" build (any browser
// without JSPI) gets a stub that always times out immediately, so a TUI
// script still runs (degrading to non-interactive, like piped stdin does
// natively) instead of failing to link.

inline void raw_on() {}
inline void raw_off() {}

// self.__termCols/__termRows are set once by worker.js before a run starts
// (from the terminal emulator's current size); self.__termResized is set by
// the frontend on an actual resize and consumed here. Plain EM_JS (no
// suspend) — reading JS state is synchronous, unlike waiting for a key.
EM_JS(int, _wasm_term_cols, (), { return self.__termCols || 80; });
EM_JS(int, _wasm_term_rows, (), { return self.__termRows || 24; });
EM_JS(int, _wasm_take_resize, (), {
  if (self.__termResized) {
    self.__termResized = false;
    return 1;
  }
  return 0;
});

inline int cols() { return _wasm_term_cols(); }
inline int rows() { return _wasm_term_rows(); }
inline bool take_resize() { return _wasm_take_resize() != 0; }

#if defined(CULEBRA_WASM_JSPI)
// Suspends the wasm call (via JSPI) until worker.js's self.__waitForKey
// resolves — either a key arrived (the frontend forwards it early) or
// timeout_ms elapsed with none. See playground/worker.js.
EM_ASYNC_JS(char*, _wasm_read_key, (double timeout_ms), {
  return Asyncify.handleAsync(async () => {
    const key = await self.__waitForKey(timeout_ms);
    const len = lengthBytesUTF8(key) + 1;
    const ptr = _malloc(len);
    stringToUTF8(key, ptr, len);
    return ptr;
  });
});

inline std::string read_key(double timeout_secs) {
  double ms = timeout_secs <= 0 ? 0.0 : timeout_secs * 1000.0;
  char* p = _wasm_read_key(ms);
  std::string out(p);
  std::free(p);
  return out;
}
#else
// No JSPI in this build: a TUI script's poll loop always sees "no key
// yet" — the same degradation piped/non-tty stdin already causes natively.
inline std::string read_key(double) { return ""; }
#endif  // CULEBRA_WASM_JSPI

#elif !defined(_WIN32)

// Original terminal attributes, captured on the first `raw_on` so `raw_off`
// can restore them. `_raw_active` guards against double enable/disable.
inline struct termios& _saved_termios() {
  static struct termios t;
  return t;
}
inline bool& _raw_active() {
  static bool b = false;
  return b;
}

// Set by the SIGWINCH handler when the terminal is resized; consumed by
// `take_resize`. The render loop polls this to know it must re-query the
// size and repaint. (Only SIGWINCH is touched — separate from culebra's
// cooperative SIGINT handling.)
inline volatile std::sig_atomic_t& _winch_flag() {
  static volatile std::sig_atomic_t f = 0;
  return f;
}
inline void _winch_handler(int) { _winch_flag() = 1; }

// True (once) if a resize happened since the last call. Clears the flag.
inline bool take_resize() {
  if (_winch_flag()) {
    _winch_flag() = 0;
    return true;
  }
  return false;
}

// Restore cooked mode. Registered with atexit on first raw_on so an abnormal
// exit (uncaught error, signal that bypasses culebra's scope unwinding) does
// not leave the user's terminal wedged in raw mode.
inline void raw_off() {
  if (!_raw_active()) return;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &_saved_termios());
  std::signal(SIGWINCH, SIG_DFL);
  _raw_active() = false;
}

// Enter raw mode: no echo, no line buffering, no signal generation, and
// non-blocking reads (VMIN/VTIME = 0 — the timed wait is done with poll in
// `read_key`). A no-op when stdin is not a tty (piped input / test runs), so
// a TUI script run non-interactively degrades instead of erroring.
inline void raw_on() {
  if (_raw_active()) return;
  if (!isatty(STDIN_FILENO)) return;
  if (tcgetattr(STDIN_FILENO, &_saved_termios()) != 0) return;
  static bool atexit_registered = false;
  if (!atexit_registered) {
    std::atexit(raw_off);
    atexit_registered = true;
  }
  struct termios raw = _saved_termios();
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
  raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
  raw.c_oflag &= ~(OPOST);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return;
  _raw_active() = true;
  // Watch for terminal resizes. No SA_RESTART, so a SIGWINCH interrupts a
  // blocking poll in read_key and the loop notices the resize promptly.
  struct sigaction sa;
  sa.sa_handler = _winch_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGWINCH, &sa, nullptr);
}

// Terminal width / height in character cells, querying the controlling
// terminal. Falls back to a conventional 80x24 when there is no tty (so
// layout math stays sane off-terminal).
inline int cols() {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
    return ws.ws_col;
  return 80;
}
inline int rows() {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
    return ws.ws_row;
  return 24;
}

// Wait up to `timeout_secs` for input, then read the bytes that arrived in
// one shot — a single keypress, possibly a multi-byte escape sequence
// (arrows / function keys send `\x1b[A` etc.). Returns "" on timeout, EOF,
// or non-tty stdin. NUL bytes are dropped so the result survives the
// NUL-terminated `const char*` boundary the JIT path returns through; no
// arrow/function key contains a NUL.
inline std::string read_key(double timeout_secs) {
  if (!isatty(STDIN_FILENO)) return "";
  struct pollfd pfd;
  pfd.fd = STDIN_FILENO;
  pfd.events = POLLIN;
  pfd.revents = 0;
  int ms = timeout_secs <= 0 ? 0 : static_cast<int>(timeout_secs * 1000.0);
  int r = poll(&pfd, 1, ms);
  if (r <= 0 || !(pfd.revents & POLLIN)) return "";
  char buf[32];
  ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
  if (n <= 0) return "";
  std::string out;
  for (ssize_t i = 0; i < n; i++)
    if (buf[i] != '\0') out.push_back(buf[i]);
  return out;
}

#else  // _WIN32

// Windows Console API port of the POSIX termios/poll primitives. The console's
// Virtual Terminal modes are the bridge to symmetry: ENABLE_VIRTUAL_TERMINAL_-
// INPUT makes the console deliver arrow/function keys as the same `\x1b[A`
// escape sequences the POSIX path and the culebra `Term` preamble already
// parse, and ENABLE_VIRTUAL_TERMINAL_PROCESSING makes the ANSI colour/cursor
// codes the render loop emits take effect — so nothing above this layer needs a
// Windows branch.

// Terminal width/height in cells from a single console query (the per-frame
// take_resize poll wants both dimensions at once), falling back to 80x24.
inline std::pair<int, int> _console_size() {
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
    int w = info.srWindow.Right - info.srWindow.Left + 1;
    int h = info.srWindow.Bottom - info.srWindow.Top + 1;
    return {w > 0 ? w : 80, h > 0 ? h : 24};
  }
  return {80, 24};
}
inline int cols() { return _console_size().first; }
inline int rows() { return _console_size().second; }

// Saved console modes captured on the first raw_on so raw_off can restore them.
// _raw_active guards against double enable/disable, mirroring the POSIX path.
inline bool& _raw_active() {
  static bool b = false;
  return b;
}
inline DWORD& _saved_in_mode() {
  static DWORD m = 0;
  return m;
}
inline DWORD& _saved_out_mode() {
  static DWORD m = 0;
  return m;
}

inline void raw_off() {
  if (!_raw_active()) return;
  HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
  HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
  if (hin != INVALID_HANDLE_VALUE) SetConsoleMode(hin, _saved_in_mode());
  if (hout != INVALID_HANDLE_VALUE) SetConsoleMode(hout, _saved_out_mode());
  _raw_active() = false;
}

// Enter raw mode: no echo, no line buffering, no input processing, and keys as
// VT escape sequences. A no-op when stdin is not a console (piped input / test
// runs), so a TUI script run non-interactively degrades instead of erroring —
// GetConsoleMode failing is the Windows equivalent of the POSIX !isatty check.
inline void raw_on() {
  if (_raw_active()) return;
  HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
  HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD in_mode = 0;
  if (hin == INVALID_HANDLE_VALUE || !GetConsoleMode(hin, &in_mode)) return;
  _saved_in_mode() = in_mode;
  DWORD out_mode = 0;
  bool have_out = hout != INVALID_HANDLE_VALUE && GetConsoleMode(hout, &out_mode);
  _saved_out_mode() = out_mode;

  static bool atexit_registered = false;
  if (!atexit_registered) {
    std::atexit(raw_off);
    atexit_registered = true;
  }
  // Raw input built from scratch: only VT input (no line/echo/processed/mouse/
  // window events), so ReadFile returns keypress bytes and nothing spurious
  // wakes the wait in read_key.
  DWORD raw_in = ENABLE_VIRTUAL_TERMINAL_INPUT;
  if (!SetConsoleMode(hin, raw_in)) return;
  if (have_out)
    SetConsoleMode(hout, out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                             DISABLE_NEWLINE_AUTO_RETURN);
  _raw_active() = true;
}

// Last console size take_resize observed; a change since the previous call is
// reported as a resize. Windows has no SIGWINCH, and in VT-input mode buffer-
// size events are not in the byte stream, so the render loop's per-frame poll
// compares the size instead — cheap and dependency-free.
inline std::pair<int, int>& _last_size() {
  static std::pair<int, int> s{-1, -1};
  return s;
}
inline bool take_resize() {
  std::pair<int, int> cur = _console_size();  // one syscall for both dims
  std::pair<int, int>& last = _last_size();
  if (last.first < 0) { last = cur; return false; }  // first call: establish
  if (cur != last) { last = cur; return true; }
  return false;
}

// Wait up to `timeout_secs` for a keypress, then read the VT bytes that
// arrived. Returns "" on timeout, EOF, or non-console stdin. NUL bytes are
// dropped so the result survives the NUL-terminated `const char*` JIT boundary.
inline std::string read_key(double timeout_secs) {
  HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode = 0;
  if (hin == INVALID_HANDLE_VALUE || !GetConsoleMode(hin, &mode)) return "";
  DWORD ms = timeout_secs <= 0 ? 0 : static_cast<DWORD>(timeout_secs * 1000.0);
  if (WaitForSingleObject(hin, ms) != WAIT_OBJECT_0) return "";
  char buf[32];
  DWORD n = 0;
  if (!ReadFile(hin, buf, sizeof(buf), &n, nullptr) || n == 0) return "";
  std::string out;
  for (DWORD i = 0; i < n; i++)
    if (buf[i] != '\0') out.push_back(buf[i]);
  return out;
}

#endif  // _WIN32

// Detected colour capability: 0 = none, 1 = 16 colours, 2 = 256, 3 = 24-bit
// truecolour. Honours the `NO_COLOR` convention (present => off), `FORCE_COLOR`
// (overrides the tty check), and `COLORTERM` / `TERM`. The culebra layer
// downsamples colours to this level (and may override it).
inline int color_level() {
  if (std::getenv("NO_COLOR")) return 0;
  const char* fc = std::getenv("FORCE_COLOR");
  bool forced = fc && std::string(fc) != "0";
  if (!forced && !os_isatty(1)) return 0;
  auto env = [](const char* k) {
    const char* v = std::getenv(k);
    return std::string(v ? v : "");
  };
  std::string ct = env("COLORTERM");
  if (ct.find("truecolor") != std::string::npos ||
      ct.find("24bit") != std::string::npos)
    return 3;
  std::string t = env("TERM");
  if (t.find("256color") != std::string::npos) return 2;
  if (t == "dumb") return 0;
  return 1;
}

// Flush buffered output. Both backends write through std::cout, which is
// block-buffered when stdout is not a tty and may not surface a frame that
// lacks a trailing newline; a TUI builds a whole frame then flushes once.
inline void flush() { std::cout.flush(); }

// Display width (terminal columns) of a UTF-8 string, measured per extended
// grapheme cluster — so an emoji ZWJ sequence, a flag, a keycap, or a
// base+variation-selector pair counts as one cell group rather than the sum
// of its scalars. Delegates to cpp-unicodelib (UAX #11 + UTS #51), which
// tracks the Unicode version the vendored library is pinned to; width thus
// stays consistent with the grapheme / category / case-folding the rest of
// culebra already sources from there. Ambiguous-width characters count as 1
// (the modern non-East-Asian terminal convention).
inline int width(const std::string& s) {
  std::u32string u32;
  unicode::utf8::decode(s.data(), s.size(), u32);
  return unicode::width(u32.data(), u32.size());
}

}  // namespace _term_detail
}  // namespace culebra
