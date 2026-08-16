#!/usr/bin/env python3
"""Native loader vs pure-Python feature parity.

The native loader only earns the right to replace the Python extractors if it
agrees with them on every index of every position.  This harness runs both over
the same records and compares the full sparse batch: piece and spell indices per
perspective, FullThreats indices and the train only freeze factors for v2, plus
the output bucket, side to move, eval target and game result.

Two position sources are used, mirroring ``parity_a.py``:

* a real run7 file, which is what training actually consumes;
* synthetic positions, which reach gate, frozen and thermometer combinations a
  self-play file only meets rarely.

Live jump and live freeze zones are counted in both, and the run fails if the
sample is too thin to exercise them.
"""

from __future__ import annotations

import argparse
import mmap
import os
import random
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import numpy as np

import features
import features_a
import native_loader as nl
import parity_a
import run7
from train_overfit import training_score

ARCH_NAMES = {"a": nl.ARCH_A, "v2": nl.ARCH_V2}


def live_zones(record: run7.Record) -> tuple[bool, bool]:
    gates = features.normalized_gates(record)
    return (any(g >= 0 for g in (gates[1], gates[3])),
            any(g >= 0 for g in (gates[0], gates[2])))


def write_random_file(path: str, count: int, seed: int) -> int:
    """Writes ``count`` synthetic positions as a run7 file."""
    rng = random.Random(seed)
    records = [parity_a.random_record(rng) for _ in range(count)]
    with open(path, "wb") as file:
        run7.write_header(file, len(records))
        for record in records:
            file.write(run7.pack(record))
    return len(records)


def _bag_bounds(offsets: np.ndarray, total: int, bag: int) -> tuple[int, int]:
    start = int(offsets[bag])
    end = int(offsets[bag + 1]) if bag + 1 < offsets.shape[0] else total
    return start, end


def compare(path: str, arch: int, count: int, batch_size: int, workers: int,
            report_limit: int = 5) -> dict:
    stream = nl.NativeBatchStream(path, arch=arch, records=count, batch_size=batch_size,
                                  workers=workers, epochs=1, seed=0, device="cpu")
    mismatches = 0
    checked = 0
    live_jump = 0
    live_freeze = 0
    reported = 0

    with open(path, "rb") as file:
        with mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as mapped:
            for batch, release in stream.raw_batches():
                # Every field has to be read before release(): the batch goes
                # straight back into the worker's free pool and gets refilled.
                size = batch.size
                bags = 2 * size
                num_spell = int(batch.num_spell)
                num_threat = int(batch.num_threat)
                num_freeze = int(batch.num_freeze)
                spell = np.ctypeslib.as_array(batch.spell_indices,
                                              shape=(num_spell,)).copy()
                offsets = np.ctypeslib.as_array(batch.offsets, shape=(3, bags)).copy()
                stm = np.ctypeslib.as_array(batch.stm, shape=(size,)).copy()
                bucket = np.ctypeslib.as_array(batch.bucket, shape=(size,)).copy()
                target = np.ctypeslib.as_array(batch.target, shape=(size,)).copy()
                result = np.ctypeslib.as_array(batch.result, shape=(size,)).copy()
                source = np.ctypeslib.as_array(batch.source_index, shape=(size,)).copy()
                if arch == nl.ARCH_V2:
                    threat = np.ctypeslib.as_array(batch.threat_indices,
                                                   shape=(num_threat,)).copy()
                    freeze = np.ctypeslib.as_array(batch.freeze_indices,
                                                   shape=(num_freeze,)).copy()
                else:
                    threat = freeze = np.zeros(0, dtype=np.int64)
                release()

                for i in range(size):
                    index = int(source[i])
                    offset = run7.HEADER_SIZE + index * run7.RECORD_SIZE
                    record = run7.unpack(mapped[offset: offset + run7.RECORD_SIZE])
                    jump, frozen = live_zones(record)
                    live_jump += jump
                    live_freeze += frozen

                    if arch == nl.ARCH_A:
                        item = features_a.extract(record)
                        expected = {"psq_w": item.psq_white, "psq_b": item.psq_black}
                    else:
                        item = features.extract(record)
                        expected = {"psq_w": item.psq_white, "psq_b": item.psq_black,
                                    "threat_w": item.threats_white,
                                    "threat_b": item.threats_black}

                    actual = {}
                    for perspective, key in enumerate(("psq_w", "psq_b")):
                        start, end = _bag_bounds(offsets[0], num_spell, 2 * i + perspective)
                        actual[key] = tuple(int(x) for x in spell[start:end])
                    if arch == nl.ARCH_V2:
                        for perspective, key in enumerate(("threat_w", "threat_b")):
                            start, end = _bag_bounds(offsets[1], num_threat,
                                                     2 * i + perspective)
                            actual[key] = tuple(int(x) for x in threat[start:end])

                    problems = [key for key in expected if actual[key] != expected[key]]
                    if int(bucket[i]) != item.bucket:
                        problems.append("bucket")
                    if int(stm[i]) != record.stm:
                        problems.append("stm")
                    if float(target[i]) != training_score(record):
                        problems.append("target")
                    if float(result[i]) != float(record.result):
                        problems.append("result")

                    if arch == nl.ARCH_V2:
                        for perspective, psq in enumerate((item.psq_white, item.psq_black)):
                            start, end = _bag_bounds(offsets[2], num_freeze,
                                                     2 * i + perspective)
                            wanted = tuple(
                                (value - features.FREEZE_ZONE_BASE) % 64 for value in psq
                                if features.FREEZE_ZONE_BASE <= value < features.JUMP_ZONE_BASE)
                            if tuple(int(x) for x in freeze[start:end]) != wanted:
                                problems.append(f"freeze_factor[{perspective}]")

                    if problems:
                        mismatches += 1
                        if reported < report_limit:
                            reported += 1
                            print(f"mismatch record {index}: {', '.join(problems)}",
                                  file=sys.stderr)
                            print(f"  {run7.to_fen(record)}", file=sys.stderr)
                            for key in problems:
                                if key in expected:
                                    only_native = sorted(set(actual[key]) - set(expected[key]))
                                    only_python = sorted(set(expected[key]) - set(actual[key]))
                                    print(f"  {key}: only-native={only_native[:16]} "
                                          f"only-python={only_python[:16]}", file=sys.stderr)
                    checked += 1

    stream.close()
    return {"checked": checked, "mismatches": mismatches, "live_jump": live_jump,
            "live_freeze": live_freeze}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--data", required=True, help="run7 file with real positions")
    parser.add_argument("--arch", choices=sorted(ARCH_NAMES), default="a")
    parser.add_argument("--count", type=int, default=100_000,
                        help="real positions to compare")
    parser.add_argument("--random", type=int, default=20_000,
                        help="synthetic positions to compare (0 disables)")
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--batch-size", type=int, default=4096)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--scratch", default=None,
                        help="directory for the synthetic run7 file")
    parser.add_argument("--min-live-jump", type=int, default=200)
    parser.add_argument("--min-live-freeze", type=int, default=200)
    args = parser.parse_args()

    arch = ARCH_NAMES[args.arch]
    started = time.monotonic()
    totals = {"checked": 0, "mismatches": 0, "live_jump": 0, "live_freeze": 0}

    sources = [("file", args.data, args.count)]
    scratch = args.scratch or os.path.join(HERE, ".parity")
    if args.random:
        os.makedirs(scratch, exist_ok=True)
        synthetic = os.path.join(scratch, f"parity-random-{args.seed}.run7")
        written = write_random_file(synthetic, args.random, args.seed)
        sources.append(("random", synthetic, written))

    for label, path, count in sources:
        result = compare(path, arch, count, args.batch_size, args.workers)
        print(f"{label}: checked={result['checked']:,} mismatches={result['mismatches']} "
              f"live_jump={result['live_jump']:,} live_freeze={result['live_freeze']:,}",
              flush=True)
        for key in totals:
            totals[key] += result[key]

    elapsed = time.monotonic() - started
    if totals["live_jump"] < args.min_live_jump or totals["live_freeze"] < args.min_live_freeze:
        raise AssertionError(f"zone coverage too thin: {totals}")
    if totals["mismatches"]:
        raise AssertionError(f"parity gate failed: {totals}")
    print(f"NATIVE PARITY PASS ({args.arch}): {totals['checked']:,} positions, "
          f"live jump={totals['live_jump']:,}, live freeze={totals['live_freeze']:,}, "
          f"mismatches={totals['mismatches']} ({elapsed:.1f}s)")


if __name__ == "__main__":
    main()
