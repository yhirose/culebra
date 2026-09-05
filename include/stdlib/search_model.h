#pragma once

// The models `Search.segmenter` knows by name, and where they are kept.
//
// `Search.segmenter("ja-ud-gsd")` names a model rather than a file: the name
// resolves to a cached copy under the per-user data directory, fetched once
// with the user's consent. Anything that is not a name in the catalog is a
// path, as before. Value-neutral (no JitValue, no http): the store's
// `confirm` and `fetch` are supplied by the caller -- the binding layer wires
// the terminal and http::download in, and test/search_model_test.cc wires
// fakes in -- so the fetch discipline (pinned version, digest before the
// file exists, temporary then rename, NOTICE beside the model) is testable
// without a network.

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>

#include <base/shared.h>  // CulebraError, culebra::format
#include <stdlib/hash.h>  // culebra::hashing::sha256

namespace culebra::search {

struct ModelSpec {
  std::string_view name;     // what Search.segmenter takes
  std::string_view version;  // part of the cached file's name, see below
  std::string_view url;      // pinned to that version, never a branch
  std::string_view sha256;
  std::string_view size;     // for the question, as a person reads it
  std::string_view license;
  std::string_view notice;   // written beside the model, verbatim
};

// cpp-segmentlib's NOTICE for its reference model, as of the pinned release.
// The model is a trained artifact under CC BY-SA 4.0 (UD_Japanese-GSD) and
// BSD (unidic-mecab), separately from the MIT code that loads it.
inline constexpr std::string_view kJaUdGsdNotice =
    R"(The model file in this directory (ja-ud-gsd.mod) is a trained artifact and
carries the licenses of what it was trained on, separately from this
repository's MIT-licensed code. This is the same split spaCy uses for its
ja_core_news models, which are trained on the same treebank.

1. Training corpus: UD_Japanese-GSD

  https://github.com/UniversalDependencies/UD_Japanese-GSD
  Release r2.18 (commit 33e7310b58308e85fd2b33a2fc3ef3e434f821c7)
  License: CC BY-SA 4.0

  The model's parameters are derived from this treebank, so the model file
  is distributed under CC BY-SA 4.0: redistribution or modification of the
  model file must keep this attribution and the same license. This applies
  to the model file only; it places no requirement on code that loads it or
  on segmentation output produced with it.

2. Dictionary: unidic-mecab 2.1.2

  The dictionary word list is compiled into the model file, so the model
  also derives from:

  unidic-mecab 2.1.2
  Copyright (c) 2011-2013, The UniDic Consortium
  https://clrd.ninjal.ac.jp/unidic/

  unidic-mecab is copyrighted free software, released under any of the GPL,
  the LGPL, or the BSD License, at your option. It is used here under the
  BSD License, whose conditions this notice satisfies; the full text is in
  the BSD file of the unidic-mecab source archive (kept under
  corpus/ud-gsd/.unidic by scripts/fetch_unidic_dict.sh).

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED.
)";

// Names are artifacts (language + training corpus), not languages: a second
// Japanese model would be a second name, not a replacement. The version is in
// the cached file's name so an upstream update never swaps the model under an
// index built with the old one -- word boundaries that change mean a
// re-index, and a model that silently changed would mean one nobody knew to
// do. The digest is what makes the pin a pin.
inline constexpr ModelSpec kModels[] = {
    {"ja-ud-gsd", "0.1.1",
     "https://raw.githubusercontent.com/yhirose/cpp-segmentlib/v0.1.1/models/"
     "mlp/ja-ud-gsd.mod",
     "3e08be0d6bcd5a40756caf1589e43763c045b9f333ca3dbf562c59a4e3542091",
     "2.1 MB", "CC BY-SA 4.0", kJaUdGsdNotice},
};

inline const ModelSpec* find_model(std::string_view name) {
  for (const auto& m : kModels)
    if (m.name == name) return &m;
  return nullptr;
}

// `<name>-v<version>.mod`: the file a name resolves to inside the store.
inline std::string model_file_name(const ModelSpec& m) {
  return culebra::format("{}-v{}.mod", m.name, m.version);
}

// Where models live and how one is obtained. The binding layer builds the
// real one (default_model_store in bindings.h); a test builds its own.
struct ModelStore {
  std::filesystem::path dir;  // created on the first fetch
  bool offline = false;       // never fetch; a miss is reported like a "no"
  // Asks before a fetch. Answers false when nobody is there to ask.
  std::function<bool(std::string_view question)> confirm;
  // GET `url` into `body`; false with `err` on failure.
  std::function<bool(const std::string& url, std::string& body,
                     std::string& err)>
      fetch;
};

namespace detail {

// What a miss tells the user, whatever the reason: where the model comes
// from, where this machine looks for it, and the two ways around the fetch.
[[noreturn]] inline void model_missing(const ModelSpec& m,
                                       const std::filesystem::path& file,
                                       std::string_view why) {
  throw CulebraError(
      "SearchError",
      culebra::format(
          "Search.segmenter: model \"{}\" is not cached and was not fetched "
          "({}).\n"
          "  cpp-segmentlib's reference model {} v{} ({}, {}) is at\n"
          "    {}\n"
          "  save it as\n"
          "    {}\n"
          "  or run this program on a terminal, where culebra offers to fetch "
          "it,\n"
          "  or pass the path of a model of your own.",
          m.name, why, m.name, m.version, m.size, m.license, m.url,
          file.string()),
      0, 0);
}

// Fetch, verify, then place: the digest is checked on the bytes in memory,
// so a download that fails it never exists as a file anyone could reach for;
// the file is written under a temporary name and renamed, so a reader never
// sees a partial model; the NOTICE goes down first, so a model on disk
// always has its attribution beside it.
inline void fetch_model(const ModelSpec& m, const std::filesystem::path& file,
                        const ModelStore& store) {
  namespace fs = std::filesystem;
  std::string body, err;
  if (!store.fetch(std::string(m.url), body, err)) model_missing(m, file, err);
  auto got = culebra::hashing::sha256(body);
  if (got != m.sha256) {
    model_missing(m, file,
                  culebra::format("the download does not match its digest: "
                                  "expected {}, got {}",
                                  m.sha256, got));
  }
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  auto write = [&](const fs::path& p, std::string_view bytes) {
    std::ofstream out(p, std::ios::binary);
    out.write(bytes.data(), std::streamsize(bytes.size()));
    out.close();
    if (!out) model_missing(m, file, culebra::format("could not write {}", p.string()));
  };
  write(file.parent_path() / "NOTICE", m.notice);
  // A per-process temporary: two programs fetching at once each rename their
  // own complete copy into place, and the bytes are the same.
  auto part = fs::path(file.string() + culebra::format(".{}.part",
      std::chrono::steady_clock::now().time_since_epoch().count()));
  write(part, body);
  fs::rename(part, file, ec);
  if (ec) {
    fs::remove(part, ec);
    model_missing(m, file, culebra::format("could not place {}", file.string()));
  }
}

}  // namespace detail

// The file `model` denotes: a catalog name resolves to its cached copy,
// fetched after asking when it is not there yet; anything else is a path and
// is returned as it came. Throws SearchError, with the hint above, on a miss
// that was not fetched.
inline std::string resolve_model(std::string_view model,
                                 const ModelStore& store) {
  const auto* m = find_model(model);
  if (!m) return std::string(model);
  auto file = store.dir / model_file_name(*m);
  std::error_code ec;
  if (std::filesystem::is_regular_file(file, ec)) return file.string();
  if (store.offline) detail::model_missing(*m, file, "CULEBRA_OFFLINE is set");
  auto question = culebra::format(
      "Search.segmenter needs a model. culebra can fetch cpp-segmentlib's\n"
      "reference model {} v{} ({}, {}) into\n"
      "  {}\n"
      "Download it?",
      m->name, m->version, m->size, m->license, file.string());
  if (!store.confirm(question)) {
    detail::model_missing(*m, file, "not confirmed, or not on a terminal");
  }
  detail::fetch_model(*m, file, store);
  return file.string();
}

}  // namespace culebra::search
