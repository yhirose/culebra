// Headless test for the `culebra lsp` Language Server Protocol server.
//
// Spawns `culebra lsp`, drives one session over its stdin/stdout
// (Content-Length-framed JSON-RPC), and asserts what an editor relies on:
// diagnostics with UTF-16 ranges (after a four-byte character too), a syntax
// error, a line comment ending the buffer, re-analysis after an edit, hover
// over a documented name and none over a value's member, formatting,
// definition (in the document and in an imported module), references with a
// keyword label, highlight, outline, rename and its refusals, an unknown
// request, and an orderly shutdown and exit. No JSON library and no culebra
// linkage — substring checks on the raw stream, in the style of dap_test.
//
// Usage: lsp_test <path-to-culebra>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int to_child = -1, from_child = -1;
static pid_t child = -1;
static std::string acc;  // accumulated stream from the server

static void fail(const std::string& what) {
  std::fprintf(stderr, "lsp_test FAIL: %s\nstream so far:\n%s\n", what.c_str(),
               acc.c_str());
  if (child > 0) ::kill(child, SIGKILL);
  std::exit(1);
}

static void send(const std::string& json) {
  std::string frame =
      "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json;
  size_t off = 0;
  while (off < frame.size()) {
    ssize_t n = ::write(to_child, frame.data() + off, frame.size() - off);
    if (n <= 0) fail("write to server");
    off += static_cast<size_t>(n);
  }
}

// Read until `needle` appears in the accumulated stream, or ~10s pass.
static void read_until(const std::string& needle) {
  if (acc.find(needle) != std::string::npos) return;
  for (int waited = 0; waited < 10000;) {
    struct pollfd p{from_child, POLLIN, 0};
    int r = ::poll(&p, 1, 500);
    if (r < 0) fail("poll");
    if (r == 0) {
      waited += 500;
      continue;
    }
    char buf[4096];
    ssize_t n = ::read(from_child, buf, sizeof(buf));
    if (n <= 0) fail("server closed the stream early");
    acc.append(buf, static_cast<size_t>(n));
    if (acc.find(needle) != std::string::npos) return;
  }
  fail("timed out waiting for: " + needle);
}

static void must_contain(const std::string& needle) {
  if (acc.find(needle) == std::string::npos)
    fail("missing in stream: " + needle);
}

static void spawn_server(const char* culebra) {
  int in_pipe[2], out_pipe[2];
  if (::pipe(in_pipe) || ::pipe(out_pipe)) fail("pipe");
  child = ::fork();
  if (child < 0) fail("fork");
  if (child == 0) {
    ::dup2(in_pipe[0], 0);
    ::dup2(out_pipe[1], 1);
    ::close(in_pipe[0]); ::close(in_pipe[1]);
    ::close(out_pipe[0]); ::close(out_pipe[1]);
    ::execlp(culebra, culebra, "lsp", (char*)nullptr);
    _exit(127);
  }
  ::close(in_pipe[0]);
  ::close(out_pipe[1]);
  to_child = in_pipe[1];
  from_child = out_pipe[0];
}

static std::string uri(const char* name) {
  return std::string("file:///tmp/") + name;
}

static void open_doc(const char* name, const std::string& json_text) {
  send("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{"
       "\"textDocument\":{\"uri\":\"" + uri(name) +
       "\",\"languageId\":\"culebra\",\"version\":1,\"text\":\"" + json_text +
       "\"}}}");
}

static std::string range(int l0, int c0, int l1, int c1) {
  return "\"range\":{\"start\":{\"line\":" + std::to_string(l0) +
         ",\"character\":" + std::to_string(c0) + "},\"end\":{\"line\":" +
         std::to_string(l1) + ",\"character\":" + std::to_string(c1) + "}}";
}

// A request at a position in a document, with extra params appended.
static std::string request(int id, const char* method, const std::string& doc,
                           int line, int character,
                           const std::string& extra = "") {
  return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
         ",\"method\":\"" + method + "\",\"params\":{\"textDocument\":{\"uri\":\"" +
         doc + "\"},\"position\":{\"line\":" + std::to_string(line) +
         ",\"character\":" + std::to_string(character) + "}" + extra + "}}";
}

// The response to request `id`, up to the next message.
static std::string response(int id) {
  std::string marker = "\"id\":" + std::to_string(id) + ",";
  size_t at = acc.find(marker);
  if (at == std::string::npos) fail("no response to request " + std::to_string(id));
  size_t end = acc.find("Content-Length:", at);
  return acc.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

static void response_contains(int id, const std::string& needle) {
  read_until("\"id\":" + std::to_string(id) + ",");
  if (response(id).find(needle) == std::string::npos)
    fail("response " + std::to_string(id) + " lacks: " + needle);
}

static void write_file(const std::string& path, const char* text) {
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) fail("cannot write " + path);
  std::fputs(text, f);
  std::fclose(f);
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: lsp_test <culebra>\n");
    return 2;
  }
  ::signal(SIGPIPE, SIG_IGN);
  spawn_server(argv[1]);

  send("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}");
  read_until("\"id\":1,\"result\"");
  must_contain("\"hoverProvider\":true");
  must_contain("\"documentFormattingProvider\":true");
  must_contain("\"renameProvider\":{\"prepareProvider\":true}");
  send("{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");

  // A lint error, ranged over the identifier.
  open_doc("lsp_undef.cul", "fn f() {\\n  undefined_name\\n}\\nf()\\n");
  read_until("undefined variable 'undefined_name'");
  must_contain(range(1, 2, 1, 16));

  // After a four-byte character the UTF-16 column is not the byte column:
  // `nope` is byte 16 but UTF-16 unit 14.
  open_doc("lsp_emoji.cul", "let s = '\xf0\x9f\x98\x80'; nope\\n");
  read_until("undefined variable 'nope'");
  must_contain(range(0, 14, 0, 18));

  // A syntax error after the same character: `)` is UTF-16 unit 13.
  open_doc("lsp_syntax.cul", "let s = '\xf0\x9f\x98\x80' )\\n");
  read_until("unexpected ')'");
  must_contain(range(0, 13, 0, 14));

  // A buffer whose last line is a comment still in progress parses clean.
  open_doc("lsp_eof.cul", "print(1) # end");
  read_until(uri("lsp_eof.cul") + "\",\"version\":1,\"diagnostics\":[]");

  // An edit is analysed once edits pause.
  send("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{"
       "\"textDocument\":{\"uri\":\"" + uri("lsp_eof.cul") +
       "\",\"version\":2},\"contentChanges\":[{\"text\":\"let x = (\\n\"}]}}");
  read_until(uri("lsp_eof.cul") + "\",\"version\":2,\"diagnostics\":[{");

  // Hover over a documented name.
  open_doc("lsp_hover.cul", "let xs = [Math.abs(-1)]\\nxs.size()\\n");
  send("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/hover\",\"params\":{"
       "\"textDocument\":{\"uri\":\"" + uri("lsp_hover.cul") +
       "\"},\"position\":{\"line\":0,\"character\":16}}}");
  read_until("\"id\":2,\"result\"");
  must_contain("Math.abs(x: Long|Float) -> Long|Float");
  must_contain(range(0, 10, 0, 18));

  // A value's member names no documented entry without its type.
  send("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/hover\",\"params\":{"
       "\"textDocument\":{\"uri\":\"" + uri("lsp_hover.cul") +
       "\"},\"position\":{\"line\":1,\"character\":4}}}");
  read_until("\"id\":3,\"result\":null");

  // Formatting replaces the whole document.
  open_doc("lsp_fmt.cul", "print( 1 )");
  send("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"textDocument/formatting\","
       "\"params\":{\"textDocument\":{\"uri\":\"" + uri("lsp_fmt.cul") +
       "\"},\"options\":{\"tabSize\":2,\"insertSpaces\":true}}}");
  read_until("\"id\":4,\"result\"");
  must_contain("\"newText\":\"print(1)\\n\"");
  must_contain(range(0, 0, 0, 10));

  // Navigation over the names the resolver binds. Lines of lsp_nav.cul:
  //   0 fn total(count) {       3 }
  //   1   let sum = count       4 let sum = total(count: 2)
  //   2   sum                   5 let o = {sum}
  const std::string nav = uri("lsp_nav.cul");
  open_doc("lsp_nav.cul",
           "fn total(count) {\\n  let sum = count\\n  sum\\n}\\n"
           "let sum = total(count: 2)\\nlet o = {sum}\\nprint(o)\\n");
  send(request(10, "textDocument/definition", nav, 2, 2));
  response_contains(10, "{\"uri\":\"" + nav + "\"," + range(1, 6, 1, 9) + "}");

  // A parameter's references include the keyword label at a direct call.
  send(request(11, "textDocument/references", nav, 0, 9,
               ",\"context\":{\"includeDeclaration\":true}"));
  response_contains(11, range(0, 9, 0, 14));
  response_contains(11, range(1, 12, 1, 17));
  response_contains(11, range(4, 16, 4, 21));

  send(request(12, "textDocument/documentHighlight", nav, 4, 4));
  response_contains(12, range(4, 4, 4, 7) + ",\"kind\":3");
  response_contains(12, range(5, 9, 5, 12) + ",\"kind\":2");

  send("{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"textDocument/documentSymbol\","
       "\"params\":{\"textDocument\":{\"uri\":\"" + nav + "\"}}}");
  response_contains(13, "\"name\":\"total\",\"kind\":12");
  response_contains(13, "\"name\":\"o\",\"kind\":13");

  // Rename keeps an object shorthand's key.
  send(request(14, "textDocument/rename", nav, 4, 4, ",\"newName\":\"result\""));
  response_contains(14, range(4, 4, 4, 7) + ",\"newText\":\"result\"");
  response_contains(14, range(5, 9, 5, 12) + ",\"newText\":\"sum: result\"");

  send(request(15, "textDocument/rename", nav, 1, 6, ",\"newName\":\"count\""));
  response_contains(15, "already names something visible");

  // A function also called as a method may be reached by UFCS: no rename.
  open_doc("lsp_ufcs.cul", "fn double(v) { v * 2 }\\nprint([3].double())\\n");
  send(request(16, "textDocument/prepareRename", uri("lsp_ufcs.cul"), 0, 3));
  response_contains(16, "also called as a method");

  // `Alias.member` on an import goes to the imported module's declaration.
  const char* tmp = std::getenv("TMPDIR");
  char real[PATH_MAX];
  if (!::realpath(tmp && *tmp ? tmp : "/tmp", real)) fail("realpath of TMPDIR");
  std::string dir = real;
  write_file(dir + "/lsp_mod_helper.cul", "fn helper() { 1 }\n");
  const std::string main_uri = "file://" + dir + "/lsp_mod_main.cul";
  send("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{"
       "\"textDocument\":{\"uri\":\"" + main_uri +
       "\",\"languageId\":\"culebra\",\"version\":1,\"text\":\""
       "import H from './lsp_mod_helper.cul'\\nH.helper()\\n\"}}}");
  send(request(17, "textDocument/definition", main_uri, 1, 3));
  response_contains(17, "{\"uri\":\"file://" + dir + "/lsp_mod_helper.cul\"," +
                            range(0, 3, 0, 9) + "}");

  send("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"textDocument/nonexistent\","
       "\"params\":{}}");
  read_until("\"id\":5,\"error\":{\"code\":-32601");

  send("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"shutdown\"}");
  read_until("\"id\":6,\"result\":null");
  send("{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");
  int status = 0;
  for (int i = 0; i < 50; i++) {
    if (::waitpid(child, &status, WNOHANG) == child) {
      child = -1;
      break;
    }
    ::usleep(100000);
  }
  if (child > 0) fail("server did not exit after `exit`");
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    fail("server exit status after shutdown was not 0");

  std::printf("lsp_test OK\n");
  return 0;
}
