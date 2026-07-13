#!/usr/bin/env python3
"""CECP/xboard protocol conformance test for Spell-Stockfish (S5 suite).

Covers: xboard/protover 2 handshake and feature set, variant setup line
and piece definitions, ping/pong (idle and during search), new/sd/go,
force + gated usermoves, illegal-move rejection, spell setboard, undo and
remove, analyze/exit thinking output, level/time/otim timed go, result
and graceful quit. Exit code 0 = pass.

Usage: python xboard_test.py [path-to-engine] [path-to-net]
(CECP has no setoption: the net, if given, is set via 'option EvalFile=<path>'.)
"""

import re
import subprocess
import sys
import time

ENGINE = sys.argv[1] if len(sys.argv) > 1 else "../src/stockfish.exe"
NET = sys.argv[2] if len(sys.argv) > 2 else None

SETUP_LINE = ("setup (PNBRQ................Kpnbrq................k) "
              "8x8+0_spell-chess "
              "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR[JJFFFFFjjfffff] "
              "w KQkq - 0 1")
PIECE_LETTERS = "PNBRQKFJ"

# Spell FEN with active zones (f@e2 with 2 plies left), black to move
SETBOARD_FEN = ("r1bqkbnr/pppp1ppp/2n1p3/1N6/8/4P3/PPPP1PPP/R1BQKBNR"
                "[JJFFFFjjffff] {F@-:0,J@-:0,f@e2:2,j@-:0} b KQkq - 1 3")

# Coordinate move, optionally gated with a spell prefix (f@sq, / j@sq,)
MOVE_RE = re.compile(
    r"^move\s+(?:[FJfj]@[a-h][1-8],)?[a-h][1-8][a-h][1-8][a-z]?\s*$")
THINK_RE = re.compile(r"^\s*\d+\s")  # post format: depth first

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

    def until(self, pred, tmo=60):
        """pred: str (line.startswith) or callable(line)->bool."""
        if isinstance(pred, str):
            tok = pred
            pred = lambda l: l.startswith(tok)
        out, dl = [], time.time() + tmo
        while time.time() < dl:
            line = self.p.stdout.readline()
            if not line:
                return out, False
            l = line.rstrip("\r\n")
            out.append(l)
            if pred(l):
                return out, True
        return out, False

    def until_all(self, preds, tmo=60):
        """Collect lines until every predicate has matched (any order)."""
        remaining = list(preds)
        out, dl = [], time.time() + tmo
        while remaining and time.time() < dl:
            line = self.p.stdout.readline()
            if not line:
                break
            l = line.rstrip("\r\n")
            out.append(l)
            remaining = [p for p in remaining if not p(l)]
        return out, not remaining


def main():
    e = E(ENGINE)

    # 1. Handshake: xboard + protover 2 -> features until done=1
    e.send("xboard")
    e.send("protover 2")
    lines, ok = e.until(lambda l: l.startswith("feature") and "done=1" in l, 60)
    check(ok, "feature done=1 tras protover 2")
    blob = "\n".join(l for l in lines if l.startswith("feature"))
    for tok in ("setboard=1", "usermove=1", "time=1", "memory=1", "smp=1",
                "colors=0", "draw=0", "name=0", "sigint=0", "ping=1"):
        check(tok in blob, f"feature {tok}")
    check("Spell-Stockfish" in blob, 'myname contiene "Spell-Stockfish"')
    check('variants="spell-chess"' in blob, 'variants="spell-chess"')

    if NET:
        e.send(f"option EvalFile={NET}")

    # 1b. variant spell-chess -> setup + piece lines (may already have come
    # with the protover response; accept either). Sync with ping.
    e.send("variant spell-chess")
    e.send("ping 1")
    lines2, ok = e.until("pong 1", 120)
    check(ok, "pong 1 tras ping 1")  # 2. ping/pong en reposo
    everything = lines + lines2
    check(any(l.startswith(SETUP_LINE) for l in everything),
          "linea setup spell-chess correcta")
    for c in PIECE_LETTERS:
        check(any(l.startswith(f"piece {c}&") for l in everything),
              f"linea piece {c}&")

    # 3. new + sd 4 + go -> move; ping durante busqueda debe llegar
    e.send("new")
    e.send("sd 4")
    e.send("go")
    e.send("ping 2")
    lines, ok = e.until_all(
        [lambda l: l.startswith("move "), lambda l: l.startswith("pong 2")], 90)
    check(ok, "move + pong 2 tras go (ping durante busqueda llega)")
    mv = [l for l in lines if l.startswith("move ")]
    check(bool(mv) and MOVE_RE.match(mv[0]), f"formato de move: {mv[0] if mv else '(ninguno)'}")

    # 4. force + usermoves gated + go -> move
    e.send("new")
    e.send("sd 4")
    e.send("force")
    e.send("usermove f@e7,e2e4")
    e.send("usermove b8c6")
    e.send("go")
    lines, ok = e.until("move ", 90)
    check(ok, "move tras force + usermoves gated")
    mv = [l for l in lines if l.startswith("move ")]
    check(bool(mv) and MOVE_RE.match(mv[0]), "formato de move tras usermoves")

    # 5. usermove ilegal (e7 congelado por f@e7) -> Illegal move:
    e.send("new")
    e.send("sd 4")
    e.send("force")
    e.send("usermove f@e7,e2e4")
    e.send("usermove e7e5")
    lines, ok = e.until("Illegal move", 30)
    check(ok, "Illegal move: con e7 congelado")

    # 6. setboard con zonas activas + go -> move
    e.send(f"setboard {SETBOARD_FEN}")
    e.send("go")
    lines, ok = e.until("move ", 90)
    check(ok, "move tras setboard con zonas activas")
    mv = [l for l in lines if l.startswith("move ")]
    check(bool(mv) and MOVE_RE.match(mv[0]), "formato de move tras setboard")

    # 7. undo / remove en force: sin crash, ping responde
    e.send("new")
    e.send("force")
    e.send("usermove f@e7,e2e4")
    e.send("usermove b8c6")
    e.send("undo")
    e.send("ping 3")
    _, ok = e.until("pong 3", 30)
    check(ok, "pong 3 tras undo")
    e.send("usermove b8c6")
    e.send("remove")
    e.send("ping 4")
    _, ok = e.until("pong 4", 30)
    check(ok, "pong 4 tras remove")

    # 8. analyze -> thinking lines (post format, empieza por depth); exit
    e.send("new")
    e.send("post")
    e.send("analyze")
    lines, ok = e.until(lambda l: bool(THINK_RE.match(l)), 60)
    check(ok, "linea de thinking en analyze")
    e.send("exit")
    e.send("ping 5")
    _, ok = e.until("pong 5", 30)
    check(ok, "pong 5 tras exit de analyze")

    # 9. level/time/otim + go -> move dentro de ~10s
    e.send("new")
    e.send("level 40 5 0")
    e.send("time 6000")
    e.send("otim 6000")
    t0 = time.time()
    e.send("go")
    _, ok = e.until("move ", 12)
    dt = time.time() - t0
    check(ok, f"move con level/time/otim en {dt:.1f}s (<12s)")

    # 10. result + quit limpio
    e.send("result 1-0 {X}")
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
