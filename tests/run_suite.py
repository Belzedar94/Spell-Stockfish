#!/usr/bin/env python3
"""S5 suite orchestrator: runs every mandatory local gate in sequence and
reports a single verdict. Intended to run after any engine change and as
the local stand-in for CI while GitHub Actions stays blocked.

Usage:
  python run_suite.py [--engine PATH] [--net PATH] [--baseline PATH]
                      [--quick]   (quick: perft d1 only, skip parity)
"""

import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def run(label, cmd, cwd=None, timeout=1800):
    t0 = time.time()
    r = subprocess.run(cmd, cwd=cwd or HERE, capture_output=True,
                       text=True, timeout=timeout)
    ok = r.returncode == 0
    took = time.time() - t0
    print(f"{'PASS' if ok else 'FAIL'}  {label} ({took:.0f}s)")
    if not ok:
        tail = (r.stdout + r.stderr).strip().splitlines()[-8:]
        for l in tail:
            print(f"      {l}")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", default=os.path.join(ROOT, "src", "stockfish.exe"))
    ap.add_argument("--net", default=os.path.join(
        ROOT, "spell-chess_run5rl_e10_l07.nnue"))
    ap.add_argument("--baseline", default=os.path.join(
        os.path.dirname(ROOT), "FSF-spell-baseline", "FSF_Spell_test_baseline.exe"))
    ap.add_argument("--quick", action="store_true")
    args = ap.parse_args()

    py = sys.executable
    results = []

    results.append(run("unit tests (spell_tests.py)",
                       [py, "spell_tests.py", args.engine]))
    results.append(run("protocolo UCI (protocol_test.py)",
                       [py, "protocol_test.py", args.engine, args.net]))
    results.append(run("reproducibilidad (repro_test.py)",
                       [py, "repro_test.py", args.engine, args.net]))
    results.append(run("protocolo XBoard/CECP (xboard_test.py)",
                       [py, "xboard_test.py", args.engine]))

    depth = "1" if args.quick else "2"
    results.append(run(f"perft suite d{depth} vs oraculo",
                       [py, "compare_perft.py",
                        args.engine, args.baseline, "--max-depth", depth],
                       cwd=os.path.join(HERE, "reference"), timeout=3600))

    if not args.quick:
        results.append(run("eval-parity vs oraculo",
                           [py, "eval_parity.py",
                            args.engine, args.baseline, args.net],
                           cwd=os.path.join(HERE, "reference"), timeout=3600))

    print(f"\n{'SUITE PASS' if all(results) else 'SUITE FAIL'} "
          f"({sum(results)}/{len(results)})")
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
