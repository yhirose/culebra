// Load GTK4 + WebKitGTK when a window is created, not when the process starts.
//
// Every other platform's engine is either part of the OS (macOS: WebKit.framework)
// or located at run time by the vendored header itself (Windows: webview.h's
// WebView2 loader reads the registry and LoadLibrary's the client DLL). Only on
// Linux is the engine a third-party shared library that webview.h calls directly,
// which puts libgtk-4.so.1 and libwebkitgtk-6.0.so.4 in DT_NEEDED and stops the
// binary from starting at all where they are absent — a headless server, a
// container, a desktop that never installed webkitgtk-6.0 (nothing in Ubuntu's
// archive depends on it, so nothing pulls it in). culebra is a language runtime
// first: a program that never opens a window should not need a WebKit stack to
// print its version.
//
// So nothing links them. This TU defines every GTK/WebKit symbol the binding
// reaches as a forwarder over a pointer resolved with dlsym. The set is taken
// from the archive's undefined symbols rather than from reading webview.h: the
// GTK3 paths there compile out, and GObject's cast macros add calls the source
// never spells (g_type_check_instance_cast and the *_get_type() it is passed).
// vendor/webview/webview.h itself is untouched, so upgrading it stays a copy.
//
// Loading is all-or-nothing, inside gtk_init_check() — the engine's first call
// (webview.h's window_init) and the one already allowed to answer "no". FALSE
// there raises "GTK init failed", webview_create returns null, and the binding
// throws `webview: failed to create window`: the same failure macOS and Windows
// produce when their engine cannot start, at the same point (Webview.Window.new).
// Nothing new to report, and nowhere new to report it.
//
// These definitions must not reach the executable's dynamic symbol table:
// exported, they would interpose on the real libraries' own internal calls once
// dlopen brings them in — every GObject cast inside GTK and WebKit routes
// through g_type_check_instance_cast. Two things keep them out, and the second
// is what actually covers all of them: -fvisibility=hidden (CMakeLists) hides
// the GTK and GLib half, while WEBKIT_API/JSC_API pin their own declarations to
// default visibility and ignore it — but the link publishes only `culebra_*`
// (cmake/exported_symbols.txt, via --dynamic-list), and an AOT link has no
// engine DSO left to export anything for. tools/check_webview_dynload.sh holds
// both ends of that.

#include <dlfcn.h>

#include <cstddef>
#include <iterator>

#include <gtk/gtk.h>
#include <webkit/webkit.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#endif

// GObject wraps this one in a function-like macro that casts the result back to
// the argument's type. Convenience for callers; here it would rewrite both the
// definition below and its own forwarding call. (gtkmain.h has the same shape
// for gtk_init_check, but only under G_OS_WIN32.)
#undef g_object_ref_sink

// webview.h picks its WebKit calls behind WEBKIT_MAJOR/MINOR tests. The 6.0 API
// this builds against did not exist before WebKit 2.40, so every one of those
// branches takes its modern arm and the set below is what it selects. A version
// that ever chose differently would leave an undefined symbol at link time —
// the same way a new call would.

// One entry per symbol, in the order `nm -u` reports them. Adding a call to the
// binding that reaches a new one is a link error naming it, not a silent gap.
#define CULEBRA_WEBVIEW_GTK_SYMBOLS(X)                                         \
  X(g_free)                                                                    \
  X(g_idle_add_full)                                                           \
  X(g_main_context_iteration)                                                  \
  X(g_object_ref_sink)                                                         \
  X(g_object_unref)                                                            \
  X(g_signal_connect_data)                                                     \
  X(g_signal_handlers_disconnect_matched)                                      \
  X(g_type_check_instance_cast)                                                \
  X(g_type_check_instance_is_a)                                                \
  X(gdk_display_get_default)                                                   \
  X(gdk_x11_display_get_type)                                                  \
  X(gtk_init_check)                                                            \
  X(gtk_widget_get_type)                                                       \
  X(gtk_widget_grab_focus)                                                     \
  X(gtk_widget_set_size_request)                                               \
  X(gtk_widget_set_visible)                                                    \
  X(gtk_window_close)                                                          \
  X(gtk_window_get_child)                                                      \
  X(gtk_window_get_type)                                                       \
  X(gtk_window_new)                                                            \
  X(gtk_window_set_child)                                                      \
  X(gtk_window_set_default_size)                                               \
  X(gtk_window_set_resizable)                                                  \
  X(gtk_window_set_title)                                                      \
  X(jsc_value_to_string)                                                       \
  X(webkit_get_major_version)                                                  \
  X(webkit_get_minor_version)                                                  \
  X(webkit_settings_set_enable_developer_extras)                               \
  X(webkit_settings_set_enable_write_console_messages_to_stdout)               \
  X(webkit_settings_set_javascript_can_access_clipboard)                       \
  X(webkit_user_content_manager_add_script)                                    \
  X(webkit_user_content_manager_register_script_message_handler)               \
  X(webkit_user_content_manager_remove_all_scripts)                            \
  X(webkit_user_script_new)                                                    \
  X(webkit_user_script_ref)                                                    \
  X(webkit_user_script_unref)                                                  \
  X(webkit_web_view_evaluate_javascript)                                       \
  X(webkit_web_view_get_settings)                                              \
  X(webkit_web_view_get_type)                                                  \
  X(webkit_web_view_get_uri)                                                   \
  X(webkit_web_view_get_user_content_manager)                                  \
  X(webkit_web_view_load_html)                                                 \
  X(webkit_web_view_load_uri)                                                  \
  X(webkit_web_view_new)

namespace {

// Slot types come from the headers' own declarations, so a signature that drifts
// is a compile error here rather than a corrupt call at run time.
struct Symbols {
#define CULEBRA_WEBVIEW_SLOT(name) decltype(&::name) name = nullptr;
  CULEBRA_WEBVIEW_GTK_SYMBOLS(CULEBRA_WEBVIEW_SLOT)
#undef CULEBRA_WEBVIEW_SLOT
};

Symbols sym;

// sonames, not the -dev symlinks: a machine that runs a Webview app has the
// runtime packages installed, not the headers. libwebkitgtk pulls GTK, GLib and
// JavaScriptCore in as its own dependencies — dlsym searches those too — but
// naming GTK as well keeps the two packages the build asks for visible here,
// and costs one already-loaded soname lookup.
constexpr const char* kEngineLibs[]{"libwebkitgtk-6.0.so.4", "libgtk-4.so.1"};

bool resolve() {
  void* handles[std::size(kEngineLibs)]{};
  for (std::size_t i = 0; i < std::size(kEngineLibs); ++i) {
    // RTLD_GLOBAL: what a DT_NEEDED link gave WebKit before, and what its own
    // dlopen'd pieces (the injected bundle, GTK's modules) resolve against. No
    // culebra symbol is published by this — only the engine's own.
    handles[i] = dlopen(kEngineLibs[i], RTLD_LAZY | RTLD_GLOBAL);
    if (!handles[i]) return false;
  }

  bool complete = true;
  auto find = [&](const char* name) -> void* {
    for (void* handle : handles) {
      if (void* addr = dlsym(handle, name)) return addr;
    }
    complete = false;
    return nullptr;
  };

  Symbols resolved;
#define CULEBRA_WEBVIEW_RESOLVE(name)                                          \
  resolved.name = reinterpret_cast<decltype(&::name)>(find(#name));
  CULEBRA_WEBVIEW_GTK_SYMBOLS(CULEBRA_WEBVIEW_RESOLVE)
#undef CULEBRA_WEBVIEW_RESOLVE

  // All or nothing: a half-resolved table would let the engine run until it
  // reached the missing call and crash there, which is not a failure shape the
  // other platforms have. Failure leaves the handles open — GTK and WebKit
  // register process-wide state on load and are not safe to dlclose.
  if (!complete) return false;
  sym = resolved;
  return true;
}

// Once per process, on the thread that creates the window; the cached result is
// what every later forwarder reads.
bool engine_loaded() {
  static const bool ok = resolve();
  return ok;
}

}  // namespace

// The forwarders. Only gtk_init_check() can be reached with the table empty —
// it is what fills it — so the rest dereference their slot directly: each is
// reachable only through a window the engine built after this returned TRUE.
//
// That holds across threads too, and not by luck: the one forwarder another
// thread reaches is g_idle_add_full, through the worker-thread `quit()` in
// culebra_rt_webview.cc, and that worker only gets there after loading the
// seq_cst g_active_window the loop thread stored once the window existed. The
// table was filled before that store, so the store/load pair publishes it.
extern "C" {

gboolean gtk_init_check(void) {
  return engine_loaded() ? sym.gtk_init_check() : FALSE;
}

void g_free(gpointer mem) { sym.g_free(mem); }

guint g_idle_add_full(gint priority, GSourceFunc function, gpointer data,
                      GDestroyNotify notify) {
  return sym.g_idle_add_full(priority, function, data, notify);
}

gboolean g_main_context_iteration(GMainContext* context, gboolean may_block) {
  return sym.g_main_context_iteration(context, may_block);
}

gpointer g_object_ref_sink(gpointer object) {
  return sym.g_object_ref_sink(object);
}

void g_object_unref(gpointer object) { sym.g_object_unref(object); }

gulong g_signal_connect_data(gpointer instance, const gchar* detailed_signal,
                             GCallback c_handler, gpointer data,
                             GClosureNotify destroy_data,
                             GConnectFlags connect_flags) {
  return sym.g_signal_connect_data(instance, detailed_signal, c_handler, data,
                                   destroy_data, connect_flags);
}

guint g_signal_handlers_disconnect_matched(gpointer instance,
                                           GSignalMatchType mask,
                                           guint signal_id, GQuark detail,
                                           GClosure* closure, gpointer func,
                                           gpointer data) {
  return sym.g_signal_handlers_disconnect_matched(instance, mask, signal_id,
                                                  detail, closure, func, data);
}

GTypeInstance* g_type_check_instance_cast(GTypeInstance* instance,
                                          GType iface_type) {
  return sym.g_type_check_instance_cast(instance, iface_type);
}

gboolean g_type_check_instance_is_a(GTypeInstance* instance, GType iface_type) {
  return sym.g_type_check_instance_is_a(instance, iface_type);
}

GdkDisplay* gdk_display_get_default(void) {
  return sym.gdk_display_get_default();
}

GType gdk_x11_display_get_type(void) { return sym.gdk_x11_display_get_type(); }

GType gtk_widget_get_type(void) { return sym.gtk_widget_get_type(); }

gboolean gtk_widget_grab_focus(GtkWidget* widget) {
  return sym.gtk_widget_grab_focus(widget);
}

void gtk_widget_set_size_request(GtkWidget* widget, int width, int height) {
  sym.gtk_widget_set_size_request(widget, width, height);
}

void gtk_widget_set_visible(GtkWidget* widget, gboolean visible) {
  sym.gtk_widget_set_visible(widget, visible);
}

void gtk_window_close(GtkWindow* window) { sym.gtk_window_close(window); }

GtkWidget* gtk_window_get_child(GtkWindow* window) {
  return sym.gtk_window_get_child(window);
}

GType gtk_window_get_type(void) { return sym.gtk_window_get_type(); }

GtkWidget* gtk_window_new(void) { return sym.gtk_window_new(); }

void gtk_window_set_child(GtkWindow* window, GtkWidget* child) {
  sym.gtk_window_set_child(window, child);
}

void gtk_window_set_default_size(GtkWindow* window, int width, int height) {
  sym.gtk_window_set_default_size(window, width, height);
}

void gtk_window_set_resizable(GtkWindow* window, gboolean resizable) {
  sym.gtk_window_set_resizable(window, resizable);
}

void gtk_window_set_title(GtkWindow* window, const char* title) {
  sym.gtk_window_set_title(window, title);
}

char* jsc_value_to_string(JSCValue* value) {
  return sym.jsc_value_to_string(value);
}

guint webkit_get_major_version(void) { return sym.webkit_get_major_version(); }

guint webkit_get_minor_version(void) { return sym.webkit_get_minor_version(); }

void webkit_settings_set_enable_developer_extras(WebKitSettings* settings,
                                                 gboolean enabled) {
  sym.webkit_settings_set_enable_developer_extras(settings, enabled);
}

void webkit_settings_set_enable_write_console_messages_to_stdout(
    WebKitSettings* settings, gboolean enabled) {
  sym.webkit_settings_set_enable_write_console_messages_to_stdout(settings,
                                                                  enabled);
}

void webkit_settings_set_javascript_can_access_clipboard(
    WebKitSettings* settings, gboolean enabled) {
  sym.webkit_settings_set_javascript_can_access_clipboard(settings, enabled);
}

void webkit_user_content_manager_add_script(WebKitUserContentManager* manager,
                                            WebKitUserScript* script) {
  sym.webkit_user_content_manager_add_script(manager, script);
}

gboolean webkit_user_content_manager_register_script_message_handler(
    WebKitUserContentManager* manager, const char* name,
    const char* world_name) {
  return sym.webkit_user_content_manager_register_script_message_handler(
      manager, name, world_name);
}

void webkit_user_content_manager_remove_all_scripts(
    WebKitUserContentManager* manager) {
  sym.webkit_user_content_manager_remove_all_scripts(manager);
}

WebKitUserScript* webkit_user_script_new(
    const gchar* source, WebKitUserContentInjectedFrames injected_frames,
    WebKitUserScriptInjectionTime injection_time,
    const gchar* const* allow_list, const gchar* const* block_list) {
  return sym.webkit_user_script_new(source, injected_frames, injection_time,
                                    allow_list, block_list);
}

WebKitUserScript* webkit_user_script_ref(WebKitUserScript* user_script) {
  return sym.webkit_user_script_ref(user_script);
}

void webkit_user_script_unref(WebKitUserScript* user_script) {
  sym.webkit_user_script_unref(user_script);
}

void webkit_web_view_evaluate_javascript(WebKitWebView* web_view,
                                         const char* script, gssize length,
                                         const char* world_name,
                                         const char* source_uri,
                                         GCancellable* cancellable,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data) {
  sym.webkit_web_view_evaluate_javascript(web_view, script, length, world_name,
                                          source_uri, cancellable, callback,
                                          user_data);
}

WebKitSettings* webkit_web_view_get_settings(WebKitWebView* web_view) {
  return sym.webkit_web_view_get_settings(web_view);
}

GType webkit_web_view_get_type(void) { return sym.webkit_web_view_get_type(); }

const gchar* webkit_web_view_get_uri(WebKitWebView* web_view) {
  return sym.webkit_web_view_get_uri(web_view);
}

WebKitUserContentManager* webkit_web_view_get_user_content_manager(
    WebKitWebView* web_view) {
  return sym.webkit_web_view_get_user_content_manager(web_view);
}

void webkit_web_view_load_html(WebKitWebView* web_view, const gchar* content,
                               const gchar* base_uri) {
  sym.webkit_web_view_load_html(web_view, content, base_uri);
}

void webkit_web_view_load_uri(WebKitWebView* web_view, const gchar* uri) {
  sym.webkit_web_view_load_uri(web_view, uri);
}

GtkWidget* webkit_web_view_new(void) { return sym.webkit_web_view_new(); }

}  // extern "C"
