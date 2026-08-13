/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
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

#include "evaluate.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>

#include "misc.h"
#include "nnue/network.h"
#include "nnue/nnue_misc.h"
#include "position.h"
#include "spell_params.h"
#include "types.h"
#include "uci.h"
#include "nnue/nnue_accumulator.h"

namespace Stockfish {

// Spell chess: the value of the spells still in hand, from the side to move.
//
// The networks already price the hand linearly and already decay it with the
// phase, and at the margin they price it correctly (see spell_params.h for the
// measurement). What they cannot express is depletion: a linear pocket feature
// charges the same for the fifth freeze and for the last one, while the search
// pays several times more to avoid running dry. So the default configuration is
// a pure reserve term — zero whenever both sides still hold a spell of each
// type — and the linear coefficients are left for SPSA to discover.
Value Eval::spell_hand_value(const Position& pos) {

    const Color us = pos.side_to_move(), them = ~us;

    const int freeze = pos.spells_in_hand(us, SPELL_FREEZE) - pos.spells_in_hand(them, SPELL_FREEZE);
    const int jump   = pos.spells_in_hand(us, SPELL_JUMP) - pos.spells_in_hand(them, SPELL_JUMP);

    const int freezeReserve = (pos.spells_in_hand(us, SPELL_FREEZE) > 0)
                            - (pos.spells_in_hand(them, SPELL_FREEZE) > 0);
    const int jumpReserve =
      (pos.spells_in_hand(us, SPELL_JUMP) > 0) - (pos.spells_in_hand(them, SPELL_JUMP) > 0);

    const int hand = SpellHandFreezeValue * freeze + SpellHandJumpValue * jump
                   + SpellHandFreezeReserve * freezeReserve + SpellHandJumpReserve * jumpReserve;

    if (!hand)
        return VALUE_ZERO;

    // A spell is worth more with a full army on the board than in a pawn
    // ending: full weight at the starting non-pawn material, decaying to
    // SpellHandPhaseEndPct of it on a bare board.
    const int npm   = std::min(int(pos.non_pawn_material()), SpellHandPhaseFullNpm);
    const int scale = SpellHandPhaseEndPct
                    + (100 - SpellHandPhaseEndPct) * npm / SpellHandPhaseFullNpm;

    return Value(hand * scale / 100);
}

// Evaluate is the evaluator for the outer world. It returns a static evaluation
// of the position from the point of view of the side to move.
Value Eval::evaluate(const Eval::NNUE::Network&     network,
                     const Position&                pos,
                     Eval::NNUE::AccumulatorStack&  accumulators,
                     Eval::NNUE::AccumulatorCaches& caches,
                     int                            optimism) {

    // Spell chess: self-check is legal (capture-the-king), so the search
    // treats in-check nodes like normal ones and DOES evaluate them
    // statically — the stock no-eval-in-check invariant does not apply.

    auto [psqt, positional] = network.evaluate(pos, accumulators, caches);

    Value nnue = psqt + positional;

    // Blend optimism and eval with nnue complexity
    int nnueComplexity = std::abs(psqt - positional);
    optimism += optimism * i64(nnueComplexity) / 476;
    nnue -= nnue * i64(nnueComplexity) / 18236;

    int material = 534 * pos.count<PAWN>() + pos.non_pawn_material();
    int v        = (nnue * i64(77871 + material) + optimism * i64(7191 + material)) / 77871;

    // Damp down the evaluation linearly when shuffling
    v -= v * pos.rule50_count() / 199;

    // Guarantee evaluation does not hit the tablebase range
    v = std::clamp(v, VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);

    return v;
}

// Like evaluate(), but instead of returning a value, it returns
// a string (suitable for outputting to stdout) that contains the detailed
// descriptions and values of each evaluation term. Useful for debugging.
// Trace scores are from white's point of view
std::string Eval::trace(Position& pos, const Eval::NNUE::Network& network) {

    if (pos.checkers())
        return "Final evaluation: none (in check)";

    auto accumulators = std::make_unique<Eval::NNUE::AccumulatorStack>();
    auto caches       = std::make_unique<Eval::NNUE::AccumulatorCaches>(network);

    std::stringstream ss;
    ss << std::showpoint << std::noshowpos << std::fixed << std::setprecision(2);
    ss << '\n' << NNUE::trace(pos, network, *caches) << '\n';

    ss << std::showpoint << std::showpos << std::fixed << std::setprecision(2) << std::setw(15);

    auto [psqt, positional] = network.evaluate(pos, *accumulators, *caches);
    Value v                 = psqt + positional;
    ss << "NNUE evaluation          " << v << " (side to move, internal units)\n";
    v = pos.side_to_move() == WHITE ? v : -v;
    ss << "NNUE evaluation        " << 0.01 * UCIEngine::to_cp(v, pos) << " (white side)\n";

    v = evaluate(network, pos, *accumulators, *caches, VALUE_ZERO);
    v = pos.side_to_move() == WHITE ? v : -v;

    ss << "Final evaluation      ";
    ss << 0.01 * UCIEngine::to_cp(v, pos) << " (white side)";
    ss << " [with scaled NNUE, ...]\n";

    return ss.str();
}

}  // namespace Stockfish
