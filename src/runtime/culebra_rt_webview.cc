// `Webview` builtin namespace — a native WebView window (webview/webview).
//
// Built into the driver (interp + JIT) and force-loaded into `culebra build`
// outputs when the program references Webview, mirroring the Scene facade's
// usage-gating (CULEBRA_ENABLE_WEBVIEW / aot_scan). So a desktop app is just:
//
//   let w = Webview.Window.new()
//   w.set_title("hi"); w.set_size(640, 480); w.navigate(url); w.run()
//
// and `culebra build app.cul -o app` — no `culebra wrap` step.
//
// The vendored single-header `webview.h` (webview/webview, MIT) is
// self-contained; in C++ header-only mode it defines `WEBVIEW_API` as `inline`,
// so including it here emits webview's C API into this TU. The native
// frameworks (WebKit / WebKitGTK / WebView2) are supplied by CULEBRA_WEBVIEW_LINK.

#include <wrap.h>

#include <atomic>
#include <stdexcept>
#include <string>

#include "webview.h"

namespace culebra_webview {

// The window currently parked in run() (one per desktop app). Set on run()
// entry, cleared on return. webview_terminate is thread-safe, so a request
// handler on the server's worker thread can close the app via the static
// `Webview.Window.quit()` without holding the (non-sendable) window handle —
// the HTTP-bridge way to quit from a UI button/menu.
inline std::atomic<webview_t> g_active_window{nullptr};

// A quit() that lands while no window is in run() yet. Desktop.run binds the
// server before it builds the window, so the app answers `/__quit` during a
// window where g_active_window is still null — without this the request would
// return 200 and the window would then open and never close.
inline std::atomic<bool> g_quit_pending{false};

// A thin owning facade over webview's opaque C handle: clean,
// void-returning methods over simple types that wrap.h's `method<&T::m>`
// DSL can bind directly. (The C++ `webview::webview` class returns a
// `noresult` wrapper that has no marshalling shape, so we drive the
// stable C API instead — same ownership story as the SQLite/handle wraps.)
class Window {
 public:
  Window() : w_(webview_create(0, nullptr)) {
    if (!w_) throw std::runtime_error("webview: failed to create window");
  }
  ~Window() {
    if (w_) webview_destroy(w_);
  }
  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  void set_title(const std::string& title) {
    webview_set_title(w_, title.c_str());
  }
  void set_size(long width, long height) {
    webview_set_size(w_, static_cast<int>(width), static_cast<int>(height),
                     WEBVIEW_HINT_NONE);
  }
  void set_html(const std::string& html) {
    webview_set_html(w_, html.c_str());
  }
  void navigate(const std::string& url) { webview_navigate(w_, url.c_str()); }

  // Runs the native event loop; blocks until terminate() (safe to call
  // from another thread per the webview C API contract). Marks this window as
  // the active one for the duration so `quit()` can find it. A quit() that
  // already arrived skips the loop rather than entering one nothing will leave:
  // the two seq_cst store/load pairs here and in quit() cross, so exactly one
  // side sees the other and the quit is never dropped.
  void run() {
    g_active_window.store(w_);
    if (!g_quit_pending.exchange(false)) webview_run(w_);
    g_active_window.store(nullptr);
    g_quit_pending.store(false);
  }
  void terminate() { terminate_on_loop_thread(w_); }

  // Terminate whichever window is currently in run() — callable from any
  // thread (e.g. an HTTP handler). If none is running yet, the quit is held
  // for the next run() instead of being dropped.
  static void quit() {
    g_quit_pending.store(true);
    if (auto* w = g_active_window.load()) terminate_on_loop_thread(w);
  }

 private:
  // webview_terminate is not thread-safe on every backend: the Win32 one calls
  // PostQuitMessage, which posts WM_QUIT to the *calling* thread's queue, so a
  // quit from an HTTP worker never reaches the main thread's GetMessage and
  // run() parks forever. webview_dispatch is the thread-safe primitive on all
  // three backends (PostMessage to the message window / g_idle_add /
  // dispatch_async), so hop onto the loop thread and terminate from there —
  // which is what the GTK backend's own terminate already does internally.
  static void terminate_on_loop_thread(webview_t w) {
    webview_dispatch(
        w, [](webview_t self, void*) { webview_terminate(self); }, nullptr);
  }

  webview_t w_;
};

}  // namespace culebra_webview

namespace {

const bool registered = [] {
  culebra::wrap<culebra_webview::Window>("Webview", "Window")
      .ctor<>()
      .method<&culebra_webview::Window::set_title>("set_title", {"title"})
      .method<&culebra_webview::Window::set_size>("set_size",
                                                  {"width", "height"})
      .method<&culebra_webview::Window::set_html>("set_html", {"html"})
      .method<&culebra_webview::Window::navigate>("navigate", {"url"})
      .method<&culebra_webview::Window::run>("run")
      .method<&culebra_webview::Window::terminate>("terminate")
      .static_method<&culebra_webview::Window::quit>("quit");
  return true;
}();

}  // namespace
