#!/usr/bin/env python3
"""Convert the existing 76-byte spell PackedSfenValue dataset to run7."""

from __future__ import annotations

import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, TOOLS)

import run7
import psv_decode


def convert(source: str, target: str, limit: int | None = None) -> int:
    source_size = os.path.getsize(source)
    if source_size % psv_decode.RECORD:
        raise ValueError(f"PSV size {source_size} is not divisible by {psv_decode.RECORD}")
    source_count = source_size // psv_decode.RECORD
    count = source_count if limit is None else min(source_count, limit)

    started = time.monotonic()
    with open(source, "rb", buffering=1024 * 1024) as src, \
         open(target, "wb", buffering=1024 * 1024) as dst:
        run7.write_header(dst, count, source_count)
        for index in range(count):
            raw = src.read(psv_decode.RECORD)
            if len(raw) != psv_decode.RECORD:
                raise ValueError(f"truncated PSV at record {index}")
            dst.write(run7.pack(run7.from_psv(psv_decode.decode(raw))))
            if (index + 1) % 100_000 == 0:
                elapsed = time.monotonic() - started
                print(f"{index + 1:,}/{count:,} ({(index + 1) / elapsed:,.0f} rec/s)", flush=True)

    expected = run7.HEADER_SIZE + count * run7.RECORD_SIZE
    actual = os.path.getsize(target)
    if actual != expected:
        raise RuntimeError(f"run7 size mismatch: {actual} != {expected}")
    return count


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", help="76-byte PackedSfenValue file")
    parser.add_argument("target", help="output .run7 file")
    parser.add_argument("--limit", type=int)
    args = parser.parse_args()
    count = convert(args.source, args.target, args.limit)
    print(f"wrote {count:,} run7 records to {args.target}")


if __name__ == "__main__":
    main()
