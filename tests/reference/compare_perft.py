#!/usr/bin/env python3
"""Compare Spell-Stockfish against the frozen reference over the perft suite.

For every suite position the node counts are compared at each recorded depth.
On a mismatch, the script descends the divide tree (position fen X moves ...)
until it isolates the exact move whose subtree differs, printing a minimal
reproduction. Optionally also checks FEN emission parity.

Usage:
    python compare_perft.py <ours> <reference> [--suite perft_spell.csv]
                            [--max-depth 2] [--fen-check] [--fail-fast]
"""

import argparse
import subprocess
import sys
import time

VARIANT = "spell-chess"


class Engine:
    def __init__(self, path, needs_variant):
        self.proc = subprocess.Popen(
            [path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.send("uci")
        self.read_until("uciok")
        if needs_variant:
            self.send(f"setoption name UCI_Variant value {VARIANT}")
        self.sync()

    def send(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def read_until(self, token, timeout=600):
        deadline = time.time() + timeout
        lines = []
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

    def position(self, fen, moves):
        cmd = f"position fen {fen}"
        if moves:
            cmd += " moves " + " ".join(moves)
        self.send(cmd)
        self.sync()

    def perft_divide(self, depth):
        self.send(f"go perft {depth}")
        moves, total = {}, None
        deadline = time.time() + 1800
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("engine terminated unexpectedly")
            line = line.rstrip("\n")
            if line.startswith("Nodes searched: "):
                return moves, int(line.split(": ")[1])
            if ": " in line and not line.startswith("info"):
                mv, _, cnt = line.rpartition(": ")
                if mv and cnt.isdigit():
                    moves[mv.strip()] = int(cnt)
        raise RuntimeError("timeout in perft_divide")

    def fen(self):
        self.send("d")
        self.send("isready")
        fen = None
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("engine terminated unexpectedly")
            line = line.rstrip("\n")
            if line.startswith("Fen: "):
                fen = line[5:].strip()
            if line.startswith("readyok"):
                return fen

    def quit(self):
        try:
            self.send("quit")
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()


def isolate(ours, ref, fen, moves, depth):
    """Descend the divide tree to the smallest differing subtree."""
    ours.position(fen, moves)
    ref.position(fen, moves)
    dA, _ = ours.perft_divide(depth)
    dB, _ = ref.perft_divide(depth)

    only_a = sorted(set(dA) - set(dB))
    only_b = sorted(set(dB) - set(dA))
    if only_a or only_b:
        print(f"    MOVE-SET MISMATCH at: fen '{fen}' moves {' '.join(moves) or '(none)'}")
        if only_a:
            print(f"      only OURS ({len(only_a)}): {only_a[:12]}")
        if only_b:
            print(f"      only REF  ({len(only_b)}): {only_b[:12]}")
        return

    diffs = [(m, dA[m], dB[m]) for m in dA if dA[m] != dB[m]]
    if not diffs:
        print("    (no per-move diff found — inconsistent totals?)")
        return
    mv, ca, cb = sorted(diffs)[0]
    print(f"    divide diff after [{' '.join(moves) or 'root'}] -> {mv}: ours={ca} ref={cb}"
          f" ({len(diffs)} moves differ)")
    if depth > 1:
        isolate(ours, ref, fen, moves + [mv], depth - 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ours")
    ap.add_argument("reference")
    ap.add_argument("--suite", default="perft_spell.csv")
    ap.add_argument("--max-depth", type=int, default=2)
    ap.add_argument("--fen-check", action="store_true")
    ap.add_argument("--fail-fast", action="store_true")
    args = ap.parse_args()

    rows = []
    with open(args.suite, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(";")
            rows.append((parts[0], [int(x) for x in parts[1:]]))

    ours = Engine(args.ours, needs_variant=True)
    ref  = Engine(args.reference, needs_variant=True)

    failures = 0
    for i, (fen, depths) in enumerate(rows):
        status = []
        mismatch_depth = None
        ours.position(fen, [])
        ref.position(fen, [])

        if args.fen_check:
            emitted = ours.fen()
            if emitted != fen:
                failures += 1
                print(f"[{i+1}/{len(rows)}] FEN MISMATCH\n    suite: {fen}\n    ours : {emitted}")

        for d, expected in enumerate(depths[:args.max_depth], start=1):
            ours.position(fen, [])
            _, total = ours.perft_divide(d)
            status.append(f"d{d}={total}{'' if total == expected else f'!={expected}'}")
            if total != expected and mismatch_depth is None:
                mismatch_depth = d

        line = f"[{i+1}/{len(rows)}] {' '.join(status)}  {fen}"
        if mismatch_depth is None:
            print("OK  " + line, flush=True)
        else:
            failures += 1
            print("FAIL " + line, flush=True)
            isolate(ours, ref, fen, [], mismatch_depth)
            if args.fail_fast:
                break

    ours.quit()
    ref.quit()
    print(f"\n{failures} failing positions out of {len(rows)}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
