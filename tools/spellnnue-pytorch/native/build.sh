#!/usr/bin/env bash
# Builds the native run7 loader.
#
# On Windows run this from an MSYS2 shell, or from any shell with
# C:/msys64/mingw64/bin on PATH.  The result is a plain C ABI shared library
# with no dependency on the Python C API, so the interpreter that loads it does
# not have to match the compiler that built it.
#
#   bash build.sh                 # release build next to this script
#   SPELL_LOADER_ARCH=x86-64-v3 bash build.sh   # portable across the fleet
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ARCH="${SPELL_LOADER_ARCH:-native}"
CXX="${CXX:-g++}"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        OUTPUT="$HERE/spell_data_loader.dll"
        # Static runtimes keep the DLL loadable without libstdc++-6.dll and
        # friends on PATH, which matters when ctypes loads it by absolute path.
        PLATFORM_FLAGS=(-shared -static -static-libgcc -static-libstdc++)
        ;;
    Darwin)
        OUTPUT="$HERE/libspell_data_loader.dylib"
        PLATFORM_FLAGS=(-shared -fPIC -pthread)
        ;;
    *)
        OUTPUT="$HERE/libspell_data_loader.so"
        PLATFORM_FLAGS=(-shared -fPIC -pthread)
        ;;
esac

echo "building $OUTPUT (arch=$ARCH)"
"$CXX" -O3 -march="$ARCH" -std=c++17 -fno-math-errno -funroll-loops \
    -Wall -Wextra -Wno-unused-parameter \
    "${PLATFORM_FLAGS[@]}" \
    -o "$OUTPUT" "$HERE/spell_data_loader.cpp"

echo "built $OUTPUT"
