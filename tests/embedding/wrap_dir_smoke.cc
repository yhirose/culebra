// A host class wrapped with the five methods the built-in `Dir` trait
// requires gets the trait's defaults on its handles, in a program that names
// neither the trait nor any of the five: a wrapped method can return a handle
// of a class whose namespace the program never names, so the stdlib splice
// reads the wrap registry rather than the program's text
// (stdlib/preamble.h, builtin_trait_unit_selected).

#include <iostream>
#include <string>

#include <culebra.h>
#include <interop/wrap.h>
#include <vm/embed.h>

// Unity-TU entry (smoke_suite.cc): the named namespace keeps
// this file's internals from colliding with the other smokes.
namespace wrap_dir_smoke_ns {

// One file, `a.txt`, at the root. `size` is the default exercised because it
// reads no listing (a wrapped method cannot return an Array), so list_dir is
// here for the trait's contract alone.
class Shelf {
 public:
  std::string read(std::string path) const {
    return path == "a.txt" ? "alpha" : "";
  }
  std::string list_dir(std::string path) const { return path; }
  bool exists(std::string path) const { return is_file(path) || is_dir(path); }
  bool is_file(std::string path) const { return path == "a.txt"; }
  bool is_dir(std::string path) const { return path.empty(); }
};

int run() {
  culebra::wrap<Shelf>("Host", "Shelf")
      .ctor<>()
      .method<&Shelf::read>("read", {"path"})
      .method<&Shelf::list_dir>("list_dir", {"path"})
      .method<&Shelf::exists>("exists", {"path"})
      .method<&Shelf::is_file>("is_file", {"path"})
      .method<&Shelf::is_dir>("is_dir", {"path"});

  culebra::Runtime rt;
  culebra::RuntimeScope scope(rt);
  culebra::vm::Embed embed;
  culebra::vm::Value v;
  std::vector<std::string> msgs;
  if (!embed.run_source("<shelf>", "Host.Shelf.new().size('a.txt')", v,
                        msgs)) {
    for (auto& m : msgs) std::cerr << "FAIL: " << m << "\n";
    return 1;
  }
  if (v.to_long() != 5) {
    std::cerr << "FAIL: size('a.txt') was " << v.to_long() << ", not 5\n";
    return 1;
  }
  std::cout << "wrap_dir smoke OK\n";
  return 0;
}

}  // namespace wrap_dir_smoke_ns
