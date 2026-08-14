#!/usr/bin/env python3
"""SPLA network format: python writer/reader for the Spell-NNUE A files
consumed by the engine (src/nnue/spell_a.{h,cpp}, docs/spell-nnue-a.md).

File layout (little-endian):
  u32 version = 0x53504C41 ("SPLA")
  u32 net_hash
  u32 desc_len,  desc bytes (utf-8)
  -- feature transformer --
  u32 ft_hash
  LEB128 block  biases   i16[1024]
  LEB128 block  weights  i16[1182][1024]     (row = feature)
  LEB128 block  psqt     i32[1182][16]
  -- 16 layer stacks, each --
  u32 arch_hash
  raw fc0_bias i32[32], fc0_weight i8[32][1024]       (row = output neuron)
  raw fc1_bias i32[32], fc1_weight i8[32][64]
  raw fc2_bias i32[1],  fc2_weight i8[1][128]

The layer stacks are byte-identical to SPL2; only the feature transformer
differs, so the stack helpers are shared with :mod:`spl2`.
"""

import struct

import numpy as np

import spl2
from spl2 import LEB_MAGIC, M32, leb128_decode, leb128_encode  # noqa: F401

VERSION = 0x53504C41

L1 = spl2.L1
SPELL_DIMS = 1182     # SpellAv2 per-perspective inputs
PSQT_BUCKETS = spl2.PSQT_BUCKETS
STACKS = spl2.STACKS
FC0_OUT = spl2.FC0_OUT
FC1_OUT = spl2.FC1_OUT
FC1_IN = spl2.FC1_IN
FC2_IN = spl2.FC2_IN

# Feature-set block layout (mirrors nnue/features/spell_a_v2.h)
PIECE_DIMS = 768
FREEZE_ZONE_BASE = 768
JUMP_ZONE_BASE = 896
FROZEN_BASE = 1024
GLOBAL_BASE = 1152
GLOBALS_PER_COLOR = 15

SPELLAV2_HASH = 0x4F234CB8


# ---------------------------------------------------------------------------
# Hash chain (FeatureTransformerA::get_hash_value / layer get_hash_value)
# ---------------------------------------------------------------------------

def ft_hash():
    return spl2._combine([SPELLAV2_HASH]) ^ (L1 * 2)


def arch_hash():
    return spl2.arch_hash()


def net_hash():
    return ft_hash() ^ arch_hash()


# ---------------------------------------------------------------------------
# Parameter containers
# ---------------------------------------------------------------------------

def empty_params():
    """All-zero quantized parameter set with the SPLA shapes."""
    return {
        "ft_bias": np.zeros(L1, dtype=np.int16),
        "ft_weight": np.zeros((SPELL_DIMS, L1), dtype=np.int16),
        "psqt_weight": np.zeros((SPELL_DIMS, PSQT_BUCKETS), dtype=np.int32),
        "stacks": [
            {
                "fc0_bias": np.zeros(FC0_OUT, dtype=np.int32),
                "fc0_weight": np.zeros((FC0_OUT, L1), dtype=np.int8),
                "fc1_bias": np.zeros(FC1_OUT, dtype=np.int32),
                "fc1_weight": np.zeros((FC1_OUT, FC1_IN), dtype=np.int8),
                "fc2_bias": np.zeros(1, dtype=np.int32),
                "fc2_weight": np.zeros((1, FC2_IN), dtype=np.int8),
            }
            for _ in range(STACKS)
        ],
    }


def _check_shapes(p):
    assert p["ft_bias"].shape == (L1,) and p["ft_bias"].dtype == np.int16
    assert p["ft_weight"].shape == (SPELL_DIMS, L1) and p["ft_weight"].dtype == np.int16
    assert p["psqt_weight"].shape == (SPELL_DIMS, PSQT_BUCKETS)
    assert len(p["stacks"]) == STACKS


def write_spla(path, params, description="SpellAv2 network"):
    _check_shapes(params)
    desc = description.encode("utf-8")

    with open(path, "wb") as f:
        f.write(struct.pack("<III", VERSION, net_hash(), len(desc)))
        f.write(desc)

        f.write(struct.pack("<I", ft_hash()))
        f.write(spl2._leb_block(params["ft_bias"]))
        f.write(spl2._leb_block(params["ft_weight"]))
        f.write(spl2._leb_block(params["psqt_weight"]))

        for s in params["stacks"]:
            f.write(struct.pack("<I", arch_hash()))
            f.write(np.ascontiguousarray(s["fc0_bias"], dtype=np.int32).tobytes())
            f.write(np.ascontiguousarray(s["fc0_weight"], dtype=np.int8).tobytes())
            f.write(np.ascontiguousarray(s["fc1_bias"], dtype=np.int32).tobytes())
            f.write(np.ascontiguousarray(s["fc1_weight"], dtype=np.int8).tobytes())
            f.write(np.ascontiguousarray(s["fc2_bias"], dtype=np.int32).tobytes())
            f.write(np.ascontiguousarray(s["fc2_weight"], dtype=np.int8).tobytes())


def read_spla(path):
    with open(path, "rb") as f:
        version, nhash, desc_len = struct.unpack("<III", f.read(12))
        if version != VERSION:
            raise ValueError(f"bad version 0x{version:08X}")
        if nhash != net_hash():
            raise ValueError("net hash mismatch")
        desc = f.read(desc_len).decode("utf-8")

        (fthash,) = struct.unpack("<I", f.read(4))
        if fthash != ft_hash():
            raise ValueError("ft hash mismatch")

        p = empty_params()
        p["ft_bias"] = spl2._read_leb_block(f, L1).astype(np.int16)
        p["ft_weight"] = spl2._read_leb_block(
            f, SPELL_DIMS * L1).astype(np.int16).reshape(SPELL_DIMS, L1)
        p["psqt_weight"] = spl2._read_leb_block(
            f, SPELL_DIMS * PSQT_BUCKETS).astype(np.int32).reshape(SPELL_DIMS, PSQT_BUCKETS)

        for s in p["stacks"]:
            (ahash,) = struct.unpack("<I", f.read(4))
            if ahash != arch_hash():
                raise ValueError("arch hash mismatch")
            s["fc0_bias"] = np.frombuffer(f.read(FC0_OUT * 4), dtype=np.int32).copy()
            s["fc0_weight"] = np.frombuffer(
                f.read(FC0_OUT * L1), dtype=np.int8).reshape(FC0_OUT, L1).copy()
            s["fc1_bias"] = np.frombuffer(f.read(FC1_OUT * 4), dtype=np.int32).copy()
            s["fc1_weight"] = np.frombuffer(
                f.read(FC1_OUT * FC1_IN), dtype=np.int8).reshape(FC1_OUT, FC1_IN).copy()
            s["fc2_bias"] = np.frombuffer(f.read(4), dtype=np.int32).copy()
            s["fc2_weight"] = np.frombuffer(
                f.read(FC2_IN), dtype=np.int8).reshape(1, FC2_IN).copy()

        rest = f.read()
        if rest:
            raise ValueError(f"{len(rest)} trailing bytes")

    return p, desc


if __name__ == "__main__":
    print(f"version   0x{VERSION:08X}")
    print(f"ft hash   0x{ft_hash():08X}")
    print(f"arch hash 0x{arch_hash():08X}")
    print(f"net hash  0x{net_hash():08X}")
