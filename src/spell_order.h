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


// ---------------------------------------------------------------------------
// Spell-SEE (big bet 1): static exchange value FOR CASTS.
//
// spell_swap: classic SEE swap-off of 'us' capturing on 'to', except enemy
// attackers under 'frozenEnemy' give no recaptures (frozen pieces do not
// attack). Legality niceties are ignored exactly like classic SEE — and in
// spell chess self-check is legal anyway, so this is if anything MORE exact
// here than in chess. Attackers are recomputed per iteration (magic lookups),
// which handles x-rays for free.
inline int spell_swap(const Position& pos, Color us, Square to, Bitboard frozenEnemy) {

    int   gain[24];
    int   d   = 0;
    Color stm = us;

    Bitboard occ = pos.pieces();
    gain[0]      = PieceValue[pos.piece_on(to)];

    while (true)
    {
        Bitboard atts = pos.attackers_to(to, occ) & occ & pos.pieces(stm);
        if (stm == ~us)
            atts &= ~frozenEnemy;
        if (!atts || d + 2 >= int(sizeof(gain) / sizeof(gain[0])))
            break;

        // least valuable attacker
        Square   from = SQ_NONE;
        for (PieceType pt : {PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING})
            if (Bitboard b = atts & pos.pieces(stm, pt))
            {
                from = lsb(b);
                break;
            }
        if (from == SQ_NONE)
            break;

        ++d;
        gain[d] = PieceValue[pos.piece_on(from)] - gain[d - 1];
        occ ^= square_bb(from);
        stm = ~stm;
    }

    while (--d > 0)
        gain[d - 1] = -std::max(-gain[d - 1], gain[d]);
    return gain[0] > 0 && d >= 0 ? std::max(0, gain[0]) : std::max(0, gain[0]);
}

// Tension points of the position: enemy pieces we already attack, with their
// defender sets. Computed once per node, consumed per candidate gate.
struct SpellTension {
    Square   sq;
    Bitboard defenders;
    int      baseGain;  // swap value with nothing frozen
};

struct SpellTensionList {
    SpellTension t[16];
    int          n = 0;
};

inline void spell_collect_tension(const Position& pos, Color us, SpellTensionList& out) {

    out.n = 0;
    const Bitboard ourAttacks =
      pos.attacks_by<PAWN>(us) | pos.attacks_by<KNIGHT>(us) | pos.attacks_by<BISHOP>(us)
      | pos.attacks_by<ROOK>(us) | pos.attacks_by<QUEEN>(us) | pos.attacks_by<KING>(us);

    Bitboard targets = pos.pieces(~us) & ourAttacks;
    while (targets && out.n < 16)
    {
        const Square s      = pop_lsb(targets);
        out.t[out.n].sq     = s;
        out.t[out.n].defenders = pos.attackers_to(s, pos.pieces()) & pos.pieces(~us);
        out.t[out.n].baseGain  = spell_swap(pos, us, s, 0);
        ++out.n;
    }
}

// Tactical value the freeze at g buys: the best IMPROVEMENT over the
// unfrozen exchange across the position's tension points ("freeze the
// defender, then take"). 0 when the zone changes no exchange.
inline int
spell_see_freeze(const Position& pos, Color us, Square g, const SpellTensionList& tension) {

    const Bitboard zone   = FreezeZoneBB[g];
    const Bitboard frozen = zone & pos.pieces(~us);
    if (!frozen)
        return 0;

    int best = 0;
    for (int i = 0; i < tension.n; ++i)
    {
        const SpellTension& t = tension.t[i];
        // The zone must touch the exchange to change it: either a defender
        // of the target or the target itself is frozen
        if (!(t.defenders & frozen) && !(square_bb(t.sq) & frozen))
            continue;
        const int withFreeze = spell_swap(pos, us, t.sq, frozen);
        best                 = std::max(best, withFreeze - t.baseGain);
    }
    return best;
}

}  // namespace Stockfish

#endif  // #ifndef SPELL_ORDER_H_INCLUDED
