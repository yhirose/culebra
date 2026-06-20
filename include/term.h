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
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace culebra {
namespace _term_detail {

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

// Flush buffered output. Both backends write through std::cout, which is
// block-buffered when stdout is not a tty and may not surface a frame that
// lacks a trailing newline; a TUI builds a whole frame then flushes once.
inline void flush() { std::cout.flush(); }

}  // namespace _term_detail
}  // namespace culebra
