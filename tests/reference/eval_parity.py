#!/usr/bin/env python3
"""Eval-parity harness: compares Spell-Stockfish's variant-NNUE evaluation
against the frozen reference engine over the perft suite positions.

Ours reports exact integers via `evalspell` (side-to-move perspective);
the reference reports "NNUE evaluation" / "Final evaluation" in pawns from
White's perspective via `eval`, so values are converted with PawnValueEg=208
and sign-flipped for black-to-move positions (tolerance +/-2 internal units
for the 2-decimal rounding).

Usage:
    python eval_parity.py <ours> <reference> <netpath> [--suite perft_spell.csv]
"""

import argparse
import re
import subprocess
import sys
import time

PAWN_VALUE = 208  # FSF PawnValueEg used by its eval printout


class Engine:
    def __init__(self, path, opts):
        self.proc = subprocess.Popen(
            [path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.send("uci")
        self.read_until("uciok")
        for k, v in opts.items():
            self.send(f"setoption name {k} value {v}")
        self.sync()

    def send(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def read_until(self, token, timeout=120):
        deadline = time.time() + timeout
        lines = []
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("engine died")
            line = line.rstrip("\n")
            lines.append(line)
            if line.startswith(token):
                return lines
        raise RuntimeError(f"timeout: {token}")

    def sync(self):
        self.send("isready")
        self.read_until("readyok")

    def position(self, fen):
        self.send(f"position fen {fen}")
        self.sync()

    def quit(self):
        try:
            self.send("quit")
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()


def ours_eval(e):
    e.send("evalspell")
    for line in e.read_until("spellnnue"):
        m = re.match(r"spellnnue raw (-?\d+) adjusted (-?\d+) scaled (-?\d+)", line)
        if m:
            return tuple(int(x) for x in m.groups())
    raise RuntimeError("no evalspell output")


def ref_eval(e):
    e.send("eval")
    e.send("isready")
    nnue = final = None
    deadline = time.time() + 60
    while time.time() < deadline:
        line = e.proc.stdout.readline()
        if not line:
            raise RuntimeError("reference died")
        line = line.rstrip("\n")
        m = re.search(r"NNUE evaluation\s+([+-]?\d+\.\d+)", line)
        if m:
            nnue = float(m.group(1))
        m = re.search(r"Final evaluation\s+([+-]?\d+\.\d+)", line)
        if m:
            final = float(m.group(1))
        if line.startswith("readyok"):
            return nnue, final
    raise RuntimeError("timeout in ref eval")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ours")
    ap.add_argument("reference")
    ap.add_argument("net")
    ap.add_argument("--suite", default="perft_spell.csv")
    ap.add_argument("--tolerance", type=int, default=2)
    args = ap.parse_args()

    fens = []
    with open(args.suite, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                fens.append(line.split(";")[0])

    ours = Engine(args.ours, {"EvalFile": args.net})
    ref  = Engine(args.reference,
                  {"UCI_Variant": "spell-chess", "EvalFile": args.net, "Use NNUE": "true"})

    raw_fail = final_deltas = 0
    deltas_final = []
    for i, fen in enumerate(fens):
        stm_black = " b " in fen  # the lone 'b' token can only be the side to move
        ours.position(fen)
        ref.position(fen)

        raw, adjusted, scaled = ours_eval(ours)
        nnue_ref, final_ref = ref_eval(ref)

        if nnue_ref is None:
            print(f"[{i+1}] no reference NNUE line  {fen}")
            continue

        sign = -1 if stm_black else 1  # ref prints white-side pawns, ours is stm-side internal
        ref_raw_internal   = round(nnue_ref * PAWN_VALUE) * sign
        ref_final_internal = round(final_ref * PAWN_VALUE) * sign if final_ref is not None else None

        d_raw = raw - ref_raw_internal
        ok = abs(d_raw) <= args.tolerance
        if not ok:
            raw_fail += 1
            print(f"[{i+1}] RAW MISMATCH ours={raw} ref={ref_raw_internal} (d={d_raw})  {fen}")
        if ref_final_internal is not None:
            deltas_final.append(scaled - ref_final_internal)

    print(f"\nraw: {len(fens) - raw_fail}/{len(fens)} within +/-{args.tolerance}")
    if deltas_final:
        exact = sum(1 for d in deltas_final if abs(d) <= args.tolerance)
        near  = sum(1 for d in deltas_final if abs(d) <= 8)
        lo, hi = min(deltas_final), max(deltas_final)
        avg = sum(deltas_final) / len(deltas_final)
        print(f"final-vs-scaled: {exact}/{len(deltas_final)} within +/-{args.tolerance}, "
              f"{near}/{len(deltas_final)} within +/-8, min={lo} max={hi} avg={avg:.1f}")
        outliers = sorted((abs(d), d) for d in deltas_final)[-6:]
        print(f"largest |deltas|: {[d for _, d in outliers]}")

    ours.quit()
    ref.quit()
    sys.exit(1 if raw_fail else 0)


if __name__ == "__main__":
    main()
