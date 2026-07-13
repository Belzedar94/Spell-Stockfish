#!/usr/bin/env python3
"""Search reproducibility test (S5 suite, reprosearch.sh equivalent).

The same position searched twice at the same fixed depth in a fresh-TT
single-thread engine must produce identical node counts and PVs; a set of
spell-heavy suite positions is used. Exit 0 = deterministic.

Usage: python repro_test.py [path-to-engine] [path-to-net]
"""

import subprocess
import sys
import time

ENGINE = sys.argv[1] if len(sys.argv) > 1 else "../src/stockfish.exe"
NET = sys.argv[2] if len(sys.argv) > 2 else None

POSITIONS = [
    "startpos",
    ("fen rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR[JJFFFFFjjfffff] "
     "{F@-:0,J@-:0,f@-:0,j@-:0} w KQkq - 0 1 moves f@e7,e2e4 b8c6 j@c6,d2d4"),
    ("fen r1bqkbnr/pppp1ppp/2n1p3/1N6/8/4P3/PPPP1PPP/R1BQKBNR[JJFFFFjjffff] "
     "{F@-:0,J@-:0,f@e2:2,j@-:0} b KQkq - 1 3"),
]
DEPTH = 10


def run_engine(cmds, wait_tokens):
    p = subprocess.Popen([ENGINE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, bufsize=1)
    out = []
    for cmd, tok in zip(cmds, wait_tokens):
        p.stdin.write(cmd + "\n")
        p.stdin.flush()
        if tok:
            dl = time.time() + 180
            while time.time() < dl:
                line = p.stdout.readline()
                if not line:
                    break
                out.append(line.rstrip("\n"))
                if line.startswith(tok):
                    break
    p.stdin.write("quit\n")
    p.stdin.flush()
    p.wait(timeout=10)
    return out


def search_signature(pos):
    cmds = ["uci", "setoption name Threads value 1", "setoption name Hash value 64"]
    toks = ["uciok", None, None]
    if NET:
        cmds.append(f"setoption name EvalFile value {NET}")
        toks.append(None)
    cmds += ["isready", f"position {pos}", f"go depth {DEPTH}"]
    toks += ["readyok", None, "bestmove"]
    out = run_engine(cmds, toks)
    last_info = next((l for l in reversed(out) if l.startswith("info depth")), "")
    best = next((l for l in reversed(out) if l.startswith("bestmove")), "")
    nodes = ""
    parts = last_info.split()
    if "nodes" in parts:
        nodes = parts[parts.index("nodes") + 1]
    return nodes, best, last_info


def main():
    fails = 0
    for pos in POSITIONS:
        a = search_signature(pos)
        b = search_signature(pos)
        same = a[0] == b[0] and a[1] == b[1] and a[0] != ""
        print(("ok  " if same else "FAIL") +
              f" nodes={a[0]} vs {b[0]}  {a[1]}  [{pos[:50]}...]")
        fails += not same
    print(f"\n{'PASS' if not fails else 'FAIL'}: {fails} posiciones no deterministas")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
