#include "docs_cmd.h"

#include <regexlib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "docs_embedded.h"
#include "term.h"

namespace culebra {
namespace {

using docs::kTopicCount;
using docs::kTopics;
using docs::Topic;

// Printing a whole section is affordable because sections are small: across
// language.md the gap between headings is 21 lines at the median. The cap is
// for the long tail (the largest is ~244 lines).
constexpr size_t kSectionLineCap = 60;

// Past this many hits a search reports headings instead of bodies, so a broad
// pattern degrades into an index rather than dumping the reference set.
constexpr size_t kFullOutputLimit = 8;

// How many heading hits still print in full once that limit is passed. A
// pattern matching 20 sections usually matches 1-2 *headings*, and those are
// the ones the reader wanted; printing them beats a pure index that costs a
// second round trip.
constexpr size_t kHeadingsShownInFull = 3;

// The index is a safety valve, so it needs a bound of its own: a pattern like
// `the` matches nearly every section in the set, and an unbounded index is
// tens of thousands of tokens even without a single body printed.
constexpr size_t kIndexLimit = 40;

enum class Rank { None = 0, Body = 1, Code = 2, Heading = 3 };

struct Section {
  size_t first = 0;  // inclusive line index of the heading
  size_t last = 0;   // inclusive line index of the last line
  Rank rank = Rank::None;
};

struct Doc {
  const Topic* topic = nullptr;
  std::string file;  // "stdlib.md" — a basename, because the release archive
                     // ships no docs/ directory for a path to point into
  std::vector<std::string_view> lines;
  std::vector<bool> in_code;
  std::vector<bool> is_heading;
  std::vector<Section> sections;
};

std::vector<std::string_view> split_lines(std::string_view s) {
  std::vector<std::string_view> out;
  size_t pos = 0;
  while (pos <= s.size()) {
    size_t nl = s.find('\n', pos);
    if (nl == std::string_view::npos) {
      out.push_back(s.substr(pos));
      break;
    }
    out.push_back(s.substr(pos, nl - pos));
    pos = nl + 1;
  }
  return out;
}

bool is_atx_heading(std::string_view l) {
  size_t h = 0;
  while (h < l.size() && l[h] == '#') h++;
  return h >= 1 && h <= 6 && h < l.size() && l[h] == ' ';
}

// `====` / `----` under a line of text. Only guide.md, llm.md, tooling.md and
// deployment.md use this form, but their part titles are exactly the headings
// a reader searches for, so the splitter cannot skip it.
bool is_setext_rule(std::string_view l) {
  if (l.size() < 2) return false;
  char c = l[0];
  if (c != '=' && c != '-') return false;
  return l.find_first_not_of(c) == std::string_view::npos;
}

bool blank(std::string_view l) {
  return l.find_first_not_of(" \t") == std::string_view::npos;
}

Doc build_doc(const Topic& t) {
  Doc d;
  d.topic = &t;
  d.file = std::string(t.name) +
           (std::string_view(t.lang) == "ja" ? ".ja.md" : ".md");
  d.lines = split_lines(t.text);
  d.in_code.assign(d.lines.size(), false);
  d.is_heading.assign(d.lines.size(), false);

  bool fenced = false;
  for (size_t i = 0; i < d.lines.size(); i++) {
    std::string_view l = d.lines[i];
    if (l.starts_with("```")) {
      // The fence line itself is code: it carries the language tag, which is
      // worth matching (`” ```culebra ”` marks the doctest-verified blocks).
      d.in_code[i] = true;
      fenced = !fenced;
      continue;
    }
    d.in_code[i] = fenced;
    if (fenced) continue;
    if (is_atx_heading(l)) {
      d.is_heading[i] = true;
    } else if (is_setext_rule(l) && i > 0 && !blank(d.lines[i - 1]) &&
               !d.is_heading[i - 1] && !d.in_code[i - 1]) {
      d.is_heading[i - 1] = true;
    }
  }

  for (size_t i = 0; i < d.lines.size(); i++) {
    if (!d.is_heading[i] && !d.sections.empty()) continue;
    if (!d.is_heading[i] && i != 0) continue;
    Section s;
    s.first = i;
    d.sections.push_back(s);
  }
  for (size_t k = 0; k < d.sections.size(); k++) {
    d.sections[k].last = (k + 1 < d.sections.size())
                             ? d.sections[k + 1].first - 1
                             : d.lines.size() - 1;
  }
  return d;
}

// Everything the language says is punctuation in a pattern, matching the
// escape set `Regex.escape` uses on the culebra side.
std::string escape_literal(std::string_view pat) {
  static constexpr std::string_view metas = R"(\.^$|?*+()[]{})";
  std::string out;
  for (char c : pat) {
    if (metas.find(c) != std::string_view::npos) out += '\\';
    out += c;
  }
  return out;
}

bool has_upper(std::string_view s) {
  return std::any_of(s.begin(), s.end(),
                     [](unsigned char c) { return c >= 'A' && c <= 'Z'; });
}

// An agent's pattern is usually a fragment of a signature (`get_or_put(`,
// `Math.wrap(x`), which is unbalanced as a regex. Compile it as one, and fall
// back to searching for it literally rather than refusing the query.
std::unique_ptr<reg::Regex> compile_pattern(std::string_view pat,
                                            bool& fell_back) {
  // Smart-case: an all-lowercase pattern matches either case, an explicit
  // capital means the reader meant it.
  std::string flags = has_upper(pat) ? "" : "(?i)";
  fell_back = false;
  try {
    return std::make_unique<reg::Regex>(flags + std::string(pat));
  } catch (const reg::RegexError&) {
  }
  fell_back = true;
  try {
    return std::make_unique<reg::Regex>(flags + escape_literal(pat));
  } catch (const reg::RegexError& e) {
    std::println(stderr, "culebra docs: cannot search for that pattern: {}",
                 e.what());
    return nullptr;
  }
}

bool line_matches(reg::Regex& re, std::string_view l) {
  try {
    return re.search(std::string(l)).matched();
  } catch (const reg::RegexError&) {
    return false;
  }
}

void rank_sections(Doc& d, reg::Regex& re) {
  for (Section& s : d.sections) {
    Rank best = Rank::None;
    for (size_t i = s.first; i <= s.last; i++) {
      if (!line_matches(re, d.lines[i])) continue;
      Rank r = d.is_heading[i] ? Rank::Heading
               : d.in_code[i]  ? Rank::Code
                               : Rank::Body;
      if (r > best) best = r;
      if (best == Rank::Heading) break;
    }
    s.rank = best;
  }
}

std::string_view rstrip(std::string_view l) {
  size_t e = l.find_last_not_of(" \t\r");
  return e == std::string_view::npos ? std::string_view{} : l.substr(0, e + 1);
}

void print_index_line(const Doc& d, const Section& s) {
  std::println("{}:{}  {}", d.file, s.first + 1, rstrip(d.lines[s.first]));
}

void print_section(const Doc& d, const Section& s, bool uncapped) {
  // A chapter heading whose whole content is subsections has nothing under it
  // to print. It is still a useful answer — just a locator, so say it as one
  // rather than emitting a heading followed by blank lines.
  bool empty = true;
  for (size_t i = s.first + 1; i <= s.last && empty; i++) {
    empty = blank(d.lines[i]);
  }
  if (empty) {
    print_index_line(d, s);
    return;
  }

  std::println("{}:{}", d.file, s.first + 1);
  size_t total = s.last - s.first + 1;
  size_t shown = (uncapped || total <= kSectionLineCap) ? total
                                                        : kSectionLineCap;
  for (size_t i = s.first; i < s.first + shown; i++) {
    std::println("{}", rstrip(d.lines[i]));
  }
  if (shown < total) {
    std::println("… (+{} lines: culebra docs {} --at {})", total - shown,
                 d.topic->name, s.first + 1);
  }
  std::println("");
}

// Rough on purpose: the number exists so an agent does not open a 60k-token
// file by accident. Counting ASCII and multibyte text separately is what makes
// one formula fit both editions — a Japanese page is mostly CJK at roughly a
// token per character, but its code blocks and signatures are still ASCII, and
// billing the whole file at either rate is off by 2x.
size_t est_tokens(std::string_view text) {
  size_t ascii = 0, wide = 0;
  for (unsigned char c : text) {
    if (c < 0x80) {
      ascii++;
    } else if ((c & 0xC0) != 0x80) {
      wide++;  // a UTF-8 lead byte — one character
    }
  }
  return static_cast<size_t>(static_cast<double>(ascii) / 3.6) + wide;
}

std::string tokens_label(std::string_view text) {
  size_t t = est_tokens(text);
  if (t < 1000) return std::format("~{} tokens", t);
  return std::format("~{}k tokens", (t + 500) / 1000);
}

// std::format pads to a byte count, which leaves the Japanese list ragged:
// its summaries are short in characters and long in bytes, and CJK occupies
// two cells besides. Pad by what the terminal will actually show.
std::string pad_to(std::string_view s, int cells) {
  int w = _term_detail::width(std::string(s));
  return std::string(s) + std::string(std::max(0, cells - w), ' ');
}

const Topic* find_topic(std::string_view name, bool ja) {
  std::string_view want = ja ? "ja" : "en";
  for (size_t i = 0; i < kTopicCount; i++) {
    if (name == kTopics[i].name && want == kTopics[i].lang) return &kTopics[i];
  }
  return nullptr;
}

void print_list(bool ja, const char* version) {
  std::println("Reference docs embedded in this binary (culebra {}).", version);
  std::println("");
  for (size_t i = 0; i < kTopicCount; i++) {
    const Topic& t = kTopics[i];
    if (std::string_view(t.lang) != (ja ? "ja" : "en")) continue;
    std::println("  {:<11} {} {}", t.name, pad_to(t.summary, 52),
                 tokens_label(t.text));
  }
  std::println("");
  std::println("  culebra docs <topic>          print it");
  std::println(
      "  culebra docs -g <pattern>     search every topic, printing the");
  std::println("                                sections that match");
  std::println("  culebra docs <topic> -g <p>   search one topic");
  std::println("  culebra docs --ja ...         the Japanese edition");
  std::println("");
  std::println(
      "Writing culebra? Read `culebra docs llm` first: it is the one file"
      " that");
  std::println(
      "fits in a prompt, and it lists the habits from other languages that do");
  std::println("not carry over.");
}

void print_usage() {
  std::println("Usage: culebra docs [topic] [-g <pattern>] [options]");
  std::println("");
  std::println("Read the reference docs carried inside this binary.");
  std::println("");
  std::println("Options:");
  std::println("  -g, --grep <pattern>  Print the sections matching <pattern>."
               " Lowercase");
  std::println("                        patterns ignore case; an invalid regex"
               " is searched");
  std::println("                        for literally. Patterns are"
               " identifiers or phrases,");
  std::println("                        not questions.");
  std::println("      --full            Print every match in full, however"
               " many there are");
  std::println("      --at <line>       Print the section containing <line> of"
               " <topic>");
  std::println("      --ja              Read the Japanese edition");
  std::println("      --list            List the topics (the default)");
  std::println("");
  std::println("Exit status: 0 printed something, 1 nothing matched, 2 bad"
               " usage.");
}

int search(std::string_view pattern, const Topic* only, bool ja, bool full) {
  bool fell_back = false;
  auto re = compile_pattern(pattern, fell_back);
  if (!re) return 2;
  if (fell_back) {
    std::println(stderr,
                 "culebra docs: /{}/ is not a valid regex — searching for it"
                 " literally",
                 pattern);
  }

  std::vector<Doc> docs;
  for (size_t i = 0; i < kTopicCount; i++) {
    const Topic& t = kTopics[i];
    if (std::string_view(t.lang) != (ja ? "ja" : "en")) continue;
    if (only && &t != only) continue;
    // llm.md's signature index is generated *from* stdlib.md and language.md,
    // so a corpus-wide search would report every API twice. Searching it on
    // purpose still works: `culebra docs llm -g …`.
    if (!only && std::string_view(t.name) == "llm") continue;
    docs.push_back(build_doc(t));
  }

  struct Hit {
    const Doc* doc;
    const Section* sec;
  };
  std::vector<Hit> hits;
  for (Doc& d : docs) {
    rank_sections(d, *re);
    for (const Section& s : d.sections) {
      if (s.rank != Rank::None) hits.push_back({&d, &s});
    }
  }
  if (hits.empty()) {
    std::println(stderr, "culebra docs: no section matches /{}/", pattern);
    return 1;
  }

  // Rank first, document order second: a heading hit in stdlib.md is what the
  // reader wants ahead of a passing mention in a guide paragraph.
  std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
    return a.sec->rank > b.sec->rank;
  });

  if (full || hits.size() <= kFullOutputLimit) {
    for (const Hit& h : hits) print_section(*h.doc, *h.sec, full);
    return 0;
  }

  size_t in_full = 0;
  while (in_full < hits.size() && in_full < kHeadingsShownInFull &&
         hits[in_full].sec->rank == Rank::Heading) {
    in_full++;
  }
  for (size_t i = 0; i < in_full; i++) {
    print_section(*hits[i].doc, *hits[i].sec, false);
  }
  std::println("{} more sections match /{}/ — headings only. Narrow the"
               " pattern, restrict it",
               hits.size() - in_full, pattern);
  std::println("to one topic (`culebra docs stdlib -g …`), or pass --full.");
  std::println("");
  size_t shown = std::min(hits.size(), in_full + kIndexLimit);
  for (size_t i = in_full; i < shown; i++) {
    print_index_line(*hits[i].doc, *hits[i].sec);
  }
  if (shown < hits.size()) {
    std::println("… and {} more.", hits.size() - shown);
  }
  return 0;
}

int print_at(const Topic& t, long line) {
  Doc d = build_doc(t);
  if (line < 1 || static_cast<size_t>(line) > d.lines.size()) {
    std::println(stderr, "culebra docs: {} has no line {}", d.file, line);
    return 2;
  }
  size_t idx = static_cast<size_t>(line) - 1;
  for (size_t k = d.sections.size(); k-- > 0;) {
    if (d.sections[k].first <= idx) {
      print_section(d, d.sections[k], /*uncapped=*/true);
      return 0;
    }
  }
  return 1;
}

}  // namespace

int run_docs(int argc, const char** argv, const char* version) {
  std::string_view topic_name, pattern;
  bool ja = false, full = false, list = false;
  long at = 0;

  for (int i = 2; i < argc; i++) {
    std::string_view a = argv[i];
    auto value = [&](std::string_view flag) -> const char* {
      if (i + 1 >= argc) {
        std::println(stderr, "culebra docs: {} needs a value", flag);
        return nullptr;
      }
      return argv[++i];
    };
    if (a == "-h" || a == "--help") {
      print_usage();
      return 0;
    } else if (a == "--list") {
      list = true;
    } else if (a == "--ja") {
      ja = true;
    } else if (a == "--full") {
      full = true;
    } else if (a == "-g" || a == "--grep") {
      const char* v = value(a);
      if (!v) return 2;
      pattern = v;
    } else if (a == "--at") {
      const char* v = value(a);
      if (!v) return 2;
      at = std::strtol(v, nullptr, 10);
    } else if (a.starts_with("-")) {
      std::println(stderr, "culebra docs: unknown option {}", a);
      print_usage();
      return 2;
    } else if (topic_name.empty()) {
      topic_name = a;
    } else {
      std::println(stderr, "culebra docs: unexpected argument {}", a);
      return 2;
    }
  }

  const Topic* topic = nullptr;
  if (!topic_name.empty()) {
    topic = find_topic(topic_name, ja);
    if (!topic) {
      std::println(stderr, "culebra docs: no topic named '{}'", topic_name);
      std::println(stderr, "");
      print_list(ja, version);
      return 2;
    }
  }

  if (at) {
    if (!topic) {
      std::println(stderr, "culebra docs: --at needs a topic");
      return 2;
    }
    return print_at(*topic, at);
  }
  if (!pattern.empty()) return search(pattern, topic, ja, full);
  if (topic && !list) {
    std::fwrite(topic->text, 1, std::strlen(topic->text), stdout);
    return 0;
  }
  print_list(ja, version);
  return 0;
}

}  // namespace culebra
