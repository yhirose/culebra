// Unit test for include/stdlib/search_model.h: how `Search.segmenter` turns a
// model name into a cached file. The store's confirm and fetch are fakes, so
// every path -- a hit, a miss offline, a miss declined, a fetch that fails, a
// download with the wrong digest, and a fetch that lands -- runs offline and
// deterministically. argv[1] is the reference model (the bytes the catalog's
// digest names); the test reads it once and serves it from memory.

#include <stdlib/search_model.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using culebra::CulebraError;
using culebra::search::ModelStore;
using culebra::search::resolve_model;

namespace {

int failures = 0;

#define CHECK(cond)                                                  \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
      failures++;                                                    \
    }                                                                \
  } while (0)

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

bool contains(const std::string& s, std::string_view needle) {
  return s.find(needle) != std::string::npos;
}

// The message of the SearchError `resolve_model` throws, or "" when it
// returns instead.
std::string miss(std::string_view name, const ModelStore& store) {
  try {
    resolve_model(name, store);
  } catch (const CulebraError& e) {
    CHECK(e.kind == "SearchError");
    return e.what();
  }
  return "";
}

// A store that records what was asked of it.
struct Fake {
  ModelStore store;
  int asked = 0, fetched = 0;
  bool answer = false;
  bool reachable = true;
  std::string body;

  explicit Fake(const fs::path& dir) {
    store.dir = dir;
    store.confirm = [this](std::string_view) { return ++asked, answer; };
    store.fetch = [this](const std::string& url, std::string& out,
                         std::string& err) {
      ++fetched;
      CHECK(url.starts_with("https://raw.githubusercontent.com/yhirose/"
                            "cpp-segmentlib/v0.1.1/"));
      if (!reachable) return err = "could not reach " + url, false;
      out = body;
      return true;
    };
  }
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: search_model_test <ja-ud-gsd.mod>\n");
    return 2;
  }
  const std::string model_bytes = read_file(argv[1]);
  CHECK(model_bytes.size() == 2164434);

  auto dir = fs::temp_directory_path() / "culebra-search-model-test";
  fs::remove_all(dir);
  const auto cached = dir / "ja-ud-gsd-v0.1.1.mod";

  // A path is a path: nothing is looked up, nothing is asked.
  {
    Fake f(dir);
    CHECK(resolve_model("models/mine.mod", f.store) == "models/mine.mod");
    CHECK(resolve_model("ja-ud-gsd.mod", f.store) == "ja-ud-gsd.mod");
    CHECK(f.asked == 0 && f.fetched == 0);
  }

  // Offline: a miss is reported without asking or fetching, and the hint
  // names the URL and the file this machine would look for.
  {
    Fake f(dir);
    f.store.offline = true;
    auto m = miss("ja-ud-gsd", f.store);
    CHECK(contains(m, "CULEBRA_OFFLINE"));
    CHECK(contains(m, "models/mlp/ja-ud-gsd.mod"));
    CHECK(contains(m, cached.string()));
    CHECK(f.asked == 0 && f.fetched == 0);
  }

  // Declined (which is also what "not a terminal" looks like): no fetch.
  {
    Fake f(dir);
    auto m = miss("ja-ud-gsd", f.store);
    CHECK(contains(m, "not confirmed"));
    CHECK(f.asked == 1 && f.fetched == 0);
    CHECK(!fs::exists(dir));
  }

  // Accepted, but unreachable: the transport's reason is in the message.
  {
    Fake f(dir);
    f.answer = true;
    f.reachable = false;
    auto m = miss("ja-ud-gsd", f.store);
    CHECK(contains(m, "could not reach"));
    CHECK(f.asked == 1 && f.fetched == 1);
    CHECK(!fs::exists(cached));
  }

  // Accepted, but the bytes are not the pinned model: refused before any
  // file exists, so nothing half-right is left for the next run to load.
  {
    Fake f(dir);
    f.answer = true;
    f.body = model_bytes.substr(0, 1000);
    auto m = miss("ja-ud-gsd", f.store);
    CHECK(contains(m, "does not match its digest"));
    CHECK(!fs::exists(cached));
    CHECK(!fs::exists(dir / "NOTICE"));
  }

  // Accepted and intact: the model lands under its versioned name with the
  // NOTICE beside it, and no temporary is left behind.
  {
    Fake f(dir);
    f.answer = true;
    f.body = model_bytes;
    CHECK(resolve_model("ja-ud-gsd", f.store) == cached.string());
    CHECK(f.asked == 1 && f.fetched == 1);
    CHECK(read_file(cached) == model_bytes);
    CHECK(contains(read_file(dir / "NOTICE"), "CC BY-SA 4.0"));
    int entries = 0;
    for (const auto& e : fs::directory_iterator(dir)) (void)e, ++entries;
    CHECK(entries == 2);
  }

  // Cached: found without asking, fetching, or caring about being offline.
  {
    Fake f(dir);
    f.store.offline = true;
    CHECK(resolve_model("ja-ud-gsd", f.store) == cached.string());
    CHECK(f.asked == 0 && f.fetched == 0);
  }

  fs::remove_all(dir);
  if (failures == 0) std::printf("search_model_test: all passed\n");
  return failures == 0 ? 0 : 1;
}
