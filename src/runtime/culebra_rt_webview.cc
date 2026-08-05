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
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "webview.h"

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#include <objc/objc-runtime.h>
#include <objc/runtime.h>
#elif defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <gtk/gtk.h>
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

// --- Asking the page before the OS closes the window -------------------------
//
// webview.h itself never asks: every platform's close path runs straight to
// destruction (Win32 WM_CLOSE -> DestroyWindow; GTK4's default close-request
// handler; a macOS delegate that only hears windowWillClose:, after the
// decision is already made). That means a program mid-write when the user
// clicks the frame's close button has no chance to intervene. The three
// blocks below intercept the OS-level "may I close?" event on each platform
// — without touching the vendored header, which stays a straight copy of
// upstream — and hand the decision to the page instead of closing outright.
//
// Contract with JS: if the page defines window.__culebra_before_close__, it
// alone decides whether/when to actually close, by eventually calling
// window.__culebra_close__() (bound in Window's ctor). If it defines
// nothing, the window closes immediately — the same behavior as before this
// hook existed, so hello.cul and any app that doesn't opt in are unaffected.

// Fires on the UI thread from inside the OS's own close callback, so a plain
// webview_eval (no dispatch) is safe — every other engine call in webview.h
// already assumes it runs on that same thread.
inline void request_close(webview_t w) {
  webview_eval(w,
               "(function(){"
               "if (typeof window.__culebra_before_close__ === 'function') {"
               "window.__culebra_before_close__();"
               "} else {"
               "window.__culebra_close__();"
               "}})();");
}

#if defined(__APPLE__)
// Native NSWindow* -> the webview_t it belongs to. windowShouldClose: below
// is added once to the delegate class shared by every window webview.h
// creates, so the callback needs this to recover which webview_t a given
// NSWindow* belongs to — Windows and GTK don't need it, since their close
// callbacks receive their own per-window data directly (see below).
inline std::mutex g_native_window_mu;
inline std::unordered_map<void*, webview_t> g_native_window_map;

inline void register_native_window(void* native_window, webview_t w) {
  std::lock_guard<std::mutex> lock(g_native_window_mu);
  g_native_window_map[native_window] = w;
}
inline void unregister_native_window(void* native_window) {
  std::lock_guard<std::mutex> lock(g_native_window_mu);
  g_native_window_map.erase(native_window);
}
inline webview_t lookup_native_window(void* native_window) {
  std::lock_guard<std::mutex> lock(g_native_window_mu);
  auto it = g_native_window_map.find(native_window);
  return it != g_native_window_map.end() ? it->second : nullptr;
}

// The delegate class (WebviewNSWindowDelegate) is registered once and shared
// by every window webview.h creates; adding windowShouldClose: to it here —
// guarded so a second window doesn't try to add it twice — reaches all of
// them. windowWillClose:, which vendor already implements on the same class,
// fires only after the decision is made and cannot cancel it.
inline void install_close_intercept(webview_t w, void* native_window) {
  register_native_window(native_window, w);
  id nswindow = static_cast<id>(native_window);
  id delegate = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(
      nswindow, sel_registerName("delegate"));
  if (!delegate) return;
  Class cls = object_getClass(delegate);
  SEL sel = sel_registerName("windowShouldClose:");
  if (class_respondsToSelector(cls, sel)) return;
  class_addMethod(
      cls, sel,
      (IMP)(+[](id, SEL, id sender) -> BOOL {
        if (webview_t found =
                lookup_native_window(static_cast<void*>(sender))) {
          request_close(found);
        }
        return NO;
      }),
      "c@:@");
}
inline void uninstall_close_intercept(void* native_window) {
  unregister_native_window(native_window);
}
#elif defined(_WIN32)
namespace win32_close_intercept {
constexpr auto kPropWebview = L"CulebraWebviewHandle";
constexpr auto kPropOrigProc = L"CulebraOrigWndProc";

inline LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_CLOSE) {
    if (webview_t w = static_cast<webview_t>(GetPropW(hwnd, kPropWebview))) {
      request_close(w);
      return 0;  // Handled: the default DestroyWindow is suppressed until
                 // the page (or the fallback above) says to actually close.
    }
  }
  auto orig = reinterpret_cast<WNDPROC>(GetPropW(hwnd, kPropOrigProc));
  return orig ? CallWindowProcW(orig, hwnd, msg, wp, lp)
              : DefWindowProcW(hwnd, msg, wp, lp);
}
}  // namespace win32_close_intercept

// Classic GWLP_WNDPROC subclassing rather than comctl32's SetWindowSubclass,
// so this needs no new link dependency. wndproc reads the webview_t straight
// back off the window (kPropWebview), so this needs no shared registry.
inline void install_close_intercept(webview_t w, void* native_window) {
  HWND hwnd = static_cast<HWND>(native_window);
  SetPropW(hwnd, win32_close_intercept::kPropWebview,
          static_cast<HANDLE>(w));
  auto orig = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
      hwnd, GWLP_WNDPROC,
      reinterpret_cast<LONG_PTR>(win32_close_intercept::wndproc)));
  SetPropW(hwnd, win32_close_intercept::kPropOrigProc,
          reinterpret_cast<HANDLE>(orig));
}
inline void uninstall_close_intercept(void* native_window) {
  HWND hwnd = static_cast<HWND>(native_window);
  if (auto orig = reinterpret_cast<WNDPROC>(
          GetPropW(hwnd, win32_close_intercept::kPropOrigProc))) {
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig));
  }
  RemovePropW(hwnd, win32_close_intercept::kPropOrigProc);
  RemovePropW(hwnd, win32_close_intercept::kPropWebview);
}
#elif defined(__linux__)
// GTK4's cancelable close signal is close-request (delete-event is the GTK3
// / raw X11 name and does not apply to the GtkWindow this engine builds).
// g_signal_connect only needs symbols webview_gtk_dynload.cc already
// forwards (g_signal_connect_data, gtk_window_get_type,
// g_type_check_instance_cast via the GTK_WINDOW() cast), so nothing there
// needs a new entry. w travels as the signal's own user_data, so this needs
// no shared registry either — each GtkWindow's connection carries its own.
inline void install_close_intercept(webview_t w, void* native_window) {
  auto* window = GTK_WINDOW(static_cast<GtkWidget*>(native_window));
  g_signal_connect(
      window, "close-request",
      G_CALLBACK(+[](GtkWindow*, gpointer data) -> gboolean {
        request_close(static_cast<webview_t>(data));
        return TRUE;  // Stop the default handler from destroying the window.
      }),
      static_cast<gpointer>(w));
}
inline void uninstall_close_intercept(void*) {}
#else
inline void install_close_intercept(webview_t, void*) {}
inline void uninstall_close_intercept(void*) {}
#endif

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
    bind_close();
    native_window_ =
        webview_get_native_handle(w_, WEBVIEW_NATIVE_HANDLE_KIND_UI_WINDOW);
    if (native_window_) install_close_intercept(w_, native_window_);
  }
  ~Window() {
    if (native_window_) uninstall_close_intercept(native_window_);
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

  // window.__culebra_close__() — the page's own way to actually close the
  // window, at any point of its choosing (not only after a confirmation).
  // Also what request_close() falls back to when the page defines no
  // window.__culebra_before_close__, so a window with no opt-in keeps
  // closing immediately, as before that hook existed.
  void bind_close() {
    webview_bind(
        w_, "__culebra_close__",
        [](const char* id, const char*, void* ctx) {
          auto w = static_cast<webview_t>(ctx);
          terminate_on_loop_thread(w);
          webview_return(w, id, 0, "null");
        },
        w_);
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
  void* native_window_{};
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
