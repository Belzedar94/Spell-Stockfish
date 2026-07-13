#!/usr/bin/env python3
"""Decoder/validator for the spell-chess PackedSfenValue training format.

Byte contract (verified against the reference tools binary, DATA_SIZE=512):
  sfen: 64 bytes, LSB-first bitstream:
    1  side to move
    7  white king square, 7 black king square
    board scan rank 8->1, file a->h, skipping kings:
       '0' = empty | 5-bit code (LSB=1, piece = (code+1)/2 - 1 in variant
       order P N B R Q F J) + 1 color bit (0=white)
    hands: for color in (W, B): 8 x 5-bit counts in FSF piece-id order
       (P N B R Q K F J) — F (idx 6) and J (idx 7) carry the spells
    potions: for color in (W, B): for potion in (FREEZE, JUMP):
       1 has_zone + 7 zone-center square + 16 cooldown
    4  castling KQkq
    1  has-ep [+ 7 ep square]
    6  rule50 low, 8 fullmove low, 8 fullmove high, 1 rule50 high
  tail: score i16, move u16, gamePly u16, result i8, padding u8 (LE)

Usage: python psv_decode.py FILE.bin [-n N] [--quiet]
Exits nonzero if any record fails structural validation.
"""

import argparse
import struct
import sys

PIECES = "PNBRQKFJ"  # hand-slot order (FSF ids)
RECORD = 76  # DATA_SIZE/8 + 12


class Bits:
    def __init__(self, data):
        self.d = data
        self.c = 0

    def bit(self):
        b = (self.d[self.c >> 3] >> (self.c & 7)) & 1
        self.c += 1
        return b

    def bits(self, n):
        v = 0
        for i in range(n):
            v |= self.bit() << i
        return v


def sq_name(s):
    return chr(ord("a") + s % 8) + str(s // 8 + 1)


def decode(rec):
    s = Bits(rec[:64])
    stm = s.bit()
    wk, bk = s.bits(7), s.bits(7)

    board = {}  # square -> (pieceIdx 0..6, color)
    for r in range(7, -1, -1):
        for f in range(8):
            sq = r * 8 + f
            if sq in (wk, bk):
                continue
            if s.bit() == 0:
                continue
            code = 1
            for i in range(1, 5):
                code |= s.bit() << i
            pr = (code + 1) // 2  # 1-based variant piece index
            color = s.bit()
            board[sq] = (pr - 1, color)

    hands = [[s.bits(5) for _ in range(8)] for _ in range(2)]

    potions = []
    for c in range(2):
        for pt in range(2):
            has = s.bit()
            zone = s.bits(7)
            cd = s.bits(16)
            potions.append((has, zone if has else None, cd))

    castling = [s.bit() for _ in range(4)]
    ep = s.bits(7) if s.bit() else None
    rule50 = s.bits(6)
    fullmove = s.bits(8) | (s.bits(8) << 8)
    rule50 |= s.bit() << 6

    # score i16 @64, 2 alignment bytes, move u32 @68, gamePly u16 @72,
    # result i8 @74, padding u8 @75 (the reference PackedSfenValue layout)
    score, move, ply, result, _pad = struct.unpack("<h2xIHbB", rec[64:76])
    return dict(stm=stm, wk=wk, bk=bk, board=board, hands=hands,
                potions=potions, castling=castling, ep=ep, rule50=rule50,
                fullmove=fullmove, bits_used=s.c,
                score=score, move=move, ply=ply, result=result)


def to_fen(d):
    rows = []
    for r in range(7, -1, -1):
        row, run = "", 0
        for f in range(8):
            sq = r * 8 + f
            pc = None
            if sq == d["wk"]:
                pc = "K"
            elif sq == d["bk"]:
                pc = "k"
            elif sq in d["board"]:
                idx, c = d["board"][sq]
                pc = PIECES[idx] if c == 0 else PIECES[idx].lower()
            if pc is None:
                run += 1
            else:
                row += (str(run) if run else "") + pc
                run = 0
        rows.append(row + (str(run) if run else ""))
    board = "/".join(rows)

    hold = ""
    for c, tr in ((0, str.upper), (1, str.lower)):
        for idx in (7, 6):  # J first, then F (reference holdings order)
            hold += tr(PIECES[idx]) * d["hands"][c][idx]

    st = []
    for i, (label, c) in enumerate((("F", 0), ("J", 0), ("f", 1), ("j", 1))):
        has, zone, cd = d["potions"][c * 2 + (0 if label.upper() == "F" else 1)]
        st.append(f"{label}@{sq_name(zone) if zone is not None else '-'}:{cd}")

    cast = "".join(x for x, b in zip("KQkq", d["castling"]) if b) or "-"
    epf = sq_name(d["ep"]) if d["ep"] is not None else "-"
    return (f"{board}[{hold}] {{{','.join(st)}}} {'b' if d['stm'] else 'w'} "
            f"{cast} {epf} {d['rule50']} {d['fullmove']}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("-n", type=int, default=5)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    data = open(args.file, "rb").read()
    if len(data) % RECORD:
        print(f"FAIL: size {len(data)} not a multiple of {RECORD}")
        sys.exit(1)
    n = len(data) // RECORD
    bad = 0
    for i in range(n):
        d = decode(data[i * RECORD:(i + 1) * RECORD])
        ok = (d["bits_used"] <= 512 and d["wk"] < 64 and d["bk"] < 64
              and all(h <= 31 for hs in d["hands"] for h in hs)
              and d["fullmove"] >= 1)
        bad += not ok
        if not args.quiet and (i < args.n or not ok):
            print(f"[{i}] {'OK ' if ok else 'BAD'} bits={d['bits_used']} "
                  f"score={d['score']} ply={d['ply']} res={d['result']} "
                  f"move=0x{d['move']:08x}")
            print(f"    {to_fen(d)}")
    print(f"\n{n} records, {bad} structurally bad")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
