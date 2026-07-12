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

#include "movegen.h"

#include <algorithm>
#include <cassert>
#include <initializer_list>

#include "attacks.h"
#include "bitboard.h"
#include "position.h"
#include "spell.h"
#include "spell_params.h"

namespace Stockfish {

namespace {

// Note on GenTypes in Spell Chess: self-check is legal (capture-the-king),
// so being in check does not restrict the move universe. EVASIONS is kept as
// an alias of NON_EVASIONS so that the search's staging keeps working.
//
// Sliding movement sees through jump-transparent gates (active zones and, for
// a candidate jump cast, the candidate gate). Pieces standing inside an enemy
// freeze zone cannot move at all and are excluded at the origin.

template<Direction offset>
inline Move* splat_pawn_moves(Move* moveList, Bitboard to_bb) {
    while (to_bb)
    {
        Square to   = pop_lsb(to_bb);
        *moveList++ = Move(to - offset, to);
    }
    return moveList;
}

inline Move* splat_moves(Move* moveList, Square from, Bitboard to_bb) {
    while (to_bb)
        *moveList++ = Move(from, pop_lsb(to_bb));
    return moveList;
}

template<GenType Type, Direction D, bool Enemy>
Move* make_promotions(Move* moveList, [[maybe_unused]] Square to) {

    constexpr bool all = Type == EVASIONS || Type == NON_EVASIONS;

    if constexpr (Type == CAPTURES || all)
        *moveList++ = Move::make<PROMOTION>(to - D, to, QUEEN);

    if constexpr ((Type == CAPTURES && Enemy) || (Type == QUIETS && !Enemy) || all)
    {
        *moveList++ = Move::make<PROMOTION>(to - D, to, ROOK);
        *moveList++ = Move::make<PROMOTION>(to - D, to, BISHOP);
        *moveList++ = Move::make<PROMOTION>(to - D, to, KNIGHT);
    }

    return moveList;
}


// Pawns ignore the generic target: their pushes are governed by the
// phase-flipped occupancy and their captures by physical enemies, both
// computed here (EVASIONS equals NON_EVASIONS in Spell Chess).
template<Color Us, GenType Type>
Move* generate_pawn_moves(const Position& pos, Move* moveList) {

    constexpr Color     Them     = ~Us;
    constexpr Bitboard  TRank7BB = (Us == WHITE ? Rank7BB : Rank2BB);
    constexpr Bitboard  TRank3BB = (Us == WHITE ? Rank3BB : Rank6BB);
    constexpr Direction Up       = pawn_push(Us);
    constexpr Direction UpRight  = (Us == WHITE ? NORTH_EAST : SOUTH_WEST);
    constexpr Direction UpLeft   = (Us == WHITE ? NORTH_WEST : SOUTH_EAST);

    // Pawn pushes use a "phase-flipped" virtual occupancy (reference rule):
    // jump-transparent squares invert their state — an occupied one counts as
    // empty (the push may land there; do_move resolves it as a capture), an
    // empty one counts as solid. Pawn captures target physical enemies.
    // Note: a push may even land on an OWN piece standing on a transparent
    // square — the reference resolves this as a self-capture in do_move.
    const Bitboard flipOcc      = pos.pieces() ^ pos.jump_transparent();
    const Bitboard emptySquares = ~flipOcc;
    const Bitboard enemies      = pos.pieces(Them);

    const Bitboard movablePawns = pos.pieces(Us, PAWN) & ~pos.frozen_squares(Us);

    Bitboard pawnsOn7    = movablePawns & TRank7BB;
    Bitboard pawnsNotOn7 = movablePawns & ~TRank7BB;

    // Single and double pawn pushes, no promotions. Under the phase-flip rule
    // a push may land on ANY piece standing on a transparent square (even an
    // own one, resolved as a self-capture), so pushes landing on a physically
    // occupied square belong to the CAPTURES partition and the rest are true
    // quiets. Both partitions together preserve the legal universe.
    {
        const Bitboard step1 = shift<Up>(pawnsNotOn7) & emptySquares;
        const Bitboard step2 = shift<Up>(step1 & TRank3BB) & emptySquares;

        if constexpr (Type != CAPTURES)
        {
            moveList = splat_pawn_moves<Up>(moveList, step1 & ~pos.pieces());
            moveList = splat_pawn_moves<Up + Up>(moveList, step2 & ~pos.pieces());
        }
        if constexpr (Type == CAPTURES || Type == EVASIONS || Type == NON_EVASIONS)
        {
            moveList = splat_pawn_moves<Up>(moveList, step1 & pos.pieces());
            moveList = splat_pawn_moves<Up + Up>(moveList, step2 & pos.pieces());
        }
    }

    // Promotions and underpromotions. A promotion push landing on a
    // physically occupied transparent square is a capture under the
    // phase-flip rule, so all four promotions belong to the CAPTURES
    // partition (Enemy=true), like promotion captures.
    if (pawnsOn7)
    {
        Bitboard b1  = shift<UpRight>(pawnsOn7) & enemies;
        Bitboard b2  = shift<UpLeft>(pawnsOn7) & enemies;
        Bitboard b3  = shift<Up>(pawnsOn7) & emptySquares;
        Bitboard b3c = b3 & pos.pieces();
        Bitboard b3q = b3 & ~pos.pieces();

        while (b1)
            moveList = make_promotions<Type, UpRight, true>(moveList, pop_lsb(b1));

        while (b2)
            moveList = make_promotions<Type, UpLeft, true>(moveList, pop_lsb(b2));

        while (b3c)
            moveList = make_promotions<Type, Up, true>(moveList, pop_lsb(b3c));

        while (b3q)
            moveList = make_promotions<Type, Up, false>(moveList, pop_lsb(b3q));
    }

    // Standard and en passant captures
    if constexpr (Type == CAPTURES || Type == EVASIONS || Type == NON_EVASIONS)
    {
        Bitboard b1 = shift<UpRight>(pawnsNotOn7) & enemies;
        Bitboard b2 = shift<UpLeft>(pawnsNotOn7) & enemies;

        moveList = splat_pawn_moves<UpRight>(moveList, b1);
        moveList = splat_pawn_moves<UpLeft>(moveList, b2);

        if (pos.ep_square() != SQ_NONE)
        {
            assert(rank_of(pos.ep_square()) == relative_rank(Us, RANK_6));

            // The capturing pawn may itself be frozen
            b1 = pawnsNotOn7 & Attacks::attacks_bb<PAWN>(pos.ep_square(), Them);

            while (b1)
                *moveList++ = Move::make<EN_PASSANT>(pop_lsb(b1), pos.ep_square());
        }
    }

    return moveList;
}


template<Color Us, PieceType Pt>
Move* generate_moves(const Position& pos, Move* moveList, Bitboard target, Bitboard occSliding) {

    static_assert(Pt != KING && Pt != PAWN, "Unsupported piece type in generate_moves()");

    Bitboard bb = pos.pieces(Us, Pt) & ~pos.frozen_squares(Us);

    while (bb)
    {
        Square   from = pop_lsb(bb);
        Bitboard b    = Attacks::attacks_bb<Pt>(from, occSliding) & target;

        moveList = splat_moves(moveList, from, b);
    }

    return moveList;
}


// Appends the gated (spell-casting) versions of the base moves, plus the new
// slider/pawn moves a candidate jump gate enables. See SPELL_SPEC.md §4.
template<Color Us, GenType Type>
Move* generate_spell_moves(const Position& pos, Move* baseStart, Move* baseEnd) {

    Move* cur = baseEnd;

    const Bitboard occupied   = pos.pieces();
    const Bitboard occSliding = pos.occupied_for_sliding();
    const Bitboard frozenUs   = pos.frozen_squares(Us);

    const auto keep = [](bool isCapture) {
        return Type == CAPTURES ? isCapture : Type == QUIETS ? !isCapture : true;
    };

    // Search policy (not a rule): the QUIETS stage considers only the top
    // few gates by impact score. The full universe stays available to
    // LEGAL/NON_EVASIONS (perft, UCI) and to EVASIONS (in check), and the
    // limit is lifted while an enemy freeze zone is active.
    const bool limitGates = Type == QUIETS && !pos.spell_zone(~Us, SPELL_FREEZE);

    const Square   eksq = pos.count<KING>(~Us) ? pos.square<KING>(~Us) : SQ_NONE;
    const Bitboard eRing =
      eksq != SQ_NONE ? Attacks::attacks_bb<KING>(eksq) | square_bb(eksq) : Bitboard(0);

    // Squares revealed to each own slider by lifting one blocker, scored once
    int  jumpScore[SQUARE_NB];
    bool jumpScoreReady = false;

    for (SpellType sp : {SPELL_FREEZE, SPELL_JUMP})
    {
        if (!pos.can_cast(Us, sp))
            continue;

        Bitboard allGates = sp == SPELL_FREEZE ? ~Bitboard(0) : occupied;

        Square gateList[SQUARE_NB];
        int    gateCount = 0;

        if (!limitGates)
        {
            for (Bitboard b = allGates; b;)
                gateList[gateCount++] = pop_lsb(b);
        }
        else
        {
            struct GateScore {
                Square g;
                int    score;
            };
            GateScore scored[SQUARE_NB];
            int       n = 0, ringCount = 0;

            if (sp == SPELL_JUMP && !jumpScoreReady)
            {
                std::fill_n(jumpScore, SQUARE_NB, 0);
                Bitboard sliders = pos.pieces(Us, BISHOP, ROOK, QUEEN) & ~frozenUs;
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
                        for (Bitboard t = reveal & pos.pieces(~Us); t;)
                            s += PieceValue[pos.piece_on(pop_lsb(t))];
                        if (eksq != SQ_NONE && (reveal & square_bb(eksq)))
                            s += SpellGateKingBonus;
                        jumpScore[b] += s;
                    }
                }
                jumpScoreReady = true;
            }

            for (Bitboard b = allGates; b;)
            {
                const Square g = pop_lsb(b);
                int          s = 0;

                if (sp == SPELL_FREEZE)
                {
                    const Bitboard zone = FreezeZoneBB[g];
                    for (Bitboard t = zone & pos.pieces(~Us); t;)
                        s += PieceValue[pos.piece_on(pop_lsb(t))];
                    if (eksq != SQ_NONE && (zone & square_bb(eksq)))
                        s += SpellGateKingBonus;
                    if (zone & eRing)
                    {
                        s += SpellGateKingRingBonus;
                        ++ringCount;
                    }
                }
                else
                    s = jumpScore[g];

                scored[n++] = {g, s};
            }

            int limit = sp == SPELL_FREEZE ? MaxFreezeGates : MaxJumpGates;
            if (sp == SPELL_FREEZE && ringCount > limit)
                limit = ringCount;
            if (n > limit)
            {
                std::partial_sort(scored, scored + limit, scored + n,
                                  [](const GateScore& a, const GateScore& b) {
                                      return a.score > b.score;
                                  });
                n = limit;
            }
            for (int i = 0; i < n; ++i)
                gateList[gateCount++] = scored[i].g;
        }

        for (int gi = 0; gi < gateCount; ++gi)
        {
            const Square gate = gateList[gi];

            if (sp == SPELL_FREEZE)
            {
                // Gate the base moves. The caster's own move may not start on
                // the gate square or its orthogonal neighbors.
                const Bitboard blocked = FreezeBlockBB[gate];

                for (Move* it = baseStart; it != baseEnd; ++it)
                {
                    const Move     base = *it;
                    const MoveType mt   = base.type_of();

                    if (mt != NORMAL && mt != CASTLING)
                        continue;
                    if (blocked & base.from_sq())
                        continue;
                    // Reference rule: a freeze-gated castling may not use the
                    // king's destination square as the gate
                    if (mt == CASTLING
                        && gate
                             == relative_square(Us, base.to_sq() > base.from_sq() ? SQ_G1 : SQ_C1))
                        continue;
                    if (!keep(pos.capture(base)))
                        continue;

                    *cur++ = Move::make_spell(base, SPELL_FREEZE, gate);
                }
            }
            else  // SPELL_JUMP
            {
                // Gate the base moves (may not land on the gate square)
                for (Move* it = baseStart; it != baseEnd; ++it)
                {
                    const Move     base = *it;
                    const MoveType mt   = base.type_of();

                    if (mt != NORMAL && mt != CASTLING)
                        continue;
                    if (base.to_sq() == gate)
                        continue;
                    if (!keep(pos.capture(base)))
                        continue;

                    *cur++ = Move::make_spell(base, SPELL_JUMP, gate);
                }

                // New slider moves through the now-transparent gate: exactly
                // the squares reachable with the gate removed but not before.
                const Bitboard occJump = occSliding & ~square_bb(gate);
                const Bitboard sliders =
                  pos.pieces(Us, BISHOP, ROOK, QUEEN) & ~frozenUs & ~square_bb(gate);

                Bitboard bb = sliders;
                while (bb)
                {
                    const Square    from = pop_lsb(bb);
                    const PieceType pt   = type_of(pos.piece_on(from));

                    Bitboard extra = (Attacks::attacks_bb(pt, from, occJump)
                                      & ~Attacks::attacks_bb(pt, from, occSliding))
                                   & ~pos.pieces(Us) & ~square_bb(gate)
                                   & ~(pos.jump_transparent() & ~occupied);

                    while (extra)
                    {
                        const Square to = pop_lsb(extra);
                        if (keep(bool(occupied & to)))
                            *cur++ = Move::make_spell(Move(from, to), SPELL_JUMP, gate);
                    }
                }

                // New pawn double pushes whose intermediate square is the gate
                constexpr Direction Up      = pawn_push(Us);
                constexpr Bitboard  TRank2  = (Us == WHITE ? Rank2BB : Rank7BB);
                const Bitboard      midGate = square_bb(gate) & (Us == WHITE ? Rank3BB : Rank6BB);

                if (midGate && (occSliding & gate))
                {
                    const Square mid  = gate;
                    const Square from = mid - Up;
                    const Square to   = mid + Up;

                    // Landing uses the phase-flipped occupancy incl. the
                    // candidate gate (occupied, so flipped out); the landing
                    // classifies as capture/quiet by physical occupancy
                    const Bitboard flipOcc =
                      (occupied ^ pos.jump_transparent()) ^ square_bb(gate);

                    if ((pos.pieces(Us, PAWN) & ~frozenUs & square_bb(from)) && (TRank2 & from)
                        && !(flipOcc & to) && keep(bool(occupied & to)))
                        *cur++ = Move::make_spell(Move(from, to), SPELL_JUMP, gate);
                }
            }
        }
    }

    return cur;
}


template<Color Us, GenType Type>
Move* generate_all(const Position& pos, Move* moveList) {

    static_assert(Type != LEGAL, "Unsupported type in generate_all()");

    // Terminal position: a king has been captured, the game is over
    if (!pos.count<KING>(Us) || !pos.count<KING>(~Us))
        return moveList;

    const Square   ksq        = pos.square<KING>(Us);
    const Bitboard occSliding = pos.occupied_for_sliding();

    const Bitboard target = Type == CAPTURES ? pos.pieces(~Us)
                          : Type == QUIETS   ? ~pos.pieces()
                                             : ~pos.pieces(Us);  // EVASIONS | NON_EVASIONS

    // Pieces (not pawns) may not land quietly on jump-transparent squares
    // (reference rule); captures of pieces standing on them are fine.
    const Bitboard banned      = pos.jump_transparent() & ~pos.pieces();
    const Bitboard pieceTarget = target & ~banned;

    Move* cur = moveList;

    moveList = generate_pawn_moves<Us, Type>(pos, moveList);
    moveList = generate_moves<Us, KNIGHT>(pos, moveList, pieceTarget, occSliding);
    moveList = generate_moves<Us, BISHOP>(pos, moveList, pieceTarget, occSliding);
    moveList = generate_moves<Us, ROOK>(pos, moveList, pieceTarget, occSliding);
    moveList = generate_moves<Us, QUEEN>(pos, moveList, pieceTarget, occSliding);

    if (!(pos.frozen_squares(Us) & ksq))
    {
        Bitboard b = Attacks::attacks_bb<KING>(ksq) & pieceTarget;
        moveList   = splat_moves(moveList, ksq, b);

        if ((Type == QUIETS || Type == EVASIONS || Type == NON_EVASIONS)
            && pos.can_castle(Us & ANY_CASTLING))
            for (CastlingRights cr : {Us & KING_SIDE, Us & QUEEN_SIDE})
                if (!pos.castling_impeded(cr) && pos.can_castle(cr)
                    && !(pos.frozen_squares(Us) & pos.castling_rook_square(cr)))
                    *moveList++ = Move::make<CASTLING>(ksq, pos.castling_rook_square(cr));
    }

    return generate_spell_moves<Us, Type>(pos, cur, moveList);
}

}  // namespace


// <CAPTURES>     Generates all pseudo-legal captures plus queen promotions
// <QUIETS>       Generates all pseudo-legal non-captures and underpromotions
// <EVASIONS>     Same as NON_EVASIONS (self-check is legal in Spell Chess)
// <NON_EVASIONS> Generates all pseudo-legal captures and non-captures
//
// Returns a pointer to the end of the move list.
template<GenType Type>
Move* generate(const Position& pos, Move* moveList) {

    static_assert(Type != LEGAL, "Unsupported type in generate()");

    Color us = pos.side_to_move();

    return us == WHITE ? generate_all<WHITE, Type>(pos, moveList)
                       : generate_all<BLACK, Type>(pos, moveList);
}

// Explicit template instantiations
template Move* generate<CAPTURES>(const Position&, Move*);
template Move* generate<QUIETS>(const Position&, Move*);
template Move* generate<EVASIONS>(const Position&, Move*);
template Move* generate<NON_EVASIONS>(const Position&, Move*);

// generate<LEGAL> generates all the legal moves in the given position

template<>
Move* generate<LEGAL>(const Position& pos, Move* moveList) {

    Move* cur = moveList;

    moveList = pos.side_to_move() == WHITE ? generate_all<WHITE, NON_EVASIONS>(pos, moveList)
                                           : generate_all<BLACK, NON_EVASIONS>(pos, moveList);

    // Self-check is legal: only king moves (no stepping into an attacked
    // square), castling (path attacks, spell context) and en passant (the
    // classic does-it-leave-the-king-attacked test) need the full legality
    // test; everything else is legal by construction.
    while (cur != moveList)
        if ((cur->type_of() == CASTLING || cur->type_of() == EN_PASSANT
             || type_of(pos.moved_piece(*cur)) == KING)
            && !pos.legal(*cur))
            *cur = *(--moveList);
        else
            ++cur;

    return moveList;
}

}  // namespace Stockfish
