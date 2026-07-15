#!/usr/bin/env python3
"""SPL2 network format: python writer/reader for the Spell-NNUE v2 files
consumed by the engine (src/nnue/spell_v2.{h,cpp}, docs/spell-nnue-v2.md §5).

File layout (little-endian):
  u32 version = 0x53504C33 (SPL2 semantic revision: spell-aware FullThreats)
  u32 net_hash
  u32 desc_len,  desc bytes (utf-8)
  -- feature transformer --
  u32 ft_hash
  LEB128 block  biases           i16[1024]
  raw           threat_weights   i8 [60720][1024]     (row = feature)
  LEB128 block  threat_psqt      i32[60720][16]
  LEB128 block  weights          i16[26910][1024]     (row = feature)
  LEB128 block  psqt             i32[26910][16]
  -- 16 layer stacks, each --
  u32 arch_hash
  raw fc0_bias i32[32], fc0_weight i8[32][1024]       (row = output neuron)
  raw fc1_bias i32[32], fc1_weight i8[32][64]
  raw fc2_bias i32[1],  fc2_weight i8[1][128]

A LEB128 block is: b"COMPRESSED_LEB128" + u32 byte_count + signed-LEB128
stream of every element in row-major order (same as nnue_common.h).

The hash constants replicate the C++ chain so a mismatched binary refuses the
file at load (Detail::read_parameters / NetworkV2::read_parameters).
"""

import struct

import numpy as np

M32 = 0xFFFFFFFF

LEGACY_VERSION = 0x53504C32
VERSION = 0x53504C33

L1 = 1024
SPELL_DIMS = 26910    # SpellKAv2 per-perspective inputs
THREAT_DIMS = 60720   # FullThreats
PSQT_BUCKETS = 16
STACKS = 16
FC0_OUT = 32
FC1_OUT = 32
FC1_IN = FC0_OUT * 2              # sqr-CReLU + CReLU concat
FC2_IN = FC0_OUT * 2 + FC1_OUT * 2

# Feature-set block layout (mirrors nnue/features/spell_ka_v2.h)
PIECE_DIMS = 22528
FREEZE_ZONE_BASE = 22528
JUMP_ZONE_BASE = 26624
FROZEN_BASE = 26752
GLOBAL_BASE = 26880
GLOBALS_PER_COLOR = 15

HALFKA_HASH = 0x7F234CB8
FULLTHREATS_HASH = 0x8F234CB8
SPELLKAV2_HASH = 0x5F234CB8

LEB_MAGIC = b"COMPRESSED_LEB128"


# ---------------------------------------------------------------------------
# Hash chain (FeatureTransformer::get_hash_value / layer get_hash_value)
# ---------------------------------------------------------------------------

def _combine(hashes):
    h = 0
    for c in hashes:
        h = ((h << 1) | (h >> 31)) & M32
        h ^= c
    return h & M32


def _affine_hash(prev, out_dims):
    h = (0xCC03DAE4 + out_dims) & M32
    h ^= prev >> 1
    h ^= (prev << 31) & M32
    return h & M32


def _crelu_hash(prev):
    return (0x538D24C7 + prev) & M32


def ft_hash():
    return _combine([FULLTHREATS_HASH, SPELLKAV2_HASH]) ^ (L1 * 2)


def arch_hash():
    h = (0xEC42E90D ^ (L1 * 2)) & M32
    h = _affine_hash(h, FC0_OUT)   # fc_0 (sparse input)
    h = _crelu_hash(h)             # ac_0
    h = _affine_hash(h, FC1_OUT)   # fc_1
    h = _crelu_hash(h)             # ac_1
    h = _affine_hash(h, 1)         # fc_2
    return h


def net_hash():
    return ft_hash() ^ arch_hash()


# ---------------------------------------------------------------------------
# Signed LEB128 (vectorized numpy port of nnue_common.h)
# ---------------------------------------------------------------------------

def leb128_encode(arr):
    v = np.asarray(arr).astype(np.int64).ravel()
    byte_cols, active_cols = [], []
    pending = v
    active = np.ones(len(v), dtype=bool)
    while active.any():
        b = (pending & 0x7F).astype(np.uint8)
        nxt = pending >> 7
        sign_clear = (b & 0x40) == 0
        done = np.where(sign_clear, nxt == 0, nxt == -1) & active
        cont = active & ~done
        b = b.copy()
        b[cont] |= 0x80
        byte_cols.append(b)
        active_cols.append(active)
        pending = nxt
        active = cont
    mat = np.stack(byte_cols, axis=1)
    act = np.stack(active_cols, axis=1)
    return mat[act].tobytes()


def leb128_decode(buf, count, bits=32):
    a = np.frombuffer(buf, dtype=np.uint8).astype(np.int64)
    term = (a & 0x80) == 0
    idx = np.flatnonzero(term)
    if len(idx) != count:
        raise ValueError(f"LEB128 stream holds {len(idx)} values, expected {count}")
    starts = np.concatenate(([0], idx[:-1] + 1))
    lengths = idx - starts + 1
    vals = np.zeros(count, dtype=np.int64)
    for k in range(int(lengths.max())):
        sel = lengths > k
        vals[sel] |= (a[starts[sel] + k] & 0x7F) << (7 * k)
    # sign-extend exactly like the C++ reader: only when shift < 32
    shift = 7 * lengths
    ext = ((a[idx] & 0x40) != 0) & (shift < bits)
    vals[ext] |= (-1) << shift[ext]
    # truncate to 32-bit two's complement like the i32 accumulator
    vals = (vals & M32).astype(np.uint32).view(np.int32).astype(np.int64)
    return vals


def _leb_block(arr):
    stream = leb128_encode(arr)
    return LEB_MAGIC + struct.pack("<I", len(stream)) + stream


def _read_leb_block(f, count):
    magic = f.read(len(LEB_MAGIC))
    if magic != LEB_MAGIC:
        raise ValueError("missing LEB128 magic")
    (n,) = struct.unpack("<I", f.read(4))
    return leb128_decode(f.read(n), count)


# ---------------------------------------------------------------------------
# Parameter containers
# ---------------------------------------------------------------------------

def empty_params():
    """All-zero quantized parameter set with the SPL2 shapes."""
    return {
        "ft_bias": np.zeros(L1, dtype=np.int16),
        "ft_weight": np.zeros((SPELL_DIMS, L1), dtype=np.int16),
        "threat_weight": np.zeros((THREAT_DIMS, L1), dtype=np.int8),
        "psqt_weight": np.zeros((SPELL_DIMS, PSQT_BUCKETS), dtype=np.int32),
        "threat_psqt_weight": np.zeros((THREAT_DIMS, PSQT_BUCKETS), dtype=np.int32),
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
    assert p["threat_weight"].shape == (THREAT_DIMS, L1) and p["threat_weight"].dtype == np.int8
    assert p["psqt_weight"].shape == (SPELL_DIMS, PSQT_BUCKETS)
    assert p["threat_psqt_weight"].shape == (THREAT_DIMS, PSQT_BUCKETS)
    assert len(p["stacks"]) == STACKS


def write_spl2(path, params, description="SpellKAv2 network"):
    _check_shapes(params)
    desc = description.encode("utf-8")

    with open(path, "wb") as f:
        f.write(struct.pack("<III", VERSION, net_hash(), len(desc)))
        f.write(desc)

        f.write(struct.pack("<I", ft_hash()))
        f.write(_leb_block(params["ft_bias"]))
        f.write(np.ascontiguousarray(params["threat_weight"], dtype=np.int8).tobytes())
        f.write(_leb_block(params["threat_psqt_weight"]))
        f.write(_leb_block(params["ft_weight"]))
        f.write(_leb_block(params["psqt_weight"]))

        for s in params["stacks"]:
            f.write(struct.pack("<I", arch_hash()))
            f.write(np.ascontiguousarray(s["fc0_bias"], dtype=np.int32).tobytes())
            f.write(np.ascontiguousarray(s["fc0_weight"], dtype=np.int8).tobytes())
            f.write(np.ascontiguousarray(s["fc1_bias"], dtype=np.int32).tobytes())
            f.write(np.ascontiguousarray(s["fc1_weight"], dtype=np.int8).tobytes())
            f.write(np.ascontiguousarray(s["fc2_bias"], dtype=np.int32).tobytes())
            f.write(np.ascontiguousarray(s["fc2_weight"], dtype=np.int8).tobytes())


def read_spl2(path):
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
        p["ft_bias"] = _read_leb_block(f, L1).astype(np.int16)
        p["threat_weight"] = np.frombuffer(
            f.read(THREAT_DIMS * L1), dtype=np.int8).reshape(THREAT_DIMS, L1).copy()
        p["threat_psqt_weight"] = _read_leb_block(
            f, THREAT_DIMS * PSQT_BUCKETS).astype(np.int32).reshape(THREAT_DIMS, PSQT_BUCKETS)
        p["ft_weight"] = _read_leb_block(
            f, SPELL_DIMS * L1).astype(np.int16).reshape(SPELL_DIMS, L1)
        p["psqt_weight"] = _read_leb_block(
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
