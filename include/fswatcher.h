#pragma once

// Value-neutral filesystem-watching core for FS.watch. Mirrors http.h /
// sqlite.h: no interpreter/JIT/Value types here, only plain C++ structs and a
// blocking pull API that the three backends (interp / JIT / AOT) each adapt
// into their own iterator representation.
//
// The model is deliberately pull, not push: the OS side only appends POD
// events to a queue, and the script pulls them on its own thread. A push
// design would have to stand a Runtime up on the OS notification thread and
// call a culebra closure there — the wasted work of a rejected event would
// then delay the notification thread itself, which is exactly what provokes
// inotify's IN_Q_OVERFLOW (dropped events). Pulling late only grows the queue.
//
//   fs_watch_open  — start watching a directory; returns an id (>= 0).
//   fs_watch_next  — block for the next event on the calling thread.
//   fs_watch_close — stop the OS side, join, free. Idempotent.
//
// Event kinds are best effort and their granularity is the OS's, not ours:
// FSEvents coalesces several changes to one path inside its latency window
// into a single event carrying the union of the flags, while inotify reports
// each change separately. A rename is never paired — it surfaces as a Deleted
// on the old path and a Created on the new one.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <id_registry.h>  // IdRegistry<T> (slot+generation handle table)
#include <shared.h>       // throw_if_interrupted (Ctrl+C / isolate cancel)

#if defined(__APPLE__)
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#elif defined(__linux__)
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <thread>
#include <unordered_map>
#elif defined(_WIN32)
#include <os_compat.h>  // <windows.h> (guarded) — CreateFileW/ReadDirectoryChangesW

#include <thread>
#endif

namespace culebra::fswatch {

enum class Kind : int { Created = 0, Modified = 1, Deleted = 2 };

// One change, as delivered to the script. `path` is absolute and canonical
// (the watched root is canonicalized at open, and every path is built from
// it), so a caller comparing against its own path has to canonicalize too —
// on macOS a /tmp root reports as /private/tmp.
struct FsEvent {
  std::string path;
  Kind kind = Kind::Modified;
};

// Timeout is only reachable through fs_watch_next's optional deadline, which
// the language binding does not use (FS.watch blocks). It exists so a test can
// bound the wait — otherwise a broken watcher hangs the suite instead of
// failing it — without a second code path through the queue.
enum class Next : int { Event = 0, Closed = 1, Timeout = 2 };

inline const char* kind_name(Kind k) {
  switch (k) {
    case Kind::Created: return "created";
    case Kind::Deleted: return "deleted";
    default: return "modified";
  }
}

// "cul" and ".cul" name the same filter (watchexec's -e). Both backends
// normalize through here so a match: list means the same thing on each.
inline std::string normalize_ext(std::string_view e) {
  std::string s(e);
  if (!s.empty() && s.front() != '.') s.insert(s.begin(), '.');
  return s;
}

// ---- Watcher ---------------------------------------------------------------

struct Watcher {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<FsEvent> queue;
  bool closed = false;  // the OS side stopped on its own (error / EOF)

  std::string root;               // canonical, no trailing separator
  bool recursive = true;
  std::vector<std::string> exts;  // normalized (".cul"); empty = everything

#if defined(__APPLE__)
  FSEventStreamRef stream = nullptr;
  dispatch_queue_t dq = nullptr;
#elif defined(__linux__)
  std::thread os_thread;
  int fd = -1;                 // inotify
  int wake[2] = {-1, -1};      // self-pipe: the only way to break poll()
  std::unordered_map<int, std::string> wd_paths;  // watcher thread only
#elif defined(_WIN32)
  std::thread os_thread;
  HANDLE dir_handle = INVALID_HANDLE_VALUE;
  HANDLE stop_event = nullptr;  // manual-reset; set by platform_stop to break the wait
  // Set once the reader thread has posted its first ReadDirectoryChangesW (or
  // given up trying to). platform_start waits on it before returning, so a
  // caller's write right after FS.watch() cannot land in the gap between
  // "thread spawned" and "kernel is actually watching" — ReadDirectoryChangesW
  // only reports changes from the moment it is posted.
  HANDLE armed_event = nullptr;
#endif
};

namespace detail {

inline bool ext_matches(const std::vector<std::string>& exts,
                        const std::string& path) {
  if (exts.empty()) return true;
  for (const auto& e : exts)
    if (path.ends_with(e)) return true;
  return false;
}

// The one enqueue point. Filtering here — before an event ever reaches the
// queue — is what makes match: a real lever: a rejected event costs a suffix
// compare instead of an object allocation and a loop iteration downstream.
inline void emit(Watcher* w, std::string path, Kind kind) {
  if (!ext_matches(w->exts, path)) return;
  {
    std::lock_guard<std::mutex> lk(w->mu);
    w->queue.push_back(FsEvent{std::move(path), kind});
  }
  w->cv.notify_one();
}

inline void mark_closed(Watcher* w) {
  {
    std::lock_guard<std::mutex> lk(w->mu);
    w->closed = true;
  }
  w->cv.notify_all();
}

// True when `path` sits directly in the watched root — the non-recursive
// filter. FSEvents always reports the whole subtree, so macOS narrows here;
// on Linux a non-recursive watch simply never adds the subdirectories.
inline bool in_root_dir(const Watcher* w, const std::string& path) {
  auto slash = path.rfind('/');
  return slash != std::string::npos &&
         std::string_view(path).substr(0, slash) == w->root;
}

// ---- macOS: FSEvents -------------------------------------------------------

#if defined(__APPLE__)

// FSEvents reports what changed, not the order it changed in: one event can
// carry Created|Modified|Removed at once for a path that was created, written
// and deleted inside the latency window, and a rename raises ItemRenamed on
// both the old and the new path rather than a Removed/Created pair. Existence
// is therefore the ground truth, and the flags only break the tie between a
// create and a write.
inline Kind classify(const std::string& path, FSEventStreamEventFlags f) {
  std::error_code ec;
  if (!std::filesystem::exists(std::filesystem::path(path), ec) || ec)
    return Kind::Deleted;
  if (f & kFSEventStreamEventFlagItemCreated) return Kind::Created;
  return Kind::Modified;
}

inline void fsevents_cb(ConstFSEventStreamRef, void* info, size_t n,
                        void* paths, const FSEventStreamEventFlags flags[],
                        const FSEventStreamEventId[]) {
  auto* w = static_cast<Watcher*>(info);
  auto** names = static_cast<char**>(paths);
  for (size_t i = 0; i < n; i++) {
    FSEventStreamEventFlags f = flags[i];
    // The queue overflowed and the kernel is telling us to rescan instead of
    // naming the paths. Reporting the directory as one change would be a lie
    // in the shape of a file event, so drop it (documented as best effort).
    if (f & (kFSEventStreamEventFlagMustScanSubDirs |
             kFSEventStreamEventFlagHistoryDone))
      continue;
    std::string path(names[i]);
    if (path == w->root) continue;  // the root itself is not "an entry changed"
    if (!w->recursive && !in_root_dir(w, path)) continue;
    // Filter before classify, whose exists() is a stat: this thread falling
    // behind is what pushes FSEvents into MustScanSubDirs, and a match: the
    // caller wrote to narrow the stream should not cost a syscall per reject.
    if (!ext_matches(w->exts, path)) continue;
    Kind kind = classify(path, f);
    emit(w, std::move(path), kind);
  }
}

inline void platform_stop(Watcher* w) {
  if (w->stream) {
    FSEventStreamStop(w->stream);
    FSEventStreamInvalidate(w->stream);
    FSEventStreamRelease(w->stream);
    w->stream = nullptr;
  }
  if (w->dq) {
    // Invalidate detaches the stream, but a callback block already dispatched
    // may still be running against `w`. A sync barrier on the serial queue is
    // what makes freeing `w` below safe rather than a use-after-free.
    dispatch_sync_f(w->dq, nullptr, [](void*) {});
    dispatch_release(w->dq);
    w->dq = nullptr;
  }
}

inline bool platform_start(Watcher* w, std::string& reason) {
  FSEventStreamContext ctx{0, w, nullptr, nullptr, nullptr};
  CFStringRef cf_root = CFStringCreateWithCString(nullptr, w->root.c_str(),
                                                  kCFStringEncodingUTF8);
  if (!cf_root) {
    reason = "cannot encode path";
    return false;
  }
  CFArrayRef roots = CFArrayCreate(nullptr,
                                   reinterpret_cast<const void**>(&cf_root), 1,
                                   &kCFTypeArrayCallBacks);
  // NoDefer delivers the first change of a burst immediately and batches only
  // what follows, so an idle tree reacts at once instead of after the latency.
  w->stream = FSEventStreamCreate(
      nullptr, &fsevents_cb, &ctx, roots, kFSEventStreamEventIdSinceNow, 0.05,
      kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
  CFRelease(roots);
  CFRelease(cf_root);
  if (!w->stream) {
    reason = "cannot create the event stream";
    return false;
  }
  w->dq = dispatch_queue_create("culebra.fswatch", nullptr);
  FSEventStreamSetDispatchQueue(w->stream, w->dq);
  if (!FSEventStreamStart(w->stream)) {
    platform_stop(w);
    reason = "cannot start the event stream";
    return false;
  }
  return true;
}

// ---- Linux: inotify --------------------------------------------------------

#elif defined(__linux__)

constexpr uint32_t kInotifyMask = IN_CREATE | IN_DELETE | IN_MODIFY |
                                  IN_MOVED_FROM | IN_MOVED_TO | IN_DONT_FOLLOW;

inline bool add_dir(Watcher* w, const std::string& dir) {
  int wd = inotify_add_watch(w->fd, dir.c_str(), kInotifyMask);
  if (wd < 0) return false;
  w->wd_paths[wd] = dir;
  return true;
}

// Watch `dir`, then walk it. Walking *after* adding the watch is what closes
// the race where entries appear between the mkdir and the watch: `emit_found`
// reports them as Created (a duplicate is possible and accepted — the event
// stream promises no deduplication). A subdirectory that cannot be watched
// (permissions, or max_user_watches) leaves that part of the tree unreported
// rather than failing the whole watch.
inline void add_tree(Watcher* w, const std::string& dir, bool emit_found) {
  add_dir(w, dir);
  std::error_code ec;
  std::filesystem::directory_iterator it(dir, ec);
  if (ec) return;
  std::error_code step;
  for (auto end = std::filesystem::directory_iterator(); it != end;
       it.increment(step)) {
    if (step) break;  // unreadable entry — stop this dir, keep the watch
    std::error_code q;
    bool is_dir = !it->is_symlink(q) && !q && it->is_directory(q) && !q;
    std::string path = it->path().string();
    if (emit_found) emit(w, path, Kind::Created);
    if (is_dir && w->recursive) add_tree(w, path, emit_found);
  }
}

inline void handle_event(Watcher* w, const struct inotify_event* ev) {
  auto it = w->wd_paths.find(ev->wd);
  if (it == w->wd_paths.end()) return;
  if (ev->mask & IN_IGNORED) {  // the watch is gone (dir deleted / unmounted)
    w->wd_paths.erase(it);
    return;
  }
  if (ev->len == 0) return;  // about the watched directory itself, not an entry
  std::string path = it->second + "/" + ev->name;
  if (ev->mask & (IN_CREATE | IN_MOVED_TO)) {
    emit(w, path, Kind::Created);  // path is reused just below
    if ((ev->mask & IN_ISDIR) && w->recursive) add_tree(w, path, true);
  } else if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) {
    emit(w, std::move(path), Kind::Deleted);
  } else if (ev->mask & IN_MODIFY) {
    emit(w, std::move(path), Kind::Modified);
  }
}

inline void reader_loop(Watcher* w) {
  alignas(struct inotify_event) char buf[64 * 1024];
  for (;;) {
    struct pollfd pfds[2] = {{w->fd, POLLIN, 0}, {w->wake[0], POLLIN, 0}};
    int r = ::poll(pfds, 2, -1);
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pfds[1].revents) break;  // fs_watch_close wrote to the self-pipe
    if (!(pfds[0].revents & POLLIN)) continue;
    ssize_t n = ::read(w->fd, buf, sizeof(buf));
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      break;
    }
    constexpr ssize_t kHdr = static_cast<ssize_t>(sizeof(struct inotify_event));
    for (ssize_t off = 0; off + kHdr <= n;) {
      auto* ev = reinterpret_cast<struct inotify_event*>(buf + off);
      off += kHdr + static_cast<ssize_t>(ev->len);
      handle_event(w, ev);
    }
  }
  mark_closed(w);
}

inline void platform_stop(Watcher* w) {
  if (w->os_thread.joinable()) {
    // A thread parked in read() on the inotify fd could never be told to
    // stop, so the loop polls this pipe alongside it and close knocks here —
    // otherwise the join below would wait for a change that may never come.
    char b = 0;
    ssize_t ignored = ::write(w->wake[1], &b, 1);
    (void)ignored;
    w->os_thread.join();
  }
  if (w->fd >= 0) { ::close(w->fd); w->fd = -1; }
  for (int& e : w->wake)
    if (e >= 0) { ::close(e); e = -1; }
}

inline bool platform_start(Watcher* w, std::string& reason) {
  w->fd = inotify_init1(IN_CLOEXEC);
  if (w->fd < 0) {
    reason = std::string("inotify_init1: ") + std::strerror(errno);
    return false;
  }
  if (::pipe(w->wake) != 0) {
    ::close(w->fd);
    w->fd = -1;
    reason = std::string("pipe: ") + std::strerror(errno);
    return false;
  }
  // The root's own watch is the one failure worth reporting: a subdirectory
  // that cannot be watched only narrows coverage, but a root that cannot be
  // watched means the call did nothing.
  if (!add_dir(w, w->root)) {
    reason = errno == ENOSPC
                 ? "inotify watch limit reached (see "
                   "/proc/sys/fs/inotify/max_user_watches)"
                 : std::string("inotify_add_watch: ") + std::strerror(errno);
    platform_stop(w);
    return false;
  }
  // Re-adds the root's watch, which inotify_add_watch answers with the same
  // wd — cheaper than a second copy of the walk that would then have to be
  // kept in step with add_tree's.
  if (w->recursive) add_tree(w, w->root, /*emit_found=*/false);
  w->os_thread = std::thread(reader_loop, w);
  return true;
}

// ---- Windows: ReadDirectoryChangesW -----------------------------------------

#elif defined(_WIN32)

constexpr DWORD kNotifyFilter =
    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
    FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;

inline Kind classify(DWORD action) {
  switch (action) {
    case FILE_ACTION_REMOVED:
    case FILE_ACTION_RENAMED_OLD_NAME: return Kind::Deleted;
    case FILE_ACTION_ADDED:
    case FILE_ACTION_RENAMED_NEW_NAME: return Kind::Created;
    default: return Kind::Modified;
  }
}

inline void reader_loop(Watcher* w) {
  std::vector<BYTE> buf(64 * 1024);  // the documented sync/overlapped ceiling
  OVERLAPPED ov{};
  ov.hEvent = CreateEventW(nullptr, /*manualReset=*/TRUE, FALSE, nullptr);
  if (!ov.hEvent) { SetEvent(w->armed_event); mark_closed(w); return; }

  bool first = true;
  for (;;) {
    ResetEvent(ov.hEvent);
    DWORD unused = 0;  // must be NULL-ish (unread) for an overlapped call; MSDN
    BOOL ok = ReadDirectoryChangesW(
        w->dir_handle, buf.data(), static_cast<DWORD>(buf.size()),
        w->recursive ? TRUE : FALSE, kNotifyFilter, &unused, &ov, nullptr);
    // Signal platform_start the moment the first post is resolved, however it
    // resolved: once ReadDirectoryChangesW has been issued, changes from here
    // on are captured, which is the guarantee a caller's write-right-after-
    // FS.watch() needs — the read completing is a separate, later event.
    if (first) { SetEvent(w->armed_event); first = false; }
    if (!ok && GetLastError() != ERROR_IO_PENDING) break;

    HANDLE waits[2] = {ov.hEvent, w->stop_event};
    DWORD r = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (r != WAIT_OBJECT_0) {
      // Stop requested (or the wait itself failed). Cancel the pending read
      // and drain its completion before this thread exits — otherwise the
      // kernel could still be writing into `buf` after it is freed.
      CancelIoEx(w->dir_handle, &ov);
      DWORD drained = 0;
      GetOverlappedResult(w->dir_handle, &ov, &drained, TRUE);
      break;
    }
    DWORD bytes = 0;
    if (!GetOverlappedResult(w->dir_handle, &ov, &bytes, FALSE)) break;
    if (bytes == 0) continue;  // buffer overflowed — best effort, drop it

    size_t off = 0;
    for (;;) {
      auto* rec = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buf.data() + off);
      std::wstring_view name(rec->FileName,
                              rec->FileNameLength / sizeof(WCHAR));
      // path::operator/= joins with the native separator and understands the
      // backslashes ReadDirectoryChangesW reports subtree entries with (e.g.
      // "sub\deep.txt") as component boundaries. Plain string concatenation
      // of w->root (native, backslash) with a forward-slash-forced name would
      // mix separator styles within one path — inconsistent with every other
      // absolute path this program builds (FS.join, FS.realpath, ...).
      std::string path = (std::filesystem::path(w->root) / name).string();
      emit(w, std::move(path), classify(rec->Action));
      if (rec->NextEntryOffset == 0) break;
      off += rec->NextEntryOffset;
    }
  }
  CloseHandle(ov.hEvent);
  mark_closed(w);
}

inline void platform_stop(Watcher* w) {
  if (w->os_thread.joinable()) {
    if (w->stop_event) SetEvent(w->stop_event);
    w->os_thread.join();
  }
  if (w->stop_event) { CloseHandle(w->stop_event); w->stop_event = nullptr; }
  if (w->armed_event) { CloseHandle(w->armed_event); w->armed_event = nullptr; }
  if (w->dir_handle != INVALID_HANDLE_VALUE) {
    CloseHandle(w->dir_handle);
    w->dir_handle = INVALID_HANDLE_VALUE;
  }
}

inline bool platform_start(Watcher* w, std::string& reason) {
  // FILE_FLAG_BACKUP_SEMANTICS is what lets CreateFileW open a directory
  // rather than a file; FILE_FLAG_OVERLAPPED is what makes the read
  // cancellable (platform_stop cannot otherwise interrupt a blocked thread).
  std::wstring wroot = std::filesystem::path(w->root).wstring();
  w->dir_handle = CreateFileW(
      wroot.c_str(), FILE_LIST_DIRECTORY,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
      nullptr);
  if (w->dir_handle == INVALID_HANDLE_VALUE) {
    reason = "CreateFileW: " + std::system_category().message(GetLastError());
    return false;
  }
  w->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!w->stop_event) {
    reason = "CreateEventW: " + std::system_category().message(GetLastError());
    CloseHandle(w->dir_handle);
    w->dir_handle = INVALID_HANDLE_VALUE;
    return false;
  }
  w->armed_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!w->armed_event) {
    reason = "CreateEventW: " + std::system_category().message(GetLastError());
    CloseHandle(w->stop_event);
    w->stop_event = nullptr;
    CloseHandle(w->dir_handle);
    w->dir_handle = INVALID_HANDLE_VALUE;
    return false;
  }
  w->os_thread = std::thread(reader_loop, w);
  // Block until the reader thread's first ReadDirectoryChangesW is posted (or
  // has given up trying): only then is a write into `w->root` guaranteed to
  // be observed, closing the gap a caller's write-right-after-FS.watch()
  // could otherwise land in silently.
  WaitForSingleObject(w->armed_event, INFINITE);
  return true;
}

// ---- Unsupported platform --------------------------------------------------

#else

inline void platform_stop(Watcher*) {}

inline bool platform_start(Watcher*, std::string& reason) {
  reason = "not supported on this platform";
  return false;
}

#endif

// ---- Handle table ----------------------------------------------------------
//
// Script-visible handles carry an int64 id from an IdRegistry, never a
// Watcher*, so a forged or stale id fails safely (see id_registry.h). The
// table is thread_local because a watch handle is __nonsendable__ and so
// never crosses a thread — same reasoning as sqlite.h's db/stmt tables.
//
// Its destructor is the backstop for a thread that ends with a watch still
// open (an isolate, say): the registry holds raw Watcher*, so without it the
// Watcher and its running OS notification thread would live to process exit.
// Top-level script handles are reaped earlier and deterministically, by
// fs_watch_close_all.
struct Table {
  IdRegistry<Watcher> reg;
  ~Table();
};

inline Table& table() {
  static thread_local Table t;
  return t;
}

inline void destroy(Watcher* w) {
  platform_stop(w);
  delete w;
}

inline void close_all(Table& t) {
  for (uint32_t s = 0; s < t.reg.slots.size(); s++) {
    Watcher* w = t.reg.slots[s];
    if (!w) continue;
    t.reg.invalidate(t.reg.id_at(s));
    destroy(w);
  }
}

inline Table::~Table() { close_all(*this); }

}  // namespace detail

// ---- Public API ------------------------------------------------------------

// Start watching `path` (a directory). Returns an id, or -1 with a complete
// user-facing message in `err` — complete so that both backends raise the
// identical text rather than each formatting its own.
inline int64_t fs_watch_open(const std::string& path, bool recursive,
                             const std::vector<std::string>& exts,
                             std::string& err) {
  auto fail = [&](std::string_view reason) {
    err = culebra::format("FS.watch('{}'): {}", path, reason);
    return int64_t{-1};
  };

  std::error_code ec;
  auto canonical = std::filesystem::canonical(std::filesystem::path(path), ec);
  if (ec) return fail(ec.message());
  if (!std::filesystem::is_directory(canonical, ec) || ec)
    return fail("not a directory");

  auto* w = new Watcher();
  w->root = canonical.string();
  while (w->root.size() > 1 && w->root.back() == '/') w->root.pop_back();
  w->recursive = recursive;
  w->exts = exts;

  std::string reason;
  if (!detail::platform_start(w, reason)) {
    delete w;
    return fail(reason);
  }
  return detail::table().reg.add(w);
}

// Block until the next event, the watch closes, or an interrupt arrives.
// Waking every 200ms to poll the interrupt flag is what makes a single Ctrl+C
// (or an isolate cancel) break out of a `for e in w` that is simply waiting —
// the same discipline as the interruptible stdin read in shared.h.
// A non-negative `timeout_ms` bounds the wait and reports Timeout instead.
inline Next fs_watch_next(int64_t id, FsEvent& out, int64_t timeout_ms = -1) {
  auto* w = detail::table().reg.get(id);
  if (!w) return Next::Closed;
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
  std::unique_lock<std::mutex> lk(w->mu);
  for (;;) {
    if (!w->queue.empty()) {
      out = std::move(w->queue.front());
      w->queue.pop_front();
      return Next::Event;
    }
    if (w->closed) return Next::Closed;
    lk.unlock();
    throw_if_interrupted();
    lk.lock();
    if (!w->queue.empty() || w->closed) continue;
    auto slice = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    if (timeout_ms >= 0) {
      if (deadline <= std::chrono::steady_clock::now()) return Next::Timeout;
      slice = std::min(slice, deadline);
    }
    w->cv.wait_until(lk, slice);
  }
}

// Stop the OS side, join, free. Idempotent; a forged or already-closed id is
// a no-op.
inline void fs_watch_close(int64_t id) {
  auto& t = detail::table();
  auto* w = t.reg.get(id);
  if (!w) return;
  t.reg.invalidate(id);
  detail::destroy(w);
}

// Close every watch still open on this thread. The backends call this when a
// script run ends: a top-level `let w = FS.watch(...)` never runs its drop
// (docs/language.md — top-level bindings are alive at exit, on every backend),
// which for a watch would leave an OS thread and its stream running into
// process teardown.
inline void fs_watch_close_all() { detail::close_all(detail::table()); }

}  // namespace culebra::fswatch
