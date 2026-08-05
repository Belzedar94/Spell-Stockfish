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


def measure(eng, fens, d3_budget, tag):
    """Record perft counts for every FEN; depth 3 while the budget lasts."""
    rows = []
    for i, fen in enumerate(fens):
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
        print(f"[{tag} {i+1}/{len(fens)}] d1={d1} d2={d2} {fen}", flush=True)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("engine")
    ap.add_argument("--out", default="perft_spell.csv")
    ap.add_argument("--note", default=None,
                    help="provenance line recorded in the CSV header "
                         "(which engine was the oracle, and why)")
    ap.add_argument("--fixed", default=None,
                    help="file with FENs (one per line) appended verbatim as a "
                         "fixed section after the sampled ones; the random walk "
                         "cannot be trusted to reach them")
    ap.add_argument("--fixed-engine", default=None,
                    help="engine that measures the fixed section (defaults to "
                         "the main oracle); pass a different one to have the "
                         "fixed rows come from an independent arbiter")
    ap.add_argument("--fixed-note", default=None,
                    help="provenance line recorded above the fixed section")
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
    # startpos always first, asked to the engine instead of hardcoded: the
    # suite then records the exact FEN the engine emits (spell block included)
    # and compare_perft.py --fen-check stays clean.
    eng.set_position([])
    selected.insert(0, engine_fen(eng))

    rows = measure(eng, selected, D3_MAX_POSITIONS, "sampled")

    fixed_rows = []
    if args.fixed:
        with open(args.fixed, encoding="utf-8") as f:
            fixed_fens = [ln.strip() for ln in f
                          if ln.strip() and not ln.startswith("#")]
        fixed_eng = Engine(args.fixed_engine) if args.fixed_engine else eng
        # No depth 3 here on purpose: the fixed section exists to watch a
        # movegen rule that already fires at depth 1, and the deeper the run
        # the more of the arbiter's own limits it drags into the reference.
        fixed_rows = measure(fixed_eng, fixed_fens, 0, "fixed")
        if fixed_eng is not eng:
            fixed_eng.quit()

    with open(args.out, "w", encoding="utf-8") as f:
        f.write("# Spell Chess perft parity suite — generated by gen_perft_suite.py "
                f"(seed {SEED}) against the oracle engine given on the command line\n")
        if args.note:
            f.write(f"# {args.note}\n")
        f.write("# fen;depth1;depth2[;depth3]\n")
        for row in rows:
            f.write(";".join(row) + "\n")
        if fixed_rows:
            f.write("#\n# fixed section (not sampled)\n")
            if args.fixed_note:
                f.write(f"# {args.fixed_note}\n")
            for row in fixed_rows:
                f.write(";".join(row) + "\n")

    eng.quit()
    print(f"Wrote {len(rows)} sampled + {len(fixed_rows)} fixed positions "
          f"to {args.out}")


if __name__ == "__main__":
    main()
