#include <culebra.h>

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
  vector<string> script_path_list;
};

Options parse_command_line(int argc, const char** argv) {
  Options options;

  int argi = 1;
  while (argi < argc) {
    string arg = argv[argi++];
    if (arg == "--shell") {
      options.shell = true;
    } else if (arg == "--ast") {
      options.print_ast = true;
    } else if (arg == "--debug") {
      options.debug = true;
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
      cerr << "can't open '" << path << "'." << endl;
      return false;
    }

    vector<string> msgs;
    auto ast = culebra::parse(path, buff.data(), buff.size(), msgs);

    if (ast) {
      if (options.print_ast) {
        cout << peg::ast_to_s(ast);
      }

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

int main(int argc, const char** argv) {
  auto options = parse_command_line(argc, argv);

  try {
    auto env = culebra::environment();

    if (!run_scripts(env, options)) {
      return -1;
    }

    if (options.shell) {
      culebra::repl(env, options.print_ast);
    }
  } catch (exception& e) {
    cerr << e.what() << endl;
    return -1;
  }

  return 0;
}
