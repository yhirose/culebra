#!/usr/bin/env bash
# include/interop/search_splitter.h is what a library outside this repo includes
# to implement a splitter for Search. Its whole value is that it needs nothing
# else -- not searchlib, not the runtime, not the value representation -- and
# nothing else would notice the day it stops being true: search.h includes it,
# so every ordinary build has the rest of the world on the include path anyway.
#
# So compile a translation unit that includes it and nothing else, with only
# `-I include`, and run it. The fixture implements the adapter shape measured
# against a real morphological analyzer (UTF-16 offsets, base forms, a
# part-of-speech filter that makes the ranges disjoint), so the gate also fails
# if the contract loses the room that shape needs.
set -euo pipefail
cd "$(dirname "$0")/../.."

CXX="${CXX:-c++}"
SRC=tools/checks/search_splitter_standalone.cc
BIN="$(mktemp -d)/search_splitter_standalone"
trap 'rm -rf "$(dirname "$BIN")"' EXIT

# -I include and nothing else, on purpose: any other -I would let the header
# lean on something and the gate would stop meaning anything.
"$CXX" -std=c++20 -O0 -Wall -Wextra -Werror -I include "$SRC" -o "$BIN"
"$BIN"
