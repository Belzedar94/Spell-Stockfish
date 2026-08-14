#!/usr/bin/env python3
"""Generate a random but structurally valid SPLA (Spell-NNUE A) network.

Gate tool: the engine must load the file, run bench and the test suite without
crashing, and keep perft intact. Weights are drawn small so the i16
accumulator stays far from saturation with ~80 active features. A conventional
material PSQT keeps the untrained gate network from creating a pathological
depth-bench tree by accident.

Usage: python gen_random_a.py [out.nnue] [--seed N]
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spla


def random_params(rng):
    p = spla.empty_params()
    p["ft_bias"] = rng.integers(64, 193, spla.L1, dtype=np.int64).astype(np.int16)
    p["ft_weight"] = rng.integers(-2, 3, (spla.SPELL_DIMS, spla.L1),
                                  dtype=np.int64).astype(np.int16)

    # Material-shaped PSQT keeps a random gate net useful as a depth-bench
    # smoke test instead of letting accidental chaotic ordering explode the
    # tree. It remains entirely untrained; FT and all stacks are random.
    for piece_type, value in enumerate((126, 781, 825, 1276, 2538)):
        own = piece_type * 128
        p["psqt_weight"][own:own + 64, :] = value * 16
        p["psqt_weight"][own + 64:own + 128, :] = -value * 16

    for s in p["stacks"]:
        s["fc0_bias"].fill(0)
        s["fc0_weight"] = rng.integers(-1, 2, (spla.FC0_OUT, spla.L1),
                                       dtype=np.int64).astype(np.int8)
        s["fc1_bias"].fill(0)
        s["fc1_weight"] = rng.integers(-2, 3, (spla.FC1_OUT, spla.FC1_IN),
                                       dtype=np.int64).astype(np.int8)
        s["fc2_bias"].fill(0)
        s["fc2_weight"] = rng.integers(-2, 3, (1, spla.FC2_IN),
                                       dtype=np.int64).astype(np.int8)
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", default="spell-a-random.nnue")
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    rng = np.random.default_rng(args.seed)
    spla.write_spla(args.out, random_params(rng),
                    f"SpellAv2 random gate net (seed {args.seed})")
    print(f"wrote {args.out} ({os.path.getsize(args.out):,} bytes)")


if __name__ == "__main__":
    main()
