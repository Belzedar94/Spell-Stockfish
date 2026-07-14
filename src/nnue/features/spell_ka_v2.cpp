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

// Definition of input features SpellKAv2 of the Spell-NNUE v2 evaluation

#include "spell_ka_v2.h"

#include "../../bitboard.h"
#include "../../position.h"
#include "../../types.h"

namespace Stockfish::Eval::NNUE::Features {

// Sanity: the raw king-bucket table must be the HalfKA one divided by PS_NB
static_assert([] {
    for (int s = 0; s < SQUARE_NB; ++s)
        if (SpellKAv2::KingBucketsRaw[s] * 704 != Features::HalfKAv2_hm::KingBuckets[s])
            return false;
    return true;
}());

IndexType
SpellKAv2::make_spell_index(Color perspective, Square ksq, const DirtySpellEvent& ev) {
    switch (ev.block())
    {
    case DirtySpellEvent::FREEZE_GATE :
        return make_freeze_index(perspective, ev.color(), Square(ev.index()), ksq);
    case DirtySpellEvent::JUMP_GATE :
        return make_jump_index(perspective, ev.color(), Square(ev.index()));
    case DirtySpellEvent::FROZEN :
        return make_frozen_index(perspective, ev.color(), Square(ev.index()));
    default :
        return make_global_index(perspective, ev.color(), ev.index());
    }
}

void SpellKAv2::append_active_spell(Color           perspective,
                                    Square          ksq,
                                    const Position& pos,
                                    IndexList&      active) {
    for (Color c : {WHITE, BLACK})
    {
        // Live zone gates
        const Square gF = pos.spell_gate(c, SPELL_FREEZE);
        if (gF != SQ_NONE)
            active.push_back(make_freeze_index(perspective, c, gF, ksq));

        const Square gJ = pos.spell_gate(c, SPELL_JUMP);
        if (gJ != SQ_NONE)
            active.push_back(make_jump_index(perspective, c, gJ));

        // Frozen pieces of color c (inside the live freeze zone of ~c)
        Bitboard frozen = pos.pieces(c) & pos.frozen_squares(c);
        while (frozen)
            active.push_back(make_frozen_index(perspective, c, pop_lsb(frozen)));

        // Globals: hand/cooldown thermometers and ready bits
        for (SpellType sp : {SPELL_FREEZE, SPELL_JUMP})
        {
            const int hand = pos.spells_in_hand(c, sp);
            const int cd   = pos.spell_cooldown(c, sp);

            for (int k = 0; k < hand; ++k)
                active.push_back(make_global_index(perspective, c, slot_hand(sp) + k));
            for (int k = 0; k < cd; ++k)
                active.push_back(make_global_index(perspective, c, slot_cd(sp) + k));
            if (hand > 0 && cd == 0)
                active.push_back(make_global_index(perspective, c, slot_ready(sp)));
        }
    }
}

void SpellKAv2::append_changed_indices(Color             perspective,
                                       Square            ksq,
                                       const DirtyPiece& dp,
                                       const DirtySpell& ds,
                                       IndexList&        removed,
                                       IndexList&        added) {
    // Piece delta, exactly like HalfKAv2_hm (absent for null-move states)
    if (!ds.isNull)
    {
        removed.push_back(make_index(perspective, dp.from, dp.pc, ksq));
        if (dp.to != SQ_NONE)
            added.push_back(make_index(perspective, dp.to, dp.pc, ksq));

        if (dp.remove_sq != SQ_NONE)
            removed.push_back(make_index(perspective, dp.remove_sq, dp.remove_pc, ksq));

        if (dp.add_sq != SQ_NONE)
            added.push_back(make_index(perspective, dp.add_sq, dp.add_pc, ksq));
    }

    // Spell events (one feature flip each)
    for (const auto& ev : ds.list)
        (ev.add() ? added : removed).push_back(make_spell_index(perspective, ksq, ev));
}

}  // namespace Stockfish::Eval::NNUE::Features
