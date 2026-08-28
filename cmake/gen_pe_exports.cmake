# Write a PE .def file naming the culebra_* symbols a set of objects defines —
# the Windows spelling of exported_symbols.txt, whose `culebra_*` is a glob the
# ELF linker matches itself. Neither PE linker takes a pattern: GNU ld's
# --export-all-symbols could be narrowed with --exclude-libs ALL, but lld has
# no such option and would export LLVM's ~80k static-library symbols past the
# 65535-entry export table. So the list is read off the objects with nm at
# link time (culebra_export_jit_symbols, CMakeLists.txt) and handed to both
# linkers the same way.
#
#   cmake -DNM=<nm> -DOUT=<file.def> "-DOBJECTS=<obj;obj;...>" -P gen_pe_exports.cmake
foreach(v NM OUT OBJECTS)
  if(NOT DEFINED ${v})
    message(FATAL_ERROR "gen_pe_exports.cmake: -D${v}=... is required")
  endif()
endforeach()

execute_process(COMMAND "${NM}" --defined-only --extern-only ${OBJECTS}
                OUTPUT_VARIABLE syms RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_pe_exports.cmake: ${NM} failed (${rc})")
endif()

# nm prints `<address> <class> <name>` per symbol, under a `<file>:` header per
# object; only the symbol lines match. x86-64 COFF names carry no `_` prefix.
# T is a function; anything else defined (D/B/R/...) is data, which a .def
# marks so the linker does not export it as code.
set(funcs "")
set(datas "")
string(REGEX MATCHALL "[^\n]+" lines "${syms}")
foreach(line IN LISTS lines)
  if(line MATCHES "^[0-9a-fA-F]+ ([A-Za-z]) (culebra_[A-Za-z0-9_]+)[ \r]*$")
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
  message(FATAL_ERROR "gen_pe_exports.cmake: no culebra_* symbol in ${OBJECTS}")
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
