#include <culebra.h>
#include <stdlib_interp.h>
#ifdef CULEBRA_JIT_ENABLED
#include <stdlib_jit.h>
#endif
#include <print>

using namespace std;

bool read_file(const char* path, vector<char>& buff) {
  ifstream ifs(path, ios::in | ios::binary);
  if (ifs.fail()) {
    return false;
  }

  auto size = static_cast<unsigned int>(ifs.seekg(0, ios::end).tellg());

  if (size > 0) {
    buff.resize(size);
    ifs.seekg(0, ios::beg).read(&buff[0], static_cast<streamsize>(buff.size()));
  }

  return true;
}

struct Options {
  bool print_ast = false;
  bool shell = false;
  bool debug = false;
#ifdef CULEBRA_JIT_ENABLED
  bool jit = false;
  bool emit_llvm = false;
  int opt_level = 2;
#endif
  vector<string> script_path_list;
  vector<string> script_argv;
};

Options parse_command_line(int argc, const char** argv) {
  Options options;

  int argi = 1;
  bool past_separator = false;
  while (argi < argc) {
    string arg = argv[argi++];
    if (past_separator) {
      options.script_argv.push_back(std::move(arg));
      continue;
    }
    if (arg == "--") {
      past_separator = true;
    } else if (arg == "--shell") {
      options.shell = true;
    } else if (arg == "--ast") {
      options.print_ast = true;
    } else if (arg == "--debug") {
      options.debug = true;
#ifdef CULEBRA_JIT_ENABLED
    } else if (arg == "--jit") {
      options.jit = true;
    } else if (arg == "--emit-llvm") {
      options.emit_llvm = true;
    } else if (arg.starts_with("-O")) {
      options.opt_level = std::stoi(arg.substr(2));
#endif
    } else {
      options.script_path_list.push_back(arg);
    }
  }

  if (!options.shell) {
    options.shell = options.script_path_list.empty();
  }

  return options;
}

bool run_scripts(shared_ptr<culebra::Environment> env, const Options& options) {
  for (auto path : options.script_path_list) {
    vector<char> buff;
    if (!read_file(path.c_str(), buff)) {
      std::println(stderr, "can't open '{}'.", path);
      return false;
    }

    vector<string> msgs;
    auto ast = culebra::parse(path, buff.data(), buff.size(), msgs);

    if (ast) {
      if (options.print_ast) {
        cout << peg::ast_to_s(ast);
      }

#ifdef CULEBRA_JIT_ENABLED
      if (options.jit) {
        culebra::_culebra_sys_argv_holder() = options.script_argv;
        culebra::JIT::run(ast, options.emit_llvm, options.debug,
                          options.opt_level);
        continue;
      }
#endif

      culebra::Value val;
      auto dbg =
          options.debug ? culebra::CommandLineDebugger() : culebra::Debugger();

      if (culebra::interpret(ast, env, val, msgs, dbg)) {
        continue;
      }
    }

    for (const auto& msg : msgs) {
      cerr << msg << endl;
    }
    return false;
  }

  return true;
}

// The CLI aliases IO.puts and IO.print as globals. Embedders that use
// culebra::environment() directly get a clean environment without
// these global names.
void install_cli_aliases(culebra::Environment& env) {
  const auto& io = env.get("IO").to_object();
  env.initialize("puts", io.get("puts"), false);
  env.initialize("print", io.get("print"), false);
}

int main(int argc, const char** argv) {
  auto options = parse_command_line(argc, argv);

#ifdef CULEBRA_JIT_ENABLED
  culebra::install_jit_stdlib();
#endif

  try {
    auto env = culebra::environment(options.script_argv);
    install_cli_aliases(*env);

    if (!run_scripts(env, options)) {
      return -1;
    }

    if (options.shell) {
#ifdef CULEBRA_JIT_ENABLED
      culebra::repl(env, options.print_ast, options.jit);
#else
      culebra::repl(env, options.print_ast);
#endif
    }
  } catch (exception& e) {
    cerr << e.what() << endl;
    return -1;
  }

  return 0;
}
