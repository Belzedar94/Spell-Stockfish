#!/usr/bin/env python3
"""Convert a run7 corpus into spell-bin v1, the 76-byte legacy training format.

The legacy (FSF era) trainer only reads `.bin`, a bare concatenation of
`PackedSfenValue` records with no header, defined normatively by
`docs/spell-bin-v1.md` and consumed by `lib/nnue_training_data_formats.h` in
`../Spell-nnue-pytorch`.  The source is the 44-byte run7 record defined by
`run7.py` in this directory.  Both formats carry exactly the same state, so the
conversion is lossless in both directions except for the run7 header, which has
no counterpart in spell-bin v1.

Field mapping
-------------
Position state (all of it lands inside the 512-bit `sfen` block):

    run7                         spell-bin v1
    board (nibbles, LSB square)  king squares (7 bits each) plus the rank 8 to
                                 rank 1 / file a to file h Huffman scan that
                                 skips both king squares
    stm                          side to move, bit 0
    hands (WF, WJ, BF, BJ)       hand slots 6 (Freeze) and 7 (Jump) of each
                                 colour, in FSF piece-id order P N B R Q K F J
    gates (WF, WJ, BF, BJ)       potion block, one per colour and spell, as
                                 present flag plus 7-bit zone CENTRE
    cooldowns (WF, WJ, BF, BJ)   potion block, 16-bit cooldown
    castling                     4 bits, K Q k q
    ep                           present flag plus 7-bit square
    rule50                       6 low bits, then 1 high bit after fullmove
    fullmove                     16 bits, low byte then high byte

The potion block sits BETWEEN the hand counts and the castling rights.  A zone
is a centre square and not a mask: the legacy decoder expands freeze into the
3x3 neighbourhood clipped at the edges and jump into the centre square alone,
which is exactly `spell_zone_bb` in the engine, so the centres carry over
unchanged.

Targets (the 12-byte tail) map one to one, with no sign or perspective change:

    score      i16 at offset 64.  Both formats store the engine's search score
               in internal units from the POV of the side to move, and both
               clamp at +/-32000.  `Datagen::pack_record` already clamps, so the
               clamp here is a no-op kept as a guard.  Note that run7 stores
               +32000 on the mandatory pre-king-capture terminal, which is a
               legitimate value of the format and not a mate score.
    move       u32 at offset 68.  Copied verbatim.  This is `Move::raw()` of
               this engine, which is deliberately not the FSF reference move
               encoding (see docs/spell-bin-v1.md par. 3).  The legacy trainer
               does not decode it at lambda 1.0.
    gamePly    u16 at offset 72.  Copied verbatim.
    gameResult i8 at offset 74.  Both formats store the final result from the
               POV of the side to move IN THIS RECORD: +1 the mover ends up
               winning, -1 loses, 0 draw.  The run7 producer already patches
               every record of a game with that perspective, so the value is
               copied verbatim (run7 stores it biased by +1 in two bits, which
               `run7.unpack` has already undone).

Offsets 66 to 67 and offset 75 are padding and are written as zero.

Known and accepted asymmetry: the legacy feature loop runs
`for pt = Pawn; pt < King; ++pt` with `King == MaxPiece == 7`, so hand slot 7
never becomes a feature and the legacy net is blind to Jump in hand.  That is a
property of that trainer, not of the data.  The converter writes the jump hand
faithfully, as the format requires.

Usage
-----
    python convert_run7_to_spellbin.py SOURCE.run7 --out OUT.bin \
        [--jobs 5] [--block 50000] [--records N] \
        [--val-out VAL.bin --val-records 1000000] \
        [--slice-out SLICE.bin --slice-records 100000] \
        [--spot-check 10000] [--seed 20260816]

    python convert_run7_to_spellbin.py SOURCE.run7 --out OUT.bin \
        --check-only --spot-check 10000

The conversion is streaming and multiprocess: the output is preallocated and
each worker writes its own disjoint record range, so nothing buffers the whole
corpus.  `--spot-check` compares N random records field by field between the
run7 source, decoded by `run7.py`, and the converted output, decoded by the
independent validator `tools/psv_decode.py`.
"""

from __future__ import annotations

import argparse
import ctypes
import importlib.util
import os
import random
import sys
import time
from multiprocessing import Pool

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run7  # noqa: E402

RUN7_HEADER = run7.HEADER_SIZE
RUN7_RECORD = run7.RECORD_SIZE
PSV_RECORD = 76
SFEN_BITS = 512
SCORE_CLAMP = 32000

# Board scan order of the sfen bitstream: rank 8 down to rank 1, file a to h.
SCAN = [rank * 8 + file for rank in range(7, -1, -1) for file in range(8)]

# run7 metadata bit offsets, from the writer in run7.pack / Datagen::pack_record.
META_STM = 0
META_CASTLING = 1
META_EP = 5
META_RULE50 = 12
META_FULLMOVE = 19
META_HANDS = ((35, 3), (38, 2), (40, 3), (43, 2))  # WF WJ BF BJ
META_COOLDOWNS = 45  # four 2-bit fields, same order
META_GATES = 53  # four 7-bit fields, same order, stored as square + 1
META_SCORE = 81
META_MOVE = 97
META_PLY = 129
META_RESULT = 145
META_USED = 147

TAIL_DTYPE = np.dtype([("score", "<i2"), ("pad0", "<u2"), ("move", "<u4"),
                       ("ply", "<u2"), ("result", "i1"), ("pad1", "u1")])
assert TAIL_DTYPE.itemsize == PSV_RECORD - 64

STAT_KEYS = ("records", "score_clamped", "live_freeze", "live_jump", "with_ep",
             "with_castling", "freeze_in_hand", "jump_in_hand", "result_win",
             "result_draw", "result_loss", "stm_black")


def lower_priority() -> None:
    """Drop this process below normal priority so it yields to the fleet."""
    try:
        if os.name == "nt":
            below_normal = 0x00004000
            kernel32 = ctypes.windll.kernel32  # type: ignore[attr-defined]
            kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), below_normal)
        else:
            os.nice(10)
    except Exception:  # pragma: no cover - best effort only
        pass


def field(bits: np.ndarray, offset: int, width: int, dtype=np.uint32) -> np.ndarray:
    """Read a little-endian bit field out of an unpacked (N, bits) array."""
    out = np.zeros(bits.shape[0], dtype=dtype)
    for index in range(width):
        out |= bits[:, offset + index].astype(dtype) << index
    return out


class BitPlane:
    """One unpacked bit per byte, packed to bytes at the end."""

    __slots__ = ("bits", "rows")

    def __init__(self, count: int) -> None:
        self.bits = np.zeros((count, SFEN_BITS), dtype=np.uint8)
        self.rows = np.arange(count)

    def put(self, value, width: int, position, mask=None) -> None:
        """Set the one bits of `value` at `position` .. `position + width - 1`."""
        for index in range(width):
            selected = ((value >> index) & 1).astype(bool)
            if mask is not None:
                selected = selected & mask
            if not selected.any():
                continue
            where = self.rows[selected]
            at = position[selected] if isinstance(position, np.ndarray) else position
            self.bits[where, at + index] = 1

    def set_flag(self, mask: np.ndarray, position: np.ndarray) -> None:
        if not mask.any():
            return
        self.bits[self.rows[mask], position[mask]] = 1

    def pack(self) -> np.ndarray:
        return np.packbits(self.bits, axis=1, bitorder="little")


def _first_bad(mask: np.ndarray, base: int) -> int:
    return base + int(np.argmax(mask))


def encode_block(raw: np.ndarray, base: int) -> tuple[np.ndarray, dict]:
    """Convert `raw`, a (N, 44) run7 block, into a (N, 76) spell-bin v1 block."""
    count = raw.shape[0]

    # ---- decode run7 -----------------------------------------------------
    occupancy = np.unpackbits(np.ascontiguousarray(raw[:, 0:8]), axis=1,
                              bitorder="little")
    packed = raw[:, 8:24]
    nibbles = np.empty((count, 32), dtype=np.uint8)
    nibbles[:, 0::2] = packed & 15
    nibbles[:, 1::2] = packed >> 4

    occupied = occupancy.sum(axis=1, dtype=np.int32)
    if np.any(occupied > 32):
        raise ValueError(f"record {_first_bad(occupied > 32, base)}: more than 32 pieces")
    tail_slots = np.arange(32)[None, :] >= occupied[:, None]
    stray = (nibbles != 0) & tail_slots
    if stray.any():
        raise ValueError(f"record {_first_bad(stray.any(axis=1), base)}: "
                         "non-zero run7 piece padding")

    order = np.cumsum(occupancy, axis=1, dtype=np.int16) - 1
    board = np.where(occupancy != 0,
                     np.take_along_axis(nibbles, np.clip(order, 0, 31), axis=1),
                     0).astype(np.int16)
    if np.any(board > 12):
        raise ValueError(f"record {_first_bad((board > 12).any(axis=1), base)}: "
                         "invalid run7 piece nibble")

    meta = np.unpackbits(np.ascontiguousarray(raw[:, 24:44]), axis=1,
                         bitorder="little")
    if meta[:, META_USED:].any():
        raise ValueError(f"record {_first_bad(meta[:, META_USED:].any(axis=1), base)}: "
                         "non-zero run7 metadata padding")

    stm = field(meta, META_STM, 1)
    castling = field(meta, META_CASTLING, 4)
    ep = field(meta, META_EP, 7).astype(np.int32) - 1
    rule50 = field(meta, META_RULE50, 7)
    fullmove = field(meta, META_FULLMOVE, 16)
    hands = [field(meta, offset, width) for offset, width in META_HANDS]
    cooldowns = [field(meta, META_COOLDOWNS + 2 * i, 2) for i in range(4)]
    gates = [field(meta, META_GATES + 7 * i, 7).astype(np.int32) - 1 for i in range(4)]
    score = field(meta, META_SCORE, 16).astype(np.int32)
    score = np.where(score >= 1 << 15, score - (1 << 16), score)
    move = field(meta, META_MOVE, 32)
    ply = field(meta, META_PLY, 16)
    result = field(meta, META_RESULT, 2).astype(np.int32) - 1

    if np.any(result > 1):
        raise ValueError(f"record {_first_bad(result > 1, base)}: invalid run7 result")
    if np.any(fullmove < 1):
        raise ValueError(f"record {_first_bad(fullmove < 1, base)}: fullmove below 1")

    white_king = board == 6
    black_king = board == 12
    if np.any(white_king.sum(axis=1) != 1):
        raise ValueError(f"record {_first_bad(white_king.sum(axis=1) != 1, base)}: "
                         "expected exactly one white king")
    if np.any(black_king.sum(axis=1) != 1):
        raise ValueError(f"record {_first_bad(black_king.sum(axis=1) != 1, base)}: "
                         "expected exactly one black king")
    king_squares = (np.argmax(white_king, axis=1).astype(np.int32),
                    np.argmax(black_king, axis=1).astype(np.int32))

    # ---- encode spell-bin v1 --------------------------------------------
    plane = BitPlane(count)
    plane.bits[:, 0] = stm
    plane.put(king_squares[0], 7, 1)
    plane.put(king_squares[1], 7, 8)
    cursor = np.full(count, 15, dtype=np.int32)

    for square in SCAN:
        piece = board[:, square]
        skip = (king_squares[0] == square) | (king_squares[1] == square)
        live = (piece != 0) & ~skip
        if live.any():
            kind = (piece - 1) % 6  # P N B R Q K -> 0 .. 5, colour folded away
            if np.any(live & (kind > 4)):
                raise ValueError(f"record {_first_bad(live & (kind > 4), base)}: "
                                 "king outside its king-square field")
            # Huffman code of a piece is 2 * kind + 1, written LSB first, so
            # bit 0 is always set and bits 1 to 3 carry `kind`.
            plane.set_flag(live, cursor)
            plane.put(kind.astype(np.uint32), 3, cursor + 1, mask=live)
            plane.set_flag(live & (piece > 6), cursor + 5)
        cursor = cursor + np.where(skip, 0, np.where(live, 6, 1))

    # Hands: 2 colours x 8 slots x 5 bits, only slots 6 and 7 are ever set.
    plane.put(hands[0], 3, cursor + 30)
    plane.put(hands[1], 2, cursor + 35)
    plane.put(hands[2], 3, cursor + 70)
    plane.put(hands[3], 2, cursor + 75)
    cursor = cursor + 80

    # Potions: white freeze, white jump, black freeze, black jump.
    for index in range(4):
        block = cursor + 24 * index
        present = gates[index] >= 0
        plane.set_flag(present, block)
        plane.put(np.where(present, gates[index], 0).astype(np.uint32), 7,
                  block + 1, mask=present)
        plane.put(cooldowns[index], 16, block + 8)
    cursor = cursor + 96

    plane.put(castling, 4, cursor)
    cursor = cursor + 4

    has_ep = ep >= 0
    plane.set_flag(has_ep, cursor)
    plane.put(np.where(has_ep, ep, 0).astype(np.uint32), 7, cursor + 1, mask=has_ep)
    cursor = cursor + 1 + 7 * has_ep

    plane.put(rule50 & 63, 6, cursor)
    cursor = cursor + 6
    plane.put(fullmove, 16, cursor)
    cursor = cursor + 16
    plane.set_flag((rule50 >> 6) != 0, cursor)
    cursor = cursor + 1

    if np.any(cursor > SFEN_BITS):
        raise ValueError(f"record {_first_bad(cursor > SFEN_BITS, base)}: "
                         f"sfen needs {int(cursor.max())} bits")

    out = np.empty((count, PSV_RECORD), dtype=np.uint8)
    out[:, :64] = plane.pack()
    tail = np.zeros(count, dtype=TAIL_DTYPE)
    clamped = np.clip(score, -SCORE_CLAMP, SCORE_CLAMP)
    tail["score"] = clamped.astype(np.int16)
    tail["move"] = move
    tail["ply"] = ply.astype(np.uint16)
    tail["result"] = result.astype(np.int8)
    out[:, 64:] = tail.view(np.uint8).reshape(count, PSV_RECORD - 64)

    stats = {
        "records": count,
        "score_clamped": int(np.count_nonzero(clamped != score)),
        "live_freeze": int(np.count_nonzero(gates[0] >= 0) + np.count_nonzero(gates[2] >= 0)),
        "live_jump": int(np.count_nonzero(gates[1] >= 0) + np.count_nonzero(gates[3] >= 0)),
        "with_ep": int(np.count_nonzero(has_ep)),
        "with_castling": int(np.count_nonzero(castling)),
        "freeze_in_hand": int(hands[0].sum() + hands[2].sum()),
        "jump_in_hand": int(hands[1].sum() + hands[3].sum()),
        "result_win": int(np.count_nonzero(result > 0)),
        "result_draw": int(np.count_nonzero(result == 0)),
        "result_loss": int(np.count_nonzero(result < 0)),
        "stm_black": int(np.count_nonzero(stm)),
    }
    return out, stats


# --------------------------------------------------------------------------
# Multiprocess driver
# --------------------------------------------------------------------------

WORKER: dict = {}


def init_worker(source: str, target: str, nice: bool) -> None:
    if nice:
        lower_priority()
    WORKER["source"] = open(source, "rb", buffering=0)
    WORKER["target"] = open(target, "r+b", buffering=0)


def encode_and_write(raw: np.ndarray, start: int, depth: int = 0) -> dict:
    """Encode a block and write it in place, halving it if memory runs short.

    The host runs this beside other large jobs, so a transient MemoryError on a
    few megabytes is a queueing problem and not a data problem.  Back off, split
    the block, and only give up once the pieces are small.
    """
    try:
        out, stats = encode_block(raw, start)
    except MemoryError:
        if raw.shape[0] < 4096 or depth >= 4:
            raise
        time.sleep(2.0 * (depth + 1))
        half = raw.shape[0] // 2
        first = encode_and_write(raw[:half], start, depth + 1)
        second = encode_and_write(raw[half:], start + half, depth + 1)
        return {key: first[key] + second[key] for key in first}
    target = WORKER["target"]
    target.seek(start * PSV_RECORD)
    target.write(out)
    return stats


def run_task(task: tuple[int, int]) -> tuple[int, dict]:
    start, count = task
    source = WORKER["source"]
    source.seek(RUN7_HEADER + start * RUN7_RECORD)
    blob = source.read(count * RUN7_RECORD)
    if len(blob) != count * RUN7_RECORD:
        raise ValueError(f"truncated run7 payload at record {start}")
    raw = np.frombuffer(blob, dtype=np.uint8).reshape(count, RUN7_RECORD)
    return count, encode_and_write(raw, start)


def read_header(path: str) -> tuple[int, int, int]:
    with open(path, "rb") as handle:
        count, source_count, flags = run7.read_header(handle)
    size = os.path.getsize(path)
    expected = RUN7_HEADER + count * RUN7_RECORD
    if size != expected:
        raise SystemExit(f"{path}: header says {count} records ({expected} bytes) "
                         f"but the file is {size} bytes")
    return count, source_count, flags


def convert(args: argparse.Namespace, total: int) -> dict:
    with open(args.out, "wb") as handle:
        handle.truncate(total * PSV_RECORD)

    tasks = [(start, min(args.block, total - start))
             for start in range(0, total, args.block)]
    stats = {key: 0 for key in STAT_KEYS}
    done = 0
    started = time.time()
    last = started

    with Pool(args.jobs, initializer=init_worker,
              initargs=(args.source, args.out, not args.no_nice)) as pool:
        for count, chunk in pool.imap_unordered(run_task, tasks, chunksize=1):
            done += count
            for key, value in chunk.items():
                stats[key] += value
            now = time.time()
            if now - last >= 10.0 or done == total:
                rate = done / max(now - started, 1e-6)
                left = (total - done) / max(rate, 1e-6)
                print(f"  {done:,}/{total:,} records  {rate:,.0f} rec/s  "
                      f"eta {left / 60:.1f} min", flush=True)
                last = now

    stats["wall_seconds"] = time.time() - started
    return stats


def copy_prefix(source: str, target: str, records: int) -> None:
    chunk = 1 << 22
    remaining = records * PSV_RECORD
    with open(source, "rb") as reader, open(target, "wb") as writer:
        while remaining > 0:
            blob = reader.read(min(chunk, remaining))
            if not blob:
                raise SystemExit(f"{source}: fewer than {records} records")
            writer.write(blob)
            remaining -= len(blob)


# --------------------------------------------------------------------------
# Spot check: run7.py against tools/psv_decode.py, field by field
# --------------------------------------------------------------------------

def load_validator():
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.normpath(os.path.join(here, os.pardir, "psv_decode.py"))
    spec = importlib.util.spec_from_file_location("psv_decode", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module, path


def compare_record(source: run7.Record, decoded: dict) -> list[str]:
    """Return the list of fields that disagree between the two decoders."""
    bad = []
    if decoded["stm"] != source.stm:
        bad.append("stm")

    kings = {run7.W_KING: decoded["wk"], run7.B_KING: decoded["bk"]}
    expected_board = {}
    for square, piece in enumerate(source.board):
        if piece == 0:
            continue
        if piece in kings:
            if kings[piece] != square:
                bad.append("king square")
            continue
        expected_board[square] = (piece - 1 if piece < 8 else piece - 9,
                                  0 if piece < 8 else 1)
    if expected_board != decoded["board"]:
        bad.append("board")

    hands = decoded["hands"]
    if (hands[0][6], hands[0][7], hands[1][6], hands[1][7]) != source.hands:
        bad.append("hands")
    if any(hands[colour][slot] for colour in (0, 1) for slot in range(6)):
        bad.append("hand padding")

    for index in range(4):
        present, zone, cooldown = decoded["potions"][index]
        gate = source.gates[index]
        if bool(present) != (gate >= 0):
            bad.append(f"potion {index} present")
        if zone != (gate if gate >= 0 else None):
            bad.append(f"potion {index} zone")
        if cooldown != source.cooldowns[index]:
            bad.append(f"potion {index} cooldown")

    castling = sum(bool(value) << index for index, value in enumerate(decoded["castling"]))
    if castling != source.castling:
        bad.append("castling")
    if decoded["ep"] != (source.ep if source.ep >= 0 else None):
        bad.append("ep")
    if decoded["rule50"] != source.rule50:
        bad.append("rule50")
    if decoded["fullmove"] != source.fullmove:
        bad.append("fullmove")
    if decoded["score"] != max(-SCORE_CLAMP, min(SCORE_CLAMP, source.score)):
        bad.append("score")
    if decoded["move"] != source.move:
        bad.append("move")
    if decoded["ply"] != source.game_ply:
        bad.append("gamePly")
    if decoded["result"] != source.result:
        bad.append("gameResult")
    if decoded["bits_used"] > SFEN_BITS:
        bad.append("sfen overflow")
    return bad


def spot_check(args: argparse.Namespace, total: int) -> int:
    validator, path = load_validator()
    print(f"spot check: {args.spot_check:,} random records against {path}", flush=True)
    rng = random.Random(args.seed)
    sample = sorted(rng.sample(range(total), min(args.spot_check, total)))

    failures = 0
    started = time.time()
    with open(args.source, "rb") as source, open(args.out, "rb") as target:
        for done, index in enumerate(sample, 1):
            source.seek(RUN7_HEADER + index * RUN7_RECORD)
            record = run7.unpack(source.read(RUN7_RECORD))
            target.seek(index * PSV_RECORD)
            decoded = validator.decode(target.read(PSV_RECORD))

            bad = compare_record(record, decoded)
            if run7.to_fen(record) != validator.to_fen(decoded):
                bad.append("fen")
            if bad:
                failures += 1
                if failures <= 10:
                    print(f"  MISMATCH record {index}: {', '.join(bad)}")
                    print(f"    run7: {run7.to_fen(record)}")
                    print(f"    psv : {validator.to_fen(decoded)}")
            if done % 2000 == 0:
                print(f"  {done:,}/{len(sample):,} compared", flush=True)

    print(f"spot check: {len(sample):,} records, {failures} mismatches, "
          f"{time.time() - started:.1f} s", flush=True)
    return failures


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("source", help="run7 corpus to read")
    parser.add_argument("--out", required=True, help="spell-bin v1 file to write")
    parser.add_argument("--records", type=int, default=0,
                        help="convert only the first N records (0 = all)")
    parser.add_argument("--jobs", type=int, default=5, help="worker processes")
    parser.add_argument("--block", type=int, default=50000,
                        help="records per task")
    parser.add_argument("--no-nice", action="store_true",
                        help="keep normal process priority")
    parser.add_argument("--val-out", help="also write a validation file")
    parser.add_argument("--val-records", type=int, default=1000000,
                        help="records copied into --val-out")
    parser.add_argument("--slice-out", help="also write a small slice for psv_decode")
    parser.add_argument("--slice-records", type=int, default=100000,
                        help="records copied into --slice-out")
    parser.add_argument("--spot-check", type=int, default=0,
                        help="compare N random records field by field")
    parser.add_argument("--seed", type=int, default=20260816,
                        help="seed of the spot check sample")
    parser.add_argument("--check-only", action="store_true",
                        help="skip the conversion and only run the checks")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    count, source_count, flags = read_header(args.source)
    total = min(args.records, count) if args.records else count
    print(f"source {args.source}: {count:,} records "
          f"(source positions {source_count:,}, flags {flags})", flush=True)

    if not args.check_only:
        print(f"converting {total:,} records with {args.jobs} workers, "
              f"block {args.block:,}", flush=True)
        stats = convert(args, total)
        size = os.path.getsize(args.out)
        print(f"wrote {args.out}: {size:,} bytes", flush=True)
        if size != total * PSV_RECORD or stats["records"] != total:
            print(f"FAIL: expected {total * PSV_RECORD:,} bytes and {total:,} "
                  f"records, got {size:,} bytes and {stats['records']:,} records")
            return 1
        wall = stats.pop("wall_seconds")
        print(f"wall {wall / 60:.1f} min, {total / max(wall, 1e-6):,.0f} rec/s")
        for key in STAT_KEYS:
            print(f"  {key}: {stats[key]:,}")

    if args.slice_out:
        copy_prefix(args.out, args.slice_out, args.slice_records)
        print(f"wrote {args.slice_out}: {args.slice_records:,} records", flush=True)
    if args.val_out:
        copy_prefix(args.out, args.val_out, args.val_records)
        print(f"wrote {args.val_out}: {args.val_records:,} records", flush=True)

    if args.spot_check:
        if spot_check(args, total):
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
