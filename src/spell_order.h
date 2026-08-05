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

#ifndef SPELL_ORDER_H_INCLUDED
#define SPELL_ORDER_H_INCLUDED

#include "attacks.h"
#include "position.h"
#include "spell.h"
#include "spell_params.h"
#include "types.h"

namespace Stockfish {

// Gate impact heuristics, shared by movegen's QUIETS gate limiting and
// MovePicker's ordering of gated moves. A freeze gate is scored by the enemy
// material its zone would silence plus bonuses for reaching the enemy king
// and its ring; a jump gate by the material and king attacks its lifted
// blocker would reveal to our sliders.

// Coarse context of the learned gate picker (SpellGateHistory in history.h).
// The phase half is fixed per node, the quadrant half depends on the gate,
// so the node computes the context once and applies it per gate square.
struct GateContext {
    Square eksq;
    int    phase;  // 0 = opening, 1 = middlegame, 2 = endgame

    int operator()(Square g) const {
        if (eksq == SQ_NONE)
            return phase;
        return (2 * (rank_of(g) >= rank_of(eksq)) + (file_of(g) >= file_of(eksq))) * 3 + phase;
    }
};

// Phase cuts on total non-pawn material (16604 at the start position):
// roughly "both queens still around", "queens off / heavy pieces left",
// "light endgame".
inline GateContext gate_context(const Position& pos, Color us) {

    const Value  npm   = pos.non_pawn_material();
    const int    phase = npm > 11000 ? 0 : npm > 5000 ? 1 : 2;
    const Square eksq  = pos.count<KING>(~us) ? pos.square<KING>(~us) : SQ_NONE;

    return {eksq, phase};
}

// Per-node gate budget: the global cap modulated by remaining depth and by
// urgency. With both slopes at 0 this returns the cap unchanged, which is
// exactly what the engine did when 12/6 (today 8/4) were constants.
inline int gate_budget_for(int base, Depth d, bool urgent) {

    int k = base;
    if (SpellGateDepthSlope)
        k += SpellGateDepthSlope * std::clamp(int(d) - SpellGateDepthPivot, -8, 8) / 8;
    if (urgent)
        k += SpellGateUrgencyBonus;
    return std::max(1, k);
}

// Score of freezing with the zone centered on g. eksq/eRing are the enemy
// king square (or SQ_NONE) and king ring, precomputed by the caller.
inline int freeze_gate_score(const Position& pos, Color us, Square g, Square eksq, Bitboard eRing) {

    const Bitboard zone = FreezeZoneBB[g];

    int s = 0;
    for (Bitboard t = zone & pos.pieces(~us); t;)
        s += PieceValue[pos.piece_on(pop_lsb(t))];
    if (eksq != SQ_NONE && (zone & square_bb(eksq)))
        s += SpellGateKingBonus;
    if (zone & eRing)
        s += SpellGateKingRingBonus;
    return s;
}

// Search-policy filter (reference: is_useless_potion, applied when the
// MovePicker emits a move — the legal universe is untouched): a freeze
// whose zone contains no enemy piece wastes the spell, and a jump gated on
// a square that is not strictly on the base move's path does nothing for
// that move. This kills the vast majority of the gated universe: jump
// copies survive only when the jump actually enables the move.
inline bool is_useless_spell(const Position& pos, Move m) {

    if (!m.is_spell())
        return false;

    if (m.spell_type() == SPELL_FREEZE)
        return !(FreezeZoneBB[m.gate_sq()] & pos.pieces(~pos.side_to_move()));

    const Bitboard path = Attacks::between_bb(m.from_sq(), m.to_sq()) & ~square_bb(m.to_sq());
    return !(path & square_bb(m.gate_sq()));
}

// A freeze cast is "tactical" (reference policy: treated like a capture or
// check throughout pruning, reductions and extensions) when its zone
// touches the enemy king, silences an attacker of our own king (defensive
// freeze), or freezes an enemy piece that is major-valued, attacked by us,
// or attacking our king. ourRoyalAttackers/enemyRoyal/ourRoyal are
// precomputed once per node.
inline bool is_tactical_spell(
  const Position& pos, Move m, Bitboard ourRoyalAttackers, Square enemyRoyal, Square ourRoyal) {

    if (!m.is_spell() || m.spell_type() != SPELL_FREEZE)
        return false;

    const Color    us   = pos.side_to_move();
    const Color    them = ~us;
    const Bitboard zone = FreezeZoneBB[m.gate_sq()];

    if (enemyRoyal != SQ_NONE && (zone & square_bb(enemyRoyal)))
        return true;
    if (ourRoyalAttackers & zone)
        return true;

    Bitboard candidates = zone & pos.pieces(them);
    if (!candidates)
        return false;

    const Bitboard occ = pos.pieces();
    while (candidates)
    {
        const Square    s  = pop_lsb(candidates);
        const Piece     pc = pos.piece_on(s);
        const PieceType pt = type_of(pc);

        // Freezing an attacked or major enemy piece is a tactical motif
        if (PieceValue[pc] >= RookValue)
            return true;
        if (pos.attackers_to(s) & pos.pieces(us))
            return true;
        if (ourRoyal != SQ_NONE)
        {
            const Bitboard att =
              pt == PAWN ? Attacks::attacks_bb<PAWN>(s, them) : Attacks::attacks_bb(pt, s, occ);
            if (att & square_bb(ourRoyal))
                return true;
        }
    }
    return false;
}

// Fills out[64] with the reveal value of lifting each blocker for our
// sliders: enemy material newly attacked, plus the king bonus if the enemy
// king becomes attacked. Non-blocker squares score 0.
inline void jump_gate_scores(const Position& pos, Color us, Square eksq, int out[SQUARE_NB]) {

    std::fill_n(out, SQUARE_NB, 0);

    const Bitboard occupied   = pos.pieces();
    const Bitboard occSliding = pos.occupied_for_sliding();

    Bitboard sliders = pos.pieces(us, BISHOP, ROOK, QUEEN) & ~pos.frozen_squares(us);
    while (sliders)
    {
        const Square    from = pop_lsb(sliders);
        const PieceType pt   = type_of(pos.piece_on(from));
        const Bitboard  seen = Attacks::attacks_bb(pt, from, occSliding);

        Bitboard blockers = seen & occupied;
        while (blockers)
        {
            const Square   b = pop_lsb(blockers);
            const Bitboard reveal =
              Attacks::attacks_bb(pt, from, occSliding ^ square_bb(b)) & ~seen;

            int s = 0;
            for (Bitboard t = reveal & pos.pieces(~us); t;)
                s += PieceValue[pos.piece_on(pop_lsb(t))];
            if (eksq != SQ_NONE && (reveal & square_bb(eksq)))
                s += SpellGateKingBonus;
            out[b] += s;
        }
    }
}

}  // namespace Stockfish

#endif  // #ifndef SPELL_ORDER_H_INCLUDED
