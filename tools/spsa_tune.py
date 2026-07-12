#!/usr/bin/env python3
"""SPSA tuner for the spell search parameters (self-play, fixed nodes).

Both sides run the SAME binary with symmetric parameter perturbations
(theta+ vs theta-), so engine speed cancels out and fixed-node games are a
faithful signal (unlike cross-engine probes). Node budget should roughly
match the real-TC node count so depth-dependent parameters tune for the
conditions they will play in.

Usage:
  python spsa_tune.py ENGINE.exe --nodes 600000 --pairs 600 -T 4 \
      --book "...spell-chess.epd" --net "...run5rl.nnue" \
      --state spsa_state.json

Classic SPSA with fishtest-style schedules: c_k = c / k^gamma,
r_k = r / k^alpha, update theta += r_k * c_k * pair_result * delta.
State persists to --state for resume; final values print at the end.
"""

import argparse
import json
import math
import os
import queue
import random
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fixed_nodes_match import Engine, EngineDied, play_game  # noqa: E402

# name, default, min, max
PARAMS = [
    # defaults = session-1 results (see spell_params.cpp)
    ("MaxFreezeGates",            9,      2,     32),
    ("MaxJumpGates",              4,      1,     20),
    ("SpellGateKingBonus",        11418,  1000,  30000),
    ("SpellGateKingRingBonus",    57185,  5000,  120000),
    ("SpellDepthPenaltyTactical", 1,      0,     3),
    ("SpellDepthPenaltyQuiet",    3,      0,     4),
    ("SpellTacticalLmrBonus",     1272,   0,     3072),
    ("SpellLmrMoveCountCap",      43,     4,     96),
    ("SpellGateHistOrderWeight",  1,      0,     8),
    ("SpellGateHistStatWeight",   1,      0,     8),
]

ALPHA, GAMMA = 0.602, 0.101


def clamp(v, lo, hi):
    return max(lo, min(hi, v))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("engine")
    ap.add_argument("--nodes", type=int, default=600000)
    ap.add_argument("--pairs", type=int, default=600)
    ap.add_argument("--book", required=True)
    ap.add_argument("--net", required=True)
    ap.add_argument("-T", "--threads", type=int, default=4)
    ap.add_argument("--state", default="spsa_state.json")
    ap.add_argument("--r-end", type=float, default=0.02,
                    help="learning rate at the end of the run")
    ap.add_argument("--adj-cp", type=int, default=800)
    args = ap.parse_args()

    with open(args.book, encoding="utf-8") as f:
        book = [l.strip() for l in f if l.strip() and not l.startswith("#")]

    # c ends at ~1/20 of the range; r scheduled to hit r_end at k = pairs
    c0 = {n: (hi - lo) / 12.0 for n, _, lo, hi in PARAMS}
    r0 = args.r_end * (args.pairs ** ALPHA)

    if os.path.exists(args.state):
        st = json.loads(open(args.state, encoding="utf-8").read())
        theta, k0 = st["theta"], st["k"]
        print(f"resuming at k={k0}", flush=True)
    else:
        theta, k0 = {n: float(d) for n, d, _, _ in PARAMS}, 0

    lock = threading.Lock()
    kbox = {"k": k0}
    jobs = queue.Queue()
    for i in range(k0, args.pairs):
        jobs.put(i)

    base_opts = [("Threads", "1"), ("Hash", "256"), ("EvalFile", args.net)]

    def snap(th):
        return {n: int(round(clamp(th[n], lo, hi))) for n, _, lo, hi in PARAMS}

    def worker(wid):
        e1 = Engine(args.engine, base_opts, f"w{wid}p")   # plays theta+
        e2 = Engine(args.engine, base_opts, f"w{wid}m")   # plays theta-
        rng = random.Random(1000 + wid)
        while True:
            try:
                i = jobs.get_nowait()
            except queue.Empty:
                break
            k = i + 1
            ck = {n: c0[n] / (k ** GAMMA) for n, _, _, _ in PARAMS}
            rk = r0 / (k ** ALPHA)

            with lock:
                th = dict(theta)
            delta = {n: rng.choice((-1, 1)) for n, _, _, _ in PARAMS}
            plus, minus = {}, {}
            for n, _, lo, hi in PARAMS:
                plus[n]  = int(round(clamp(th[n] + ck[n] * delta[n], lo, hi)))
                minus[n] = int(round(clamp(th[n] - ck[n] * delta[n], lo, hi)))

            try:
                for e, vals in ((e1, plus), (e2, minus)):
                    e.send("ucinewgame")
                    for n, v in vals.items():
                        e.send(f"setoption name {n} value {v}")
                    e.sync()

                fen = book[(i * 37) % len(book)]
                r1, *_ = play_game(e1, e2, fen, args.nodes, args.adj_cp, 4, 300)
                for e in (e1, e2):
                    e.send("ucinewgame")
                    e.sync()
                r2, *_ = play_game(e2, e1, fen, args.nodes, args.adj_cp, 4, 300)
                result = r1 - r2  # theta+ score of the pair, in [-2, +2]
            except EngineDied as exc:
                print(f"ENGINE DIED in pair {i}: {exc}", flush=True)
                for e in (e1, e2):
                    e.quit()
                e1 = Engine(args.engine, base_opts, f"w{wid}p")
                e2 = Engine(args.engine, base_opts, f"w{wid}m")
                jobs.task_done()
                continue

            with lock:
                for n, _, lo, hi in PARAMS:
                    theta[n] = clamp(theta[n] + rk * ck[n] * result * delta[n], lo, hi)
                kbox["k"] += 1
                kk = kbox["k"]
                if kk % 10 == 0:
                    json.dump({"k": kk, "theta": theta},
                              open(args.state, "w", encoding="utf-8"), indent=1)
                    print(f"[{kk}/{args.pairs}] " +
                          " ".join(f"{n}={v}" for n, v in snap(theta).items()),
                          flush=True)
            jobs.task_done()
        e1.quit()
        e2.quit()

    threads = [threading.Thread(target=worker, args=(w,)) for w in range(args.threads)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    json.dump({"k": kbox["k"], "theta": theta},
              open(args.state, "w", encoding="utf-8"), indent=1)
    print("\nFINAL:", flush=True)
    for n, v in snap(theta).items():
        print(f"  {n} = {v}", flush=True)


if __name__ == "__main__":
    main()
