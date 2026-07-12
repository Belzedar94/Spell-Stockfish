/*
  Spell-Stockfish, a Spell Chess engine derived from Stockfish
  Copyright (C) 2004-2026 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "datagen.h"

#include <cstring>

#include "position.h"
#include "spell.h"

namespace Stockfish::Datagen {

namespace {

// LSB-first bitstream over the 64-byte sfen buffer (reference layout)
struct BitWriter {
    u8* d;
    int c = 0;

    void bit(int b) {
        if (b)
            d[c >> 3] |= u8(1u << (c & 7));
        ++c;
    }
    void bits(int v, int n) {
        for (int i = 0; i < n; ++i)
            bit((v >> i) & 1);
    }
};

// Variant piece index for the huffman code: P N B R Q -> 0..4 (F/J never
// stand on the board, the king is written separately)
constexpr int pieceIdx[PIECE_TYPE_NB] = {-1, 0, 1, 2, 3, 4, -1, -1};

}  // namespace

void pack_sfen(const Position& pos, u8 out[64]) {

    std::memset(out, 0, 64);
    BitWriter s{out};

    // Side to move
    s.bit(pos.side_to_move() == BLACK);

    // Kings (7 bits each); a captured king writes the reference's
    // out-of-board sentinel (files * ranks = 64)
    for (Color c : {WHITE, BLACK})
        s.bits(pos.count<KING>(c) ? int(pos.square<KING>(c)) : 64, 7);

    // Board scan rank 8 -> 1, file a -> h, skipping kings: '0' for empty,
    // else 5-bit huffman code (LSB=1, code = 2*idx+1) + 1 color bit
    for (int r = 7; r >= 0; --r)
        for (int f = 0; f < 8; ++f)
        {
            const Square sq = make_square(File(f), Rank(r));
            const Piece  pc = pos.piece_on(sq);
            if (pc != NO_PIECE && type_of(pc) == KING)
                continue;
            if (pc == NO_PIECE)
            {
                s.bit(0);
                continue;
            }
            s.bits(2 * pieceIdx[type_of(pc)] + 1, 5);
            s.bit(color_of(pc) == BLACK);
        }

    // Hands: 8 x 5-bit counts per color in FSF piece-id order
    // (P N B R Q K F J) — only the spell slots are ever nonzero
    for (Color c : {WHITE, BLACK})
        for (int slot = 0; slot < 8; ++slot)
        {
            int cnt = slot == 6 ? pos.spells_in_hand(c, SPELL_FREEZE)
                    : slot == 7 ? pos.spells_in_hand(c, SPELL_JUMP)
                                : 0;
            s.bits(cnt, 5);
        }

    // Potion blocks: [has_zone 1][zone center 7][cooldown 16] per
    // color x {FREEZE, JUMP}. Our gate IS the zone center (any center
    // producing the same zone is feature-equivalent for the trainer).
    for (Color c : {WHITE, BLACK})
        for (int sp = 0; sp < SPELL_NB; ++sp)
        {
            const Bitboard zone = pos.spell_zone(c, SpellType(sp));
            s.bit(zone != 0);
            s.bits(zone ? int(pos.spell_gate(c, SpellType(sp))) : 0, 7);
            s.bits(pos.spell_cooldown(c, SpellType(sp)), 16);
        }

    // Castling rights KQkq
    s.bit(pos.can_castle(WHITE_OO));
    s.bit(pos.can_castle(WHITE_OOO));
    s.bit(pos.can_castle(BLACK_OO));
    s.bit(pos.can_castle(BLACK_OOO));

    // En passant
    if (pos.ep_square() == SQ_NONE)
        s.bit(0);
    else
    {
        s.bit(1);
        s.bits(int(pos.ep_square()), 7);
    }

    // Counters: rule50 low 6, fullmove low/high 8+8, rule50 high bit
    const int r50 = pos.rule50_count();
    const int fm  = 1 + (pos.game_ply() - (pos.side_to_move() == BLACK)) / 2;
    s.bits(r50 & 0x3F, 6);
    s.bits(fm & 0xFF, 8);
    s.bits((fm >> 8) & 0xFF, 8);
    s.bit((r50 >> 6) & 1);
}

}  // namespace Stockfish::Datagen
