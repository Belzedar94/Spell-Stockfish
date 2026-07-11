#!/usr/bin/env python3
"""Generate the Spell Chess perft parity suite from the frozen reference binary.

Plays seeded random walks with the reference engine, samples positions (with a
bias towards active spell zones / cooldowns), and records perft node counts.

Usage:
    python gen_perft_suite.py <path-to-reference-engine> [--out perft_spell.csv]

Output CSV columns: fen;depth1;depth2[;depth3]
"""

import argparse
import random
import subprocess
import sys
import time

VARIANT = "spell-chess"
SEED = 20260711
NUM_GAMES = 30
MAX_PLIES = 60
GATED_MOVE_BIAS = 0.35      # probability of preferring a gated move when available
QUIET_SAMPLE_EVERY = 7      # sample quiet positions every N plies
MAX_ZONE_POSITIONS = 60
MAX_QUIET_POSITIONS = 25
D3_MAX_D2 = 60000           # only compute depth 3 when depth 2 is below this
D3_MAX_POSITIONS = 4


class Engine:
    def __init__(self, path):
        self.proc = subprocess.Popen(
            [path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.send("uci")
        self.read_until("uciok")
        self.send(f"setoption name UCI_Variant value {VARIANT}")
        self.sync()

    def send(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def read_until(self, token, timeout=300):
        lines = []
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("engine terminated unexpectedly")
            line = line.rstrip("\n")
            lines.append(line)
            if line.startswith(token):
                return lines
        raise RuntimeError(f"timeout waiting for '{token}'")

    def sync(self):
        self.send("isready")
        self.read_until("readyok")

    def set_position(self, moves):
        pos = "position startpos"
        if moves:
            pos += " moves " + " ".join(moves)
        self.send(pos)
        self.sync()

    def quit(self):
        try:
            self.send("quit")
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()


def engine_fen(eng):
    """Fetch current FEN via the 'd' command (line starts with 'Fen:')."""
    eng.send("d")
    eng.send("isready")
    fen = None
    deadline = time.time() + 30
    while time.time() < deadline:
        line = eng.proc.stdout.readline()
        if not line:
            raise RuntimeError("engine terminated unexpectedly")
        line = line.rstrip("\n")
        if line.startswith("Fen: "):
            fen = line[5:].strip()
        if line.startswith("readyok"):
            return fen
    raise RuntimeError("timeout in engine_fen")


def perft_divide(eng, depth, timeout=600):
    """Return (root_moves dict, total nodes) for 'go perft depth'."""
    eng.send(f"go perft {depth}")
    moves = {}
    total = None
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = eng.proc.stdout.readline()
        if not line:
            raise RuntimeError("engine terminated unexpectedly")
        line = line.rstrip("\n")
        if line.startswith("Nodes searched: "):
            total = int(line.split(": ")[1])
            return moves, total
        if ": " in line and not line.startswith("info"):
            mv, _, cnt = line.rpartition(": ")
            if mv and cnt.isdigit():
                moves[mv] = int(cnt)
    raise RuntimeError("timeout in perft_divide")


def has_royals(fen):
    board = fen.split()[0].split("[")[0]
    return "k" in board and "K" in board


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("engine")
    ap.add_argument("--out", default="perft_spell.csv")
    args = ap.parse_args()

    rng = random.Random(SEED)
    eng = Engine(args.engine)

    zone_positions = []   # FENs with active spell state ({...} present)
    quiet_positions = []
    seen = set()

    for game in range(NUM_GAMES):
        moves = []
        eng.set_position(moves)
        for ply in range(MAX_PLIES):
            root, total = perft_divide(eng, 1)
            if total == 0 or not root:
                break
            gated = [m for m in root if m.startswith(("f@", "j@"))]
            normal = [m for m in root if not m.startswith(("f@", "j@"))]
            if gated and (not normal or rng.random() < GATED_MOVE_BIAS):
                mv = rng.choice(gated)
            else:
                mv = rng.choice(normal or gated)
            moves.append(mv)
            eng.set_position(moves)
            fen = engine_fen(eng)
            if fen is None or not has_royals(fen):
                break
            key = fen
            if key in seen:
                continue
            if "{" in fen:
                seen.add(key)
                zone_positions.append(fen)
            elif ply % QUIET_SAMPLE_EVERY == 0:
                seen.add(key)
                quiet_positions.append(fen)
        print(f"[game {game+1}/{NUM_GAMES}] plies={len(moves)} "
              f"zone={len(zone_positions)} quiet={len(quiet_positions)}", flush=True)

    rng.shuffle(zone_positions)
    rng.shuffle(quiet_positions)
    selected = zone_positions[:MAX_ZONE_POSITIONS] + quiet_positions[:MAX_QUIET_POSITIONS]
    # startpos always first
    startpos_fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR[JJFFFFFjjfffff] w KQkq - 0 1"
    selected.insert(0, startpos_fen)

    rows = []
    d3_budget = D3_MAX_POSITIONS
    for i, fen in enumerate(selected):
        eng.send(f"position fen {fen}")
        eng.sync()
        _, d1 = perft_divide(eng, 1)
        _, d2 = perft_divide(eng, 2)
        row = [fen, str(d1), str(d2)]
        if d2 and d2 < D3_MAX_D2 and d3_budget > 0:
            _, d3 = perft_divide(eng, 3, timeout=1800)
            row.append(str(d3))
            d3_budget -= 1
        rows.append(row)
        print(f"[{i+1}/{len(selected)}] d1={d1} d2={d2} {fen}", flush=True)

    with open(args.out, "w", encoding="utf-8") as f:
        f.write("# Spell Chess perft parity suite — generated by gen_perft_suite.py "
                f"(seed {SEED}) against the frozen reference baseline\n")
        f.write("# fen;depth1;depth2[;depth3]\n")
        for row in rows:
            f.write(";".join(row) + "\n")

    eng.quit()
    print(f"Wrote {len(rows)} positions to {args.out}")


if __name__ == "__main__":
    main()
