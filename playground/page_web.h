#pragma once

// Page.socket in a browser: the page that runs the program, as a WebSocket's
// connection (ws::Link). The page's messages wait in the worker
// (web-page.js) until receive() takes them; what the program sends goes to
// the page as a message. Waiting gives the worker back to its event loop, and
// a message from the page or a MIDI message wakes it (worker-wake.js), so a
// loop that reads both, as the FM Synth's does, wakes for either. Without
// JSPI (an older browser) nothing can wait: there is no page socket, as there
// are no MIDI ports.
//
// Each socket opened is a session of its own number: a socket closed, or let
// go after the next one opened, touches only its own.

#include <emscripten.h>

#include <memory>
#include <string>

#include <stdlib/audio_web.h>  // _wasm_input_wait, the wait MIDI shares
#include <stdlib/ws_link.h>

namespace culebra::ws {

#if defined(CULEBRA_WASM_JSPI)
// The page told the program listens, as a new session: what the page sent
// before is dropped.
EM_JS(int, _wasm_page_open, (), {
  const page = self.__page;
  page.session = ++page.sessions;
  page.closed = false;
  page.queue.length = 0;
  postMessage({ type: "pageOpen" });
  return page.session;
});
// Whether `session` is no longer the page's open one, or the page went away.
EM_JS(int, _wasm_page_closed, (int session), {
  const page = self.__page;
  return page.session !== session || page.closed ? 1 : 0;
});
// The page's next message to `session`, malloc'd (freed by the caller), or
// null: what came before the page went away is still read.
EM_JS(char*, _wasm_page_take, (int session), {
  const page = self.__page;
  const q = page.queue;
  if (page.session !== session || q.length === 0) return 0;
  const text = q.shift();
  const n = lengthBytesUTF8(text) + 1;
  const at = _malloc(n);
  stringToUTF8(text, at, n);
  return at;
});
// Whether a socket of this run is open on the page now.
EM_JS(int, _wasm_page_in_use, (), {
  const page = self.__page;
  return page.session !== 0 && !page.closed ? 1 : 0;
});
EM_JS(void, _wasm_page_send, (const char* text, int n), {
  postMessage({ type: "pageSend", text: UTF8ToString(text, n) });
});
// `session` closed (or let go of): the page is told, if it is still the open one.
EM_JS(void, _wasm_page_close, (int session), {
  const page = self.__page;
  if (page.session !== session) return;
  page.session = 0;
  page.queue.length = 0;
  postMessage({ type: "pageClose" });
});

struct PageLink : Link {
  int64_t timeout_ms = 0;
  ~PageLink() override { close(); }
  // With a timeout, one wait, which anything coming ends: a MIDI message
  // answers -1 before the timeout, so a loop reading both is woken by either.
  int receive(std::string& out) override {
    for (bool waited = false;; waited = true) {
      if (char* text = _wasm_page_take(session_)) {
        out = text;
        free(text);
        return 1;
      }
      if (!is_open()) return 0;
      if (waited && timeout_ms > 0) return -1;
      _audio_detail::_wasm_input_wait(timeout_ms > 0 ? static_cast<double>(timeout_ms)
                                                     : kForeverMs);
    }
  }
  void set_read_timeout(int64_t ms) override { timeout_ms = ms; }
  bool send(const char* data, size_t len) override {
    if (!is_open()) return false;
    _wasm_page_send(data, static_cast<int>(len));
    return true;
  }
  void close() override { _wasm_page_close(session_); }
  bool is_open() override { return !_wasm_page_closed(session_); }

 private:
  // A wait with no timeout, a slice at a time (setTimeout's longest is 2^31-1).
  static constexpr double kForeverMs = 1e9;
  const int session_ = _wasm_page_open();
};
#endif

std::unique_ptr<Link> page_link(std::string& err) {
#if defined(CULEBRA_WASM_JSPI)
  if (_wasm_page_in_use()) {
    err = "the page has a socket open already";
    return nullptr;
  }
  return std::make_unique<PageLink>();
#else
  err = "this browser cannot wait for the page (it has no JSPI)";
  return nullptr;
#endif
}

}  // namespace culebra::ws
