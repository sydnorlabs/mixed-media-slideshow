#!/usr/bin/env sh
set -eu
cxx="${CXX:-g++}"
out="${TMPDIR:-/tmp}/mixed-media-slideshow-tests"
"$cxx" -std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc \
  tests/media-library-tests.cpp src/media-library.cpp -o "$out"
"$out"
rm -f "$out"
