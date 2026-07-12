#!/usr/bin/env python3
"""Parallel training-data generation farm.

Spawns N engine processes, each running the native `datagen` command with a
distinct seed and output shard; reports aggregate throughput and validates
every shard with psv_decode before declaring success.

Usage:
  python datagen_farm.py ENGINE.exe --net NET.nnue --outdir DIR \
      --procs 12 --count-per 1000000 --nodes 5000 [--random-plies 8]
      [--eval-limit 3000] [--seed-base 1000]

Shards are DIR/part_<seed>.bin (append-safe: rerunning with the same args
resumes into new positions; use fresh seeds for genuinely new data).
Concatenate for training with:  cat part_*.bin > run6.bin  (records are
fixed 76 bytes, plain concatenation is valid).
"""

import argparse
import os
import subprocess
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("engine")
    ap.add_argument("--net", required=True)
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--procs", type=int, default=12)
    ap.add_argument("--count-per", type=int, default=1000000)
    ap.add_argument("--nodes", type=int, default=5000)
    ap.add_argument("--random-plies", type=int, default=8)
    ap.add_argument("--eval-limit", type=int, default=3000)
    ap.add_argument("--seed-base", type=int, default=1000)
    args = ap.parse_args()

    # Only the datagen `out` argument is a single UCI token (the engine
    # path goes through Popen argv and EvalFile reads the rest of the line)
    if " " in args.outdir:
        sys.exit(f"outdir must not contain spaces (single-token UCI arg): "
                 f"{args.outdir}")
    os.makedirs(args.outdir, exist_ok=True)
    procs = []
    t0 = time.time()

    for i in range(args.procs):
        seed = args.seed_base + i
        out = os.path.join(args.outdir, f"part_{seed}.bin").replace("\\", "/")
        cmds = (
            f"setoption name EvalFile value {args.net}\n"
            f"setoption name Threads value 1\n"
            f"setoption name Hash value 256\n"
            f"datagen out {out} count {args.count_per} nodes {args.nodes} "
            f"seed {seed} random_plies {args.random_plies} "
            f"eval_limit {args.eval_limit}\n"
            "quit\n"
        )
        # Keep each worker's output: engine errors (bad paths, failed net
        # loads) are invisible otherwise. NOTE: the datagen UCI parser reads
        # single tokens — paths must not contain spaces.
        wlog = open(os.path.join(args.outdir, f"worker_{seed}.log"), "w")
        p = subprocess.Popen([args.engine], stdin=subprocess.PIPE,
                             stdout=wlog, stderr=subprocess.STDOUT, text=True)
        p.stdin.write(cmds)
        p.stdin.flush()
        procs.append((p, out, seed))
        print(f"worker {i}: seed {seed} -> {out}", flush=True)

    # Progress loop: sum shard sizes until all workers exit
    RECORD = 76
    target = args.procs * args.count_per
    while any(p.poll() is None for p, _, _ in procs):
        time.sleep(30)
        total = sum(os.path.getsize(o) // RECORD for _, o, _ in procs
                    if os.path.exists(o))
        els = time.time() - t0
        print(f"[{els/60:.0f}min] {total:,}/{target:,} positions "
              f"({total/max(els,1):.0f} pos/s)", flush=True)

    failures = [(o, p.returncode) for p, o, _ in procs if p.returncode != 0]
    if failures:
        print(f"WORKER FAILURES: {failures}")
        sys.exit(1)

    # Validate every shard structurally
    here = os.path.dirname(os.path.abspath(__file__))
    bad = 0
    for _, out, _ in procs:
        if not os.path.exists(out):
            print(f"validate {out}: MISSING (worker produced nothing)", flush=True)
            bad += 1
            continue
        r = subprocess.run([sys.executable, os.path.join(here, "psv_decode.py"),
                            out, "--quiet"], capture_output=True, text=True)
        status = "OK" if r.returncode == 0 else "BAD"
        bad += r.returncode != 0
        tail = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr.strip()[:120]
        print(f"validate {out}: {status} {tail}", flush=True)

    total = sum(os.path.getsize(o) // RECORD for _, o, _ in procs)
    print(f"\nDONE: {total:,} positions in {(time.time()-t0)/3600:.2f}h, "
          f"{bad} bad shards")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
