#pragma once

#include <iomanip>
#include <linenoise.hpp>
#include "culebra.h"

namespace culebra {

class CommandLineDebugger {
 public:
  void operator()(const peg::Ast& ast, Environment& env, bool force_to_break) {
    if (quit_) {
      return;
    }

    if ((command_ == "n" && env.level <= level_) || (command_ == "s") ||
        (command_ == "o" && env.level < level_)) {
      force_to_break = true;
    }

    if (force_to_break) {
      static auto show_initial_usage = true;
      if (show_initial_usage) {
        show_initial_usage = false;
        usage();
      }

      show_lines(ast);

      for (;;) {
        std::cout << std::endl << "debug> ";

        std::string s;
        std::getline(std::cin, s);

        std::istringstream is(s);
        is >> command_;

        if (command_ == "h") {
          usage();
        } else if (command_ == "l") {
          is >> display_lines_;
          show_lines(ast);
        } else if (command_ == "p") {
          std::string symbol;
          is >> symbol;
          print(ast, env, symbol);
        } else if (command_ == "c") {
          break;
        } else if (command_ == "n") {
          break;
        } else if (command_ == "s") {
          break;
        } else if (command_ == "o") {
          break;
        } else if (command_ == "q") {
          quit_ = true;
          break;
        }
      }
      level_ = env.level;
      ;
    }
  }

 private:
  void show_lines(const peg::Ast& ast) {
    prepare_cache(ast.path);

    std::cout << std::endl
              << "Break in " << ast.path << ":" << ast.line << std::endl;

    auto count = get_line_count(ast.path);

    auto lines_ahead = (size_t)((display_lines_ - .5) / 2);
    auto start = (size_t)std::max((int)ast.line - (int)lines_ahead, 1);
    auto end = std::min(start + display_lines_, count);

    auto needed_digits = std::to_string(count).length();

    for (auto l = start; l < end; l++) {
      auto s = get_line(ast.path, l);
      if (l == ast.line) {
        std::cout << "> ";
      } else {
        std::cout << "  ";
      }
      std::cout << std::setw(needed_digits) << l << " " << s << std::endl;
    }
  }

  std::shared_ptr<peg::Ast> find_function_node(const peg::Ast& ast) {
    using namespace peg::udl;
    auto node = ast.parent.lock();
    while (node->parent.lock() && node->tag != "FUNCTION"_) {
      node = node->parent.lock();
    }
    return node;
  }

  void enum_identifiers(const peg::Ast& ast,
                        std::set<std::string>& references) {
    using namespace peg::udl;

    for (auto node : ast.nodes) {
      switch (node->tag) {
        case "IDENTIFIER"_:
          references.insert(node->token);
          break;
        case "FUNCTION"_:
          break;
        default:
          enum_identifiers(*node, references);
          break;
      }
    }
  }

  void print(const peg::Ast& ast, Environment& env, const std::string& symbol) {
    if (symbol.empty()) {
      print_all(ast, env);
    } else if (env.has(symbol)) {
      std::cout << symbol << ": " << env.get(symbol).str() << std::endl;
    } else {
      std::cout << "'" << symbol << "'"
                << "is not undefined." << std::endl;
    }
  }

  void print_all(const peg::Ast& ast, Environment& env) {
    auto node = find_function_node(ast);
    std::set<std::string> references;
    enum_identifiers(*node, references);
    for (const auto& symbol : references) {
      if (env.has(symbol)) {
        const auto& val = env.get(symbol);
        if (val.type != Value::Function) {
          std::cout << symbol << ": " << val.str() << std::endl;
        }
      }
    }
  }

  size_t get_line_count(const std::string& path) {
    return sources_[path].size();
  }

  std::string get_line(const std::string& path, size_t line) {
    const auto& positions = sources_[path];
    auto idx = line - 1;
    auto first = idx > 0 ? positions[idx - 1] : 0;
    auto last = positions[idx];
    auto size = last - first;

    std::string s(size, 0);
    std::ifstream ifs(path, std::ios::in | std::ios::binary);
    ifs.seekg(first, std::ios::beg)
        .read((char*)s.data(), static_cast<std::streamsize>(s.size()));

    size_t count = 0;
    auto rit = s.rbegin();
    while (rit != s.rend()) {
      if (*rit == '\n') {
        count++;
      }
      ++rit;
    }

    s = s.substr(0, s.size() - count);

    return s;
  }

  void prepare_cache(const std::string& path) {
    auto it = sources_.find(path);
    if (it == sources_.end()) {
      std::vector<char> buff;
      read_file(path.c_str(), buff);

      auto& positions = sources_[path];

      auto i = 0u;
      for (; i < buff.size(); i++) {
        if (buff[i] == '\n') {
          positions.push_back(i + 1);
        }
      }
      positions.push_back(i);
    }
  }

  void usage() {
    std::cout << "Usage: (c)ontinue, (n)ext, (s)tep in, step (o)out, (p)ring, "
                 "(l)ist, (q)uit"
              << std::endl;
  }

  bool read_file(const char* path, std::vector<char>& buff) {
    std::ifstream ifs(path, std::ios::in | std::ios::binary);
    if (ifs.fail()) {
      return false;
    }

    auto size = static_cast<unsigned int>(ifs.seekg(0, std::ios::end).tellg());

    if (size > 0) {
      buff.resize(size);
      ifs.seekg(0, std::ios::beg)
          .read(&buff[0], static_cast<std::streamsize>(buff.size()));
    }

    return true;
  }

  bool quit_ = false;
  std::string command_;
  size_t level_ = 0;
  size_t display_lines_ = 4;
  std::map<std::string, std::vector<size_t>> sources_;
};

}  // namespace culebra
