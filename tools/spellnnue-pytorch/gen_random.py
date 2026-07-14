#!/usr/bin/env python3
"""Generate a random but structurally valid SPL2 (Spell-NNUE v2) network.

P0 gate tool (docs/spell-nnue-v2.md §8): the engine must load the file, run
bench and the test suite without crashing, and keep perft intact. Weights are
drawn small so the i16 accumulator stays far from saturation with ~130 active
features.

Usage: python gen_random.py [out.nnue] [--seed N]
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spl2


def random_params(rng):
    p = spl2.empty_params()
    p["ft_bias"] = rng.integers(-64, 65, spl2.L1, dtype=np.int64).astype(np.int16)
    p["ft_weight"] = rng.integers(-8, 9, (spl2.SPELL_DIMS, spl2.L1),
                                  dtype=np.int64).astype(np.int16)
    p["threat_weight"] = rng.integers(-8, 9, (spl2.THREAT_DIMS, spl2.L1),
                                      dtype=np.int64).astype(np.int8)
    p["psqt_weight"] = rng.integers(-512, 513, (spl2.SPELL_DIMS, spl2.PSQT_BUCKETS),
                                    dtype=np.int64).astype(np.int32)
    p["threat_psqt_weight"] = rng.integers(
        -512, 513, (spl2.THREAT_DIMS, spl2.PSQT_BUCKETS), dtype=np.int64).astype(np.int32)

    for s in p["stacks"]:
        s["fc0_bias"] = rng.integers(-1000, 1001, spl2.FC0_OUT, dtype=np.int64).astype(np.int32)
        s["fc0_weight"] = rng.integers(-32, 33, (spl2.FC0_OUT, spl2.L1),
                                       dtype=np.int64).astype(np.int8)
        s["fc1_bias"] = rng.integers(-1000, 1001, spl2.FC1_OUT, dtype=np.int64).astype(np.int32)
        s["fc1_weight"] = rng.integers(-64, 65, (spl2.FC1_OUT, spl2.FC1_IN),
                                       dtype=np.int64).astype(np.int8)
        s["fc2_bias"] = rng.integers(-1000, 1001, 1, dtype=np.int64).astype(np.int32)
        s["fc2_weight"] = rng.integers(-64, 65, (1, spl2.FC2_IN),
                                       dtype=np.int64).astype(np.int8)
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", default="spell-v2-random.nnue")
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    rng = np.random.default_rng(args.seed)
    params = random_params(rng)
    spl2.write_spl2(args.out, params, f"random SpellKAv2 net (seed {args.seed})")

    size = os.path.getsize(args.out)
    print(f"wrote {args.out} ({size / 1024 / 1024:.1f} MiB), "
          f"net hash 0x{spl2.net_hash():08X}")

    # Round-trip sanity: every array must come back bit-identical
    back, desc = spl2.read_spl2(args.out)
    assert np.array_equal(back["ft_bias"], params["ft_bias"])
    assert np.array_equal(back["ft_weight"], params["ft_weight"])
    assert np.array_equal(back["threat_weight"], params["threat_weight"])
    assert np.array_equal(back["psqt_weight"], params["psqt_weight"])
    assert np.array_equal(back["threat_psqt_weight"], params["threat_psqt_weight"])
    for a, b in zip(back["stacks"], params["stacks"]):
        for k in a:
            assert np.array_equal(a[k], b[k]), k
    print(f"round-trip OK ({desc!r})")


if __name__ == "__main__":
    main()
