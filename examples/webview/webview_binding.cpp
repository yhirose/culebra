// Spike 0: wrap webview/webview as a culebra builtin namespace.
//
// Build an extended culebra that knows `Webview.Window` on every backend:
//
//   culebra wrap examples/webview/webview_binding.cpp \
//       --link "-framework WebKit -ldl" -o webview-culebra      # macOS
//
// then:
//
//   let w = Webview.Window.new()
//   w.set_title("hi"); w.set_size(640, 480)
//   w.set_html("<h1>It works</h1>")
//   w.run()                 # blocks the (GUI/main) thread until terminate()
//
// The vendored single-header `webview.h` (webview/webview 0.12.0, MIT) is
// self-contained and, in C++ header-only mode, defines `WEBVIEW_API` as
// `inline` — so including it here emits webview's C API into this TU. The
// native frontend frameworks are supplied at wrap time via `--link`.

#include <wrap.h>

#include <stdexcept>
#include <string>

#include "webview.h"

namespace culebra_webview {

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
  // from another thread per the webview C API contract).
  void run() { webview_run(w_); }
  void terminate() { webview_terminate(w_); }

 private:
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
      .method<&culebra_webview::Window::terminate>("terminate");
  return true;
}();

}  // namespace
