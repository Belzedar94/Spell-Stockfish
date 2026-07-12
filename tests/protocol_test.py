#!/usr/bin/env python3
"""UCI protocol conformance test for Spell-Stockfish (S5 suite).

Covers: handshake, option enumeration and setting, position/go/stop
lifecycles, ucinewgame, isready storms, spell-FEN and spell-move round
trips through `position ... moves`, bestmove under node/depth/movetime
limits, graceful quit. Exit code 0 = pass.

Usage: python protocol_test.py [path-to-engine] [path-to-net]
"""

import subprocess
import sys
import time

ENGINE = sys.argv[1] if len(sys.argv) > 1 else "../src/stockfish.exe"
NET = sys.argv[2] if len(sys.argv) > 2 else None

STARTPOS = ("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR[JJFFFFFjjfffff] "
            "{F@-:0,J@-:0,f@-:0,j@-:0} w KQkq - 0 1")
# f@e7 freezes e7: the reply must come from outside the zone
SPELL_LINE = "f@e7,e2e4 b8c6 j@c6,d2d4 g8f6"

FAILS = []


def check(cond, label):
    print(("ok  " if cond else "FAIL") + " " + label)
    if not cond:
        FAILS.append(label)


class E:
    def __init__(self, path):
        self.p = subprocess.Popen([path], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, bufsize=1)

    def send(self, s):
        self.p.stdin.write(s + "\n")
        self.p.stdin.flush()

    def until(self, tok, tmo=60):
        out, dl = [], time.time() + tmo
        while time.time() < dl:
            line = self.p.stdout.readline()
            if not line:
                return out, False
            out.append(line.rstrip("\n"))
            if line.startswith(tok):
                return out, True
        return out, False


def main():
    e = E(ENGINE)

    # 1. Handshake
    e.send("uci")
    lines, ok = e.until("uciok")
    check(ok, "uciok tras uci")
    check(any(l.startswith("id name") for l in lines), "id name presente")
    opts = [l for l in lines if l.startswith("option name")]
    names = {l.split()[2] for l in opts}
    for req in ("Threads", "Hash", "MultiPV", "Move", "EvalFile", "UCI_Variant"):
        check(any(n.startswith(req) for n in names), f"opcion {req}")
    check(any("spell-chess" in l for l in opts), "UCI_Variant anuncia spell-chess")

    # 2. isready storm
    for _ in range(5):
        e.send("isready")
        _, ok = e.until("readyok", 10)
        check(ok, "readyok")

    # 3. Options set (incl. compat options the harness uses)
    for cmd in ("setoption name Threads value 1",
                "setoption name Hash value 64",
                "setoption name UCI_Variant value spell-chess",
                "setoption name MultiPV value 2",
                "setoption name Move Overhead value 100"):
        e.send(cmd)
    if NET:
        e.send(f"setoption name EvalFile value {NET}")
    e.send("isready")
    _, ok = e.until("readyok", 120)
    check(ok, "readyok tras setoptions")

    # 4. Spell FEN + spell moves round trip
    e.send(f"position fen {STARTPOS} moves {SPELL_LINE}")
    e.send("go depth 4")
    lines, ok = e.until("bestmove", 60)
    check(ok, "bestmove con moves gated aplicados")
    check(any("multipv 2" in l for l in lines), "MultiPV 2 emite segunda linea")

    # 5. go nodes / movetime / stop
    e.send("position startpos")
    e.send("go nodes 5000")
    _, ok = e.until("bestmove", 30)
    check(ok, "bestmove con go nodes")

    e.send("go movetime 300")
    _, ok = e.until("bestmove", 10)
    check(ok, "bestmove con movetime")

    e.send("go infinite")
    time.sleep(0.5)
    e.send("stop")
    _, ok = e.until("bestmove", 10)
    check(ok, "stop interrumpe go infinite")

    # 6. ucinewgame + reuse
    e.send("ucinewgame")
    e.send("isready")
    _, ok = e.until("readyok", 60)
    check(ok, "readyok tras ucinewgame")
    e.send("position startpos moves f@e7,e2e4 b8c6")
    e.send("go depth 3")
    _, ok = e.until("bestmove", 30)
    check(ok, "partida nueva tras ucinewgame")

    # 7. Terminal position: side with king captured must answer, not hang
    e.send("position fen 8/8/8/8/1k6/8/8/1N6[] {F@-:0,J@-:0,f@-:0,j@-:0} w - - 0 1")
    e.send("go depth 2")
    lines, ok = e.until("bestmove", 30)
    check(ok, "bestmove en posicion terminal (sin rey propio)")

    # 8. Quit
    e.send("quit")
    try:
        e.p.wait(timeout=10)
        check(True, "quit sale limpio")
    except subprocess.TimeoutExpired:
        e.p.kill()
        check(False, "quit sale limpio")

    print(f"\n{'PASS' if not FAILS else 'FAIL'}: {len(FAILS)} fallos")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
