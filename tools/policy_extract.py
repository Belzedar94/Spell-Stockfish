"""Big bet 2 (docs/big-bets.md): policy dataset extraction from PSV data.

Reads spell-bin v1 records (76 bytes, docs/spell-bin-v1.md) and emits the
CAST-POLICY labels hiding in the `move` field (our 32-bit encoding): did the
PV move cast, which spell, which gate — plus cheap context features. No
training here: this materializes the dataset and its statistics so the tiny
policy head can be trained the moment the owner green-lights it.

Usage: python tools/policy_extract.py <in.bin> [out.csv] [--stats-only]
"""
import struct
import sys

REC = 76

def sq_name(s):
    return chr(ord('a') + (s & 7)) + chr(ord('1') + (s >> 3))

def main():
    src = sys.argv[1]
    out = None
    stats_only = "--stats-only" in sys.argv
    if len(sys.argv) > 2 and not sys.argv[2].startswith("--"):
        out = open(sys.argv[2], "w", encoding="ascii")
        out.write("stm,cast,spell,gate,from,to,score,result,ply,"
                  "hand_f_us,hand_j_us,hand_f_them,hand_j_them\n")

    n = casts = freezes = jumps = 0
    gate_hist = [0] * 64
    by_phase = [0, 0, 0]  # opening (<20), middle (<60), late
    casts_by_phase = [0, 0, 0]

    with open(src, "rb") as f:
        while True:
            rec = f.read(REC)
            if len(rec) < REC:
                break
            n += 1
            sfen = rec[:64]
            score, _pad, move, ply, result, _p2 = struct.unpack("<hHIHbB", rec[64:])

            stm = sfen[0] & 1  # bit 0: side to move (LSB-first stream)
            spell_bits = (move >> 16) & 3
            cast = spell_bits != 0
            gate = (move >> 18) & 0x3F
            frm, to = (move >> 6) & 0x3F, move & 0x3F

            # hands live at bit offset 1+7+7+board; decoding the variable-
            # length board huffman is needed for exact offsets — for the
            # dataset we re-derive hands cheaply during full decode later;
            # the CSV keeps placeholders unless psv_decode is imported.
            ph = 0 if ply < 20 else (1 if ply < 60 else 2)
            by_phase[ph] += 1
            if cast:
                casts += 1
                casts_by_phase[ph] += 1
                gate_hist[gate] += 1
                if spell_bits == 1:
                    freezes += 1
                else:
                    jumps += 1

            if out and not stats_only:
                out.write(f"{stm},{int(cast)},{spell_bits},{sq_name(gate) if cast else '-'},"
                          f"{sq_name(frm)},{sq_name(to)},{score},{result},{ply},.,.,.,.\n")

    print(f"registros: {n:,}")
    print(f"PV castea: {casts:,} ({100.0 * casts / max(n, 1):.2f}%)  "
          f"freeze {freezes:,} / jump {jumps:,}")
    for i, tag in enumerate(("apertura <20", "medio <60", "final")):
        c, t = casts_by_phase[i], by_phase[i]
        print(f"  {tag:14} {t:>9,} pos, castea {100.0 * c / max(t, 1):5.2f}%")
    top = sorted(range(64), key=lambda g: -gate_hist[g])[:8]
    print("gates top:", ", ".join(f"{sq_name(g)}:{gate_hist[g]:,}" for g in top if gate_hist[g]))
    if out:
        out.close()

if __name__ == "__main__":
    main()
