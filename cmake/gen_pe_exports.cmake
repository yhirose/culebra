# Write a PE .def naming the symbols a set of objects defines that
# cmake/exported_symbols.txt says to export — the Windows end of the ELF
# whitelist, read off that same file so the two cannot drift.
#
# Neither PE linker takes a pattern: GNU ld's --export-all-symbols could be
# narrowed with --exclude-libs ALL, but lld has no such option and would export
# LLVM's ~80k static-library symbols past the 65535-entry export table. So the
# names are read off the objects with nm at link time
# (culebra_export_jit_symbols, CMakeLists.txt) and handed to the linker.
#
#   cmake -DNM=<nm> -DLIST=<exported_symbols.txt> -DOUT=<file.def> \
#         "-DOBJECTS=<obj;obj;...>" -P gen_pe_exports.cmake
foreach(v NM LIST OUT OBJECTS)
  if(NOT DEFINED ${v})
    message(FATAL_ERROR "gen_pe_exports.cmake: -D${v}=... is required")
  endif()
endforeach()

# The list is an ld version script; its globs are what to match. `culebra_*;`
# becomes `^culebra_[A-Za-z0-9_]+$` — no other wildcard shape has ever been in
# there, and one that isn't a plain prefix should stop here rather than export
# a silently different set from the ELF build's.
file(STRINGS "${LIST}" list_lines REGEX "^[ \t]*[A-Za-z_].*;")
set(want "")
foreach(line IN LISTS list_lines)
  if(NOT line MATCHES "^[ \t]*([A-Za-z_][A-Za-z0-9_]*)\\*;")
    message(FATAL_ERROR "gen_pe_exports.cmake: ${LIST} entry is not a prefix "
                        "glob: ${line}")
  endif()
  list(APPEND want "^${CMAKE_MATCH_1}[A-Za-z0-9_]+$")
endforeach()
list(JOIN want "|" want_re)

execute_process(COMMAND "${NM}" --defined-only --extern-only ${OBJECTS}
                OUTPUT_VARIABLE syms RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_pe_exports.cmake: ${NM} failed (${rc})")
endif()

# nm prints `<address> <class> <name>` per symbol, under a `<file>:` header per
# object; only the symbol lines match. x86-64 COFF names carry no `_` prefix.
# T is a function; anything else defined (D/B/R/...) is data, and a .def that
# does not say DATA exports the address of a thunk instead of the variable.
set(funcs "")
set(datas "")
string(REGEX MATCHALL "[^\n]+" lines "${syms}")
foreach(line IN LISTS lines)
  if(line MATCHES "^[0-9a-fA-F]+ ([A-Za-z]) ([A-Za-z0-9_]+)[ \r]*$"
     AND CMAKE_MATCH_2 MATCHES "${want_re}")
    if(CMAKE_MATCH_1 STREQUAL "T")
      list(APPEND funcs "${CMAKE_MATCH_2}")
    else()
      list(APPEND datas "${CMAKE_MATCH_2}")
    endif()
  endif()
endforeach()
# An inline `used` helper is defined in every object that includes its header.
list(REMOVE_DUPLICATES funcs)
list(REMOVE_DUPLICATES datas)
list(SORT funcs)
list(SORT datas)
if(NOT funcs)
  message(FATAL_ERROR "gen_pe_exports.cmake: no exported symbol in ${OBJECTS}")
endif()

set(def "EXPORTS\n")
foreach(s IN LISTS funcs)
  string(APPEND def "  ${s}\n")
endforeach()
foreach(s IN LISTS datas)
  string(APPEND def "  ${s} DATA\n")
endforeach()
file(WRITE "${OUT}" "${def}")
list(LENGTH funcs nf)
list(LENGTH datas nd)
message(STATUS "gen_pe_exports: ${nf} functions + ${nd} data symbols -> ${OUT}")
