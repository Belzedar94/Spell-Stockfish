#!/usr/bin/env python3
"""Compact fixed-size run7 records for Spell-NNUE v2.

The 44-byte payload keeps the complete extended-FEN state plus the training
target carried by the old PackedSfenValue (PSV) records.  Files start with a
32-byte header so they can be memory-mapped safely:

  header  <4sHHQQQ>  magic, version, record_size, count, source_count, flags
  record  <Q16s20s>  occupancy, packed piece nibbles, packed metadata bits

Occupied squares are enumerated least-significant-square first.  Each piece
uses a nibble: white P..K = 1..6, black P..K = 7..12.  Metadata is LSB-first:

  stm:1 castling:4 ep(+1):7 rule50:7 fullmove:16
  hands(WF,WJ,BF,BJ):3,2,3,2
  cooldowns(WF,WJ,BF,BJ):2 each
  gates(+1, same order):7 each
  score:i16 move:u32 game_ply:u16 result(+1):2 reserved:13

The record is deliberately independent of python-chess and of the engine.
"""

from __future__ import annotations

import dataclasses
import os
import struct
from collections.abc import Iterator

MAGIC = b"RUN7"
VERSION = 1
HEADER = struct.Struct("<4sHHQQQ")
RECORD = struct.Struct("<Q16s20s")
HEADER_SIZE = HEADER.size
RECORD_SIZE = RECORD.size

# Stockfish Piece enum values used by the feature extractor.
W_PAWN, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING = range(1, 7)
B_PAWN, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING = range(9, 15)
PIECE_TO_NIBBLE = {
    **{p: p for p in range(1, 7)},
    **{p: p - 2 for p in range(9, 15)},
}
NIBBLE_TO_PIECE = {v: k for k, v in PIECE_TO_NIBBLE.items()}


@dataclasses.dataclass(slots=True)
class Record:
    """Decoded run7 position and target.

    ``board`` is a 64-element tuple of Stockfish Piece enum values (zero for
    empty).  Hands, cooldowns, and gates use order WF, WJ, BF, BJ; a missing
    gate is -1.  ``stm`` is 0 for White and 1 for Black.
    """

    board: tuple[int, ...]
    stm: int
    castling: int
    ep: int
    rule50: int
    fullmove: int
    hands: tuple[int, int, int, int]
    cooldowns: tuple[int, int, int, int]
    gates: tuple[int, int, int, int]
    score: int
    move: int
    game_ply: int
    result: int

    def __post_init__(self) -> None:
        if len(self.board) != 64:
            raise ValueError("board must contain 64 squares")
        if sum(p != 0 for p in self.board) > 32:
            raise ValueError("run7 supports at most 32 occupied squares")
        if self.stm not in (0, 1) or self.result not in (-1, 0, 1):
            raise ValueError("invalid stm/result")
        if not (0 <= self.castling < 16 and -1 <= self.ep < 64):
            raise ValueError("invalid castling/ep")
        if not (0 <= self.rule50 < 128 and 0 <= self.fullmove < 65536):
            raise ValueError("rule50/fullmove out of range")
        if not (0 <= self.game_ply < 65536 and 0 <= self.move < (1 << 32)):
            raise ValueError("game_ply/move out of range")
        if any(not 0 <= x <= lim for x, lim in zip(self.hands, (5, 2, 5, 2))):
            raise ValueError("spell hand out of range")
        if any(not 0 <= x <= 3 for x in self.cooldowns):
            raise ValueError("spell cooldown out of range")
        if any(not -1 <= x < 64 for x in self.gates):
            raise ValueError("spell gate out of range")


class _BitWriter:
    def __init__(self) -> None:
        self.value = 0
        self.cursor = 0

    def put(self, value: int, bits: int) -> None:
        if value < 0 or value >= (1 << bits):
            raise ValueError(f"{value} does not fit in {bits} bits")
        self.value |= value << self.cursor
        self.cursor += bits

    def signed(self, value: int, bits: int) -> None:
        self.put(value & ((1 << bits) - 1), bits)


class _BitReader:
    def __init__(self, data: bytes) -> None:
        self.value = int.from_bytes(data, "little")
        self.cursor = 0

    def get(self, bits: int) -> int:
        value = (self.value >> self.cursor) & ((1 << bits) - 1)
        self.cursor += bits
        return value

    def signed(self, bits: int) -> int:
        value = self.get(bits)
        return value - (1 << bits) if value & (1 << (bits - 1)) else value


def pack(record: Record) -> bytes:
    occupancy = 0
    nibbles = bytearray(16)
    occupied = 0
    for square, piece in enumerate(record.board):
        if not piece:
            continue
        try:
            nibble = PIECE_TO_NIBBLE[piece]
        except KeyError as exc:
            raise ValueError(f"invalid board piece {piece}") from exc
        occupancy |= 1 << square
        nibbles[occupied >> 1] |= nibble << (4 * (occupied & 1))
        occupied += 1

    b = _BitWriter()
    b.put(record.stm, 1)
    b.put(record.castling, 4)
    b.put(record.ep + 1, 7)
    b.put(record.rule50, 7)
    b.put(record.fullmove, 16)
    for value, bits in zip(record.hands, (3, 2, 3, 2)):
        b.put(value, bits)
    for value in record.cooldowns:
        b.put(value, 2)
    for value in record.gates:
        b.put(value + 1, 7)
    b.signed(record.score, 16)
    b.put(record.move, 32)
    b.put(record.game_ply, 16)
    b.put(record.result + 1, 2)
    assert b.cursor == 147
    return RECORD.pack(occupancy, bytes(nibbles), b.value.to_bytes(20, "little"))


def unpack(data: bytes | memoryview) -> Record:
    if len(data) != RECORD_SIZE:
        raise ValueError(f"run7 record must be {RECORD_SIZE} bytes")
    occupancy, packed_pieces, metadata = RECORD.unpack(data)
    board = [0] * 64
    piece_index = 0
    bb = occupancy
    while bb:
        lsb = bb & -bb
        square = lsb.bit_length() - 1
        nibble = (packed_pieces[piece_index >> 1] >> (4 * (piece_index & 1))) & 15
        try:
            board[square] = NIBBLE_TO_PIECE[nibble]
        except KeyError as exc:
            raise ValueError(f"invalid run7 piece nibble {nibble}") from exc
        piece_index += 1
        bb ^= lsb
    if piece_index < 32:
        tail = int.from_bytes(packed_pieces, "little") >> (piece_index * 4)
        if tail:
            raise ValueError("non-zero run7 piece padding")

    b = _BitReader(metadata)
    stm = b.get(1)
    castling = b.get(4)
    ep = b.get(7) - 1
    rule50 = b.get(7)
    fullmove = b.get(16)
    hands = tuple(b.get(bits) for bits in (3, 2, 3, 2))
    cooldowns = tuple(b.get(2) for _ in range(4))
    gates = tuple(b.get(7) - 1 for _ in range(4))
    score = b.signed(16)
    move = b.get(32)
    game_ply = b.get(16)
    result = b.get(2) - 1
    if int.from_bytes(metadata, "little") >> 147:
        raise ValueError("non-zero run7 metadata padding")
    return Record(tuple(board), stm, castling, ep, rule50, fullmove,
                  hands, cooldowns, gates, score, move, game_ply, result)


def write_header(file, count: int, source_count: int = 0, flags: int = 0) -> None:
    file.write(HEADER.pack(MAGIC, VERSION, RECORD_SIZE, count, source_count, flags))


def read_header(file) -> tuple[int, int, int]:
    raw = file.read(HEADER_SIZE)
    if len(raw) != HEADER_SIZE:
        raise ValueError("truncated run7 header")
    magic, version, size, count, source_count, flags = HEADER.unpack(raw)
    if magic != MAGIC or version != VERSION or size != RECORD_SIZE:
        raise ValueError("unsupported run7 file")
    return count, source_count, flags


def iter_records(path: os.PathLike[str] | str, limit: int | None = None) -> Iterator[Record]:
    with open(path, "rb") as file:
        count, _, _ = read_header(file)
        if limit is not None:
            count = min(count, limit)
        for _ in range(count):
            raw = file.read(RECORD_SIZE)
            if len(raw) != RECORD_SIZE:
                raise ValueError("truncated run7 payload")
            yield unpack(raw)


def square_name(square: int) -> str:
    return chr(ord("a") + square % 8) + str(square // 8 + 1)


def to_fen(record: Record) -> str:
    chars = " PNBRQK  pnbrqk"
    rows = []
    for rank in range(7, -1, -1):
        row = ""
        empty = 0
        for file in range(8):
            piece = record.board[rank * 8 + file]
            if piece == 0:
                empty += 1
            else:
                if empty:
                    row += str(empty)
                    empty = 0
                row += chars[piece]
        rows.append(row + (str(empty) if empty else ""))

    wf, wj, bf, bj = record.hands
    holdings = "J" * wj + "F" * wf + "j" * bj + "f" * bf
    labels = ("F", "J", "f", "j")
    state = ",".join(
        f"{label}@{square_name(gate) if gate >= 0 else '-'}:{cooldown}"
        for label, gate, cooldown in zip(labels, record.gates, record.cooldowns)
    )
    castling = "".join(c for bit, c in enumerate("KQkq") if record.castling & (1 << bit)) or "-"
    ep = square_name(record.ep) if record.ep >= 0 else "-"
    return (f"{'/'.join(rows)}[{holdings}] {{{state}}} "
            f"{'b' if record.stm else 'w'} {castling} {ep} {record.rule50} {record.fullmove}")


def from_psv(decoded: dict) -> Record:
    board = [0] * 64
    if decoded["wk"] < 64:
        board[decoded["wk"]] = W_KING
    if decoded["bk"] < 64:
        board[decoded["bk"]] = B_KING
    for square, (piece_index, color) in decoded["board"].items():
        if piece_index >= 6:
            raise ValueError("PSV board contains a spell pseudo-piece")
        board[square] = piece_index + 1 + (8 if color else 0)

    hands = (
        decoded["hands"][0][6], decoded["hands"][0][7],
        decoded["hands"][1][6], decoded["hands"][1][7],
    )
    cooldowns = tuple(decoded["potions"][i][2] for i in range(4))
    gates = tuple(-1 if decoded["potions"][i][1] is None else decoded["potions"][i][1]
                  for i in range(4))
    castling = sum(bool(v) << i for i, v in enumerate(decoded["castling"]))
    return Record(tuple(board), decoded["stm"], castling,
                  -1 if decoded["ep"] is None else decoded["ep"],
                  decoded["rule50"], decoded["fullmove"], hands,
                  cooldowns, gates, decoded["score"], decoded["move"],
                  decoded["ply"], decoded["result"])


def self_test() -> None:
    board = [0] * 64
    board[4], board[60], board[0], board[63] = W_KING, B_KING, W_ROOK, B_ROOK
    original = Record(tuple(board), 1, 9, 20, 73, 412, (5, 2, 4, 1),
                      (3, 0, 2, 1), (27, -1, 36, -1), -321, 0xDEADBEEF,
                      65530, -1)
    assert unpack(pack(original)) == original


if __name__ == "__main__":
    self_test()
    print(f"run7 self-test OK (header={HEADER_SIZE}, record={RECORD_SIZE})")
