#!/bin/sh
# Builds ffish-spell.js (CommonJS) and ffish-spell-es6 (ES module) from the
# rules-only closure. Requires an activated emsdk (emcc in PATH), e.g.:
#   source /c/emsdk/emsdk/emsdk_env.sh
# Run from the repository root:
#   sh scripts/build_ffishjs.sh
set -e

SRCS="src/ffishjs.cpp src/position.cpp src/movegen.cpp src/bitboard.cpp \
      src/attacks.cpp src/notation.cpp src/misc.cpp src/memory.cpp \
      src/spell_params.cpp src/tune.cpp src/ucioption.cpp"

FLAGS="-O2 --bind -std=c++17 -DNDEBUG -DSPELL_RULES_ONLY -Isrc
       -s ALLOW_MEMORY_GROWTH=1 -s INITIAL_MEMORY=67108864
       -s MODULARIZE=1 -s EXPORT_NAME=ffishSpell"

mkdir -p tests/js/cjs tests/js/esm

# CommonJS (require)
emcc $SRCS $FLAGS -o tests/js/cjs/ffish_spell.js

# ES module (import)
emcc $SRCS $FLAGS -s EXPORT_ES6=1 -o tests/js/esm/ffish_spell.js

echo "built: tests/js/cjs/ffish_spell.{js,wasm} + tests/js/esm/ffish_spell.{js,wasm}"
