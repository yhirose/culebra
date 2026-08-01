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
#include <memory>
#include <stdexcept>
#include <string>

#include "webview.h"

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#include <objc/objc-runtime.h>
#endif

namespace culebra_webview {

// --- Concealing the window until it has something to show --------------------
//
// webview puts the window on screen from inside webview_create — empty, opaque
// and white — while the page only paints a few hundred ms later, so every app
// opens with a white flash (measured on macOS: the pre-paint window is a solid
// 255/255/255 rectangle for its whole duration).
//
// Taking the window off screen to hide it does not work: WebKit stops
// servicing requestAnimationFrame for a window that isn't on screen, so the
// "it painted" signal we wait for never arrives (measured: nothing within 3 s
// of an orderOut:). A fully transparent window keeps rendering while staying
// invisible, so alpha is the concealment, and the page tells us when to lift
// it.
#if defined(__APPLE__)
inline constexpr bool kConcealSupported = true;
inline void set_window_alpha(void* nswindow, double alpha) {
  if (!nswindow) return;
  reinterpret_cast<void (*)(id, SEL, double)>(objc_msgSend)(
      static_cast<id>(nswindow), sel_registerName("setAlphaValue:"), alpha);
}
#else
// Windows and Linux keep the old behaviour. The concealment proven above has
// no verified counterpart there — hiding a Win32/GTK window pauses
// requestAnimationFrame the same way orderOut: does, and the layered-window
// alpha that would dodge that is a known WebView2 compositing hazard — and
// neither can be checked from a macOS machine. Wrong here is an app that never
// becomes visible, so this stays off until someone can measure it.
inline constexpr bool kConcealSupported = false;
inline void set_window_alpha(void*, double) {}
#endif

// Shared by the two reveal paths — the page's first-paint signal and the
// deadline below — either of which may win. Everything that touches it runs on
// the loop (main) thread: WebKit delivers the page's message there, the GCD
// deadline fires on the main queue, and the culebra script that owns the
// Window runs there too. So plain fields suffice.
struct RevealGate {
  webview_t w{};
  void* window{};
  bool done = false;

  // Idempotent: the page re-arms its signal on every navigation, and the
  // deadline fires regardless of whether the signal already won.
  void reveal() {
    if (done) return;
    done = true;
    set_window_alpha(window, 1.0);
  }
};

// How long to wait for a page that never reports a first frame (JavaScript
// off, a navigation that never completes) before showing the window anyway.
// An app that stays invisible is a worse failure than a flash, and the signal
// lands ~200 ms in when it works at all, so this is pure headroom.
inline constexpr int64_t kRevealDeadlineMs = 1500;

// The window currently parked in run() (one per desktop app). Set on run()
// entry, cleared on return. It is what lets a request handler on the server's
// worker thread close the app via the static `Webview.Window.quit()` without
// holding the (non-sendable) window handle — the HTTP-bridge way to quit from
// a UI button/menu. See terminate_on_loop_thread for how that crosses threads.
inline std::atomic<webview_t> g_active_window{nullptr};

// A quit() that lands while no window is in run() yet. Desktop.run binds the
// server before it builds the window, so the app answers `/__quit` during a
// window where g_active_window is still null — without this the request would
// return 200 and the window would then open and never close.
inline std::atomic<bool> g_quit_pending{false};

// Set from inside the loop (see run()), cleared when it returns. Distinct from
// g_active_window, which is set just *before* run() and so cannot answer
// whether the loop is actually pumping.
inline std::atomic<bool> g_loop_running{false};

// A thin owning facade over webview's opaque C handle: clean,
// void-returning methods over simple types that wrap.h's `method<&T::m>`
// DSL can bind directly. (The C++ `webview::webview` class returns a
// `noresult` wrapper that has no marshalling shape, so we drive the
// stable C API instead — same ownership story as the SQLite/handle wraps.)
class Window {
 public:
  Window() : w_(webview_create(0, nullptr)) {
    if (!w_) throw std::runtime_error("webview: failed to create window");
    if (kConcealSupported) conceal_until_first_paint();
  }
  ~Window() {
    if (w_) webview_destroy(w_);
  }
  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  void set_title(const std::string& title) {
    webview_set_title(w_, title.c_str());
  }
  void set_size(int64_t width, int64_t height) {
    webview_set_size(w_, static_cast<int>(width), static_cast<int>(height),
                     WEBVIEW_HINT_NONE);
  }
  void set_html(const std::string& html) {
    webview_set_html(w_, html.c_str());
  }
  void navigate(const std::string& url) { webview_navigate(w_, url.c_str()); }

  // Runs the native event loop; blocks until terminate(). Marks this window as
  // the active one for the duration so `quit()` can find it. A quit() that
  // already arrived skips the loop rather than entering one nothing will leave:
  // the two seq_cst store/load pairs here and in quit() cross, so exactly one
  // side sees the other and the quit is never dropped.
  void run() {
    g_active_window.store(w_);
    if (!g_quit_pending.exchange(false)) {
      // Dispatched before the loop starts, so it runs on the first pump —
      // which is the earliest moment `is_running()` may answer true. Queued
      // only on the path that actually pumps: a callback posted to a loop that
      // never runs would never be freed either.
      webview_dispatch(
          w_, [](webview_t, void*) { g_loop_running.store(true); }, nullptr);
      webview_run(w_);
      g_loop_running.store(false);
    }
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

  // True while a window is pumping its event loop. `quit()` works before that
  // too (it is held), so this is not a precondition for quitting — it is for
  // code that needs to know the loop is up, such as a test that means to
  // exercise the cross-thread terminate rather than the held-quit path.
  static bool is_running() { return g_loop_running.load(); }

 private:
  // webview_terminate documents itself as safe from any thread, and is not:
  // Win32 calls PostQuitMessage, which by definition posts WM_QUIT to the
  // *calling* thread, so a quit from an HTTP worker never reaches the loop and
  // run() parks forever; Cocoa calls NSApplication_stop, which AppKit allows
  // only on the main thread. GTK is the one backend that honours the contract,
  // and it does so by routing through its own dispatch. So do that here, for
  // all three: webview_dispatch is genuinely thread-safe everywhere
  // (PostMessage to the message window / g_idle_add / dispatch_async).
  static void terminate_on_loop_thread(webview_t w) {
    webview_dispatch(
        w, [](webview_t self, void*) { webview_terminate(self); }, nullptr);
  }

  // Make the window transparent now and arrange for it to be revealed once the
  // page has a frame on screen — see the concealment note at the top.
  void conceal_until_first_paint() {
    gate_ = std::make_shared<RevealGate>();
    gate_->w = w_;
    gate_->window =
        webview_get_native_handle(w_, WEBVIEW_NATIVE_HANDLE_KIND_UI_WINDOW);
    set_window_alpha(gate_->window, 0.0);

    // DOMContentLoaded says the document exists; two animation frames later
    // WebKit has actually composited one. The script runs per document, so a
    // navigation re-arms it and the first page to paint wins. The raw context
    // is safe: the callback only runs while the webview exists, on this same
    // thread, and gate_ outlives w_.
    webview_bind(
        w_, "__culebra_first_paint__",
        [](const char* id, const char*, void* ctx) {
          auto* gate = static_cast<RevealGate*>(ctx);
          gate->reveal();
          webview_return(gate->w, id, 0, "null");
        },
        gate_.get());
    webview_init(w_,
                 "window.addEventListener('DOMContentLoaded', () => {"
                 "  requestAnimationFrame(() => requestAnimationFrame(() => {"
                 "    window.__culebra_first_paint__();"
                 "  }));"
                 "});");

    // The backstop for a page that never reports a frame. Weak: a deadline
    // that outlives the window does nothing.
#if defined(__APPLE__)
    dispatch_after_f(
        dispatch_time(DISPATCH_TIME_NOW, kRevealDeadlineMs * NSEC_PER_MSEC),
        dispatch_get_main_queue(), new std::weak_ptr<RevealGate>(gate_),
        [](void* ctx) {
          std::unique_ptr<std::weak_ptr<RevealGate>> held{
              static_cast<std::weak_ptr<RevealGate>*>(ctx)};
          if (auto gate = held->lock()) gate->reveal();
        });
#endif
  }

  webview_t w_;
  std::shared_ptr<RevealGate> gate_;
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
      .static_method<&culebra_webview::Window::quit>("quit")
      .static_method<&culebra_webview::Window::is_running>("is_running");
  return true;
}();

}  // namespace
