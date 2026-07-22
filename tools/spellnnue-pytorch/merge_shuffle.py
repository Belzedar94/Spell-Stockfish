#!/usr/bin/env python3
"""Merge + shuffle global determinista de chunks run7 (manifest congelado).

Une N chunks .bz2 (o .run7 planos) en un unico .run7 barajado con una
permutacion NumPy seedeada. Trabaja a nivel de bytes (registros fijos de
44 B), preserva version/record_size de la cabecera y verifica el recuento.
"""

import argparse
import bz2
import glob
import os
import struct
import sys

import numpy as np

HEADER = struct.Struct("<4sHHQQQ")
MAGIC = b"RUN7"


def read_chunk(path):
    raw = (bz2.open(path, "rb").read() if path.endswith(".bz2")
           else open(path, "rb").read())
    magic, version, rec_size, count, _src, _flags = HEADER.unpack(
        raw[:HEADER.size])
    if magic != MAGIC:
        raise SystemExit(f"{path}: magic invalido {magic!r}")
    body = raw[HEADER.size:]
    if len(body) != count * rec_size:
        raise SystemExit(f"{path}: cuerpo {len(body)} != {count}x{rec_size}")
    return version, rec_size, count, body


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--chunks", required=True,
                    help="glob de chunks (p.ej. 'dir/chunk_*.bz2')")
    ap.add_argument("--out", required=True)
    ap.add_argument("--seed", type=int, required=True)
    a = ap.parse_args()

    paths = sorted(glob.glob(a.chunks),
                   key=lambda p: int("".join(filter(str.isdigit,
                                                    os.path.basename(p)))
                                     or 0))
    if not paths:
        raise SystemExit("sin chunks")
    print(f"{len(paths)} chunks", flush=True)

    version = rec_size = None
    parts, total = [], 0
    for i, p in enumerate(paths):
        v, r, c, body = read_chunk(p)
        if version is None:
            version, rec_size = v, r
        elif (v, r) != (version, rec_size):
            raise SystemExit(f"{p}: version/rec_size inconsistentes")
        parts.append(body)
        total += c
        if (i + 1) % 20 == 0:
            print(f"  leidos {i + 1}/{len(paths)} ({total:,} registros)",
                  flush=True)

    blob = np.frombuffer(b"".join(parts), dtype=np.uint8)
    del parts
    recs = blob.reshape(total, rec_size)
    rng = np.random.default_rng(a.seed)
    perm = rng.permutation(total)
    shuffled = recs[perm]
    print(f"barajados {total:,} registros (seed {a.seed})", flush=True)

    with open(a.out, "wb") as f:
        f.write(HEADER.pack(MAGIC, version, rec_size, total, len(paths), 0))
        shuffled.tofile(f)
    size = os.path.getsize(a.out)
    expect = HEADER.size + total * rec_size
    assert size == expect, (size, expect)
    print(f"OK {a.out}: {total:,} registros, {size:,} bytes", flush=True)


if __name__ == "__main__":
    sys.exit(main())
