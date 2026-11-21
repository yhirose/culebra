#pragma once

// A WebSocket's connection apart from what carries it: what a WebSocket
// handle (bindings.h) reads and sends, whoever is at the other end. An Http
// server's or client's socket (http.h) and the browser page a program runs in
// (Page.socket, playground's page_web.h) are each a Link. Value-neutral and
// free of httplib, so every build has it, one without Http too.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <base/id_registry.h>

namespace culebra::ws {

// One connection's transport.
struct Link {
  virtual ~Link() = default;
  // 1 with a message in `out`, 0 once closed, -1 when the read timeout
  // elapsed with the connection still open (only with one set; a page's
  // ends early when a MIDI message wakes it).
  virtual int receive(std::string& out) = 0;
  // How long receive() waits; 0 waits forever.
  virtual void set_read_timeout(int64_t ms) = 0;
  // Sends a text message; false once the connection failed or closed.
  virtual bool send(const char* data, size_t len) = 0;
  virtual void close() = 0;
  virtual bool is_open() = 0;
};

// The connections, by id. Per thread, as a handle is not Sendable. A
// function-local thread_local: the core runtime archive and a feature archive
// both reach it, and only a namespace-scope one carries a TLS initializer of
// its own into each (tools/checks/check_rt_archive_tls.sh).
inline IdRegistry<Link>& links() {
  static thread_local IdRegistry<Link> table;
  return table;
}

inline int64_t add(std::unique_ptr<Link> link) {
  return links().add(link.release());
}
// Forgets `id` and frees its Link; a stale id is a no-op.
inline void remove(int64_t id) {
  if (Link* l = links().get(id)) {
    links().invalidate(id);
    delete l;
  }
}

// Frees every connection there is: a program's run ends and what it left
// open goes with it, where one process runs program after program (the
// Playground's worker).
inline void remove_all() {
  auto& table = links();
  for (uint32_t s = 0; s < table.slots.size(); ++s)
    if (table.slots[s]) remove(table.id_at(s));
}

// What a handle asks, by id: a stale one reads as closed.
inline int receive(int64_t id, std::string& out) {
  Link* l = links().get(id);
  return l ? l->receive(out) : 0;
}
inline bool set_timeout(int64_t id, int64_t ms) {
  Link* l = links().get(id);
  if (!l) return false;
  l->set_read_timeout(ms);
  return true;
}
inline bool send(int64_t id, const char* data, size_t len) {
  Link* l = links().get(id);
  return l ? l->send(data, len) : false;
}
inline void close(int64_t id) {
  if (Link* l = links().get(id)) l->close();
}
inline bool is_open(int64_t id) {
  Link* l = links().get(id);
  return l ? l->is_open() : false;
}

// The page this program runs in (Page.socket), or null with `err` saying why.
// A browser page runs one: the Playground's worker build defines this in
// playground/page_web.h. Nothing else has a page.
#if defined(__EMSCRIPTEN__)
std::unique_ptr<Link> page_link(std::string& err);
#else
inline std::unique_ptr<Link> page_link(std::string& err) {
  err = "no page runs this program (a page in a browser does, as the Playground's)";
  return nullptr;
}
#endif

}  // namespace culebra::ws
