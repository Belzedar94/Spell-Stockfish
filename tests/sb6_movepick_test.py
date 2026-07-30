#!/usr/bin/env python3
"""SB6 gate: builds and runs tests/cpp/sb6_frozen_threats.cpp against the
engine object files, so the test can talk to Position and MovePicker directly
(move ordering is not observable over UCI).

Requires a prior `make build` in src/ -- the object files it links against are
the ones that build produced. Compile flags are taken from the Makefile itself
(`make -B -n`), so the test always matches the engine's ABI.

Usage: python sb6_movepick_test.py [ARCH=... COMP=...]
"""

import os
import re
import shlex
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "src")

MAKE_ARGS = sys.argv[1:] or ["ARCH=x86-64-bmi2", "COMP=mingw"]


def compile_flags():
    """Pull the real compile command line out of the Makefile."""
    r = subprocess.run(["make", "-B", "-n", "build"] + MAKE_ARGS,
                       cwd=SRC, capture_output=True, text=True)
    for line in (r.stdout + r.stderr).splitlines():
        if " -c -o " in line and line.strip().endswith(".cpp"):
            parts = shlex.split(line, posix=False)
            compiler = parts[0]
            # drop "-c -o foo.o foo.cpp"
            cut = parts.index("-c")
            return compiler, parts[1:cut]
    raise SystemExit("could not recover the compile command from the Makefile")


def main():
    # main.o carries its own main(); the stockfish.ltrans*.o files are -save-temps
    # leftovers of the final link and would duplicate the embedded net.
    objs = [f for f in os.listdir(SRC)
            if f.endswith(".o") and f != "main.o" and not f.startswith("stockfish.")]
    if not objs:
        raise SystemExit("no object files in src/ -- run `make build` first")

    compiler, flags = compile_flags()
    exe = os.path.join(HERE, "cpp", "sb6_frozen_threats.exe")
    test = os.path.join(HERE, "cpp", "sb6_frozen_threats.cpp")

    cmd = ([compiler] + flags + ["-I" + SRC, "-o", exe, test] + sorted(objs)
           + ["-static", "-Wl,--stack,268435456"])
    r = subprocess.run(cmd, cwd=SRC, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-4000:])
        print(r.stderr[-4000:])
        raise SystemExit("SB6 test failed to build")

    r = subprocess.run([exe], capture_output=True, text=True, timeout=300)
    print(r.stdout.strip())
    if r.stderr.strip():
        print(r.stderr.strip())
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
