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

// PUCT Monte-Carlo tree search for Spell Chess (UseMCTS=1), an alternative
// to alpha-beta at branching ~1650. Reference points: ianfab's
// chess-variant-mcts (UCT over engine evals, mean backup) and the LC0/PUCT
// formulation. No policy network yet: priors are heuristic (captures MVV,
// gate impact score for casts, butterfly history for quiets, softmax); leaf
// values are the spell-NNUE static eval squashed to [-1, 1]; PROGRESSIVE
// WIDENING caps the children considered at sqrt-of-visits, which is the
// piece that makes b~1650 tractable at all. Single-threaded (SPRT presets
// run Threads=1); the classic search is untouched when the toggle is off.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "misc.h"
#include "movegen.h"
#include "notation.h"
#include "position.h"
#include "search.h"
#include "spell_order.h"
#include "spell_params.h"
#include "thread.h"
#include "types.h"

namespace Stockfish {

namespace {

struct MctsNode {
    double   totalW     = 0.0;  // accumulated leaf values, WHITE point of view
    uint32_t visits     = 0;
    uint32_t firstChild = 0xFFFFFFFF;
    uint16_t nChildren  = 0;
    bool     expanded   = false;
    bool     terminal   = false;
    float    termValue  = 0.0f;  // white POV, only meaningful when terminal
    float    prior      = 0.0f;
    Move     move       = Move::none();
};

// cp -> [-1, 1]; 400cp ~ 0.76 keeps midgame swings informative
inline float to_unit(Value v) { return float(std::tanh(double(v) / 400.0)); }

constexpr double CPUCT     = 2.0;
constexpr float  FPU_LOSS  = 0.25f;  // first-play-urgency malus vs parent mean
constexpr int    WIDEN_MIN = 3;      // children always considered

inline int widened(uint32_t visits, int n) {
    return std::min(n, WIDEN_MIN + int(std::sqrt(double(visits))));
}

}  // namespace

void Search::Worker::mcts_search() {

    // ~40 MB of tree per engine instance: enough for hundreds of thousands
    // of playouts, small enough for 48 concurrent match processes
    constexpr size_t PoolCap = 1u << 20;

    std::vector<MctsNode> pool;
    pool.reserve(PoolCap);
    pool.emplace_back();  // root

    Position&              pos = rootPos;
    std::vector<StateInfo> stStack(512);
    std::vector<uint32_t>  path;
    std::vector<Move>      played;
    path.reserve(512);
    played.reserve(512);

    // Time budget: movetime verbatim, otherwise the time manager's optimum;
    // 'infinite'/'nodes' run until stop or the node limit
    const TimePoint start = now();
    TimePoint       budget = TimePoint(-1);
    if (limits.movetime)
        budget = limits.movetime;
    else if (limits.use_time_management())
        budget = main_manager()->tm.optimum();

    // Scores each legal move into a softmax prior (no policy net: captures
    // by MVV, casts by gate impact, quiets by butterfly history)
    const auto expand = [&](uint32_t idx) {
        MctsNode& node = pool[idx];

        // Terminal detection first (capture-the-king rules)
        const Color us = pos.side_to_move();
        if (!pos.count<KING>(us) || !pos.count<KING>(~us))
        {
            node.terminal  = true;
            node.termValue = !pos.count<KING>(us) == (us == WHITE) ? -1.0f : 1.0f;
            return;
        }
        if (pos.is_draw(int(played.size())))
        {
            node.terminal  = true;
            node.termValue = 0.0f;
            return;
        }

        MoveList<LEGAL> legal(pos);
        if (legal.size() == 0)
        {
            node.terminal = true;
            const float stmValue = pos.checkers() ? -1.0f : 0.0f;
            node.termValue       = us == WHITE ? stmValue : -stmValue;
            return;
        }

        if (pool.size() + legal.size() > PoolCap)
            return;  // pool exhausted: stay a leaf, keep backing up evals

        const Square   eksq = pos.count<KING>(~us) ? pos.square<KING>(~us) : SQ_NONE;
        const Bitboard eRing =
          eksq != SQ_NONE ? Attacks::attacks_bb<KING>(eksq) | square_bb(eksq) : Bitboard(0);

        struct Scored {
            Move  m;
            float s;
        };
        std::vector<Scored> scored;
        scored.reserve(legal.size());

        for (const auto& m : legal)
        {
            if (m.is_spell() && is_useless_spell(pos, m))
                continue;
            float s;
            if (pos.capture(m))
                s = 9000.0f + float(PieceValue[pos.piece_on(m.to_sq())]) / 4.0f;
            else if (m.is_spell())
                s = m.spell_type() == SPELL_FREEZE
                    ? std::min(8000.0f, float(freeze_gate_score(pos, us, m.gate_sq(), eksq, eRing))
                                          / 8.0f)
                    : 2500.0f;
            else
                s = float(mainHistory[us][m.raw() & 0xFFFF]) / 4.0f;
            scored.push_back({m, s});
        }

        float smax = -1e9f;
        for (const auto& e : scored)
            smax = std::max(smax, e.s);
        float sum = 0.0f;
        for (auto& e : scored)
        {
            e.s = std::exp((e.s - smax) / 900.0f);
            sum += e.s;
        }

        std::sort(scored.begin(), scored.end(),
                  [](const Scored& a, const Scored& b) { return a.s > b.s; });

        node.firstChild = uint32_t(pool.size());
        node.nChildren  = uint16_t(scored.size());
        node.expanded   = true;
        for (const auto& e : scored)
        {
            pool.emplace_back();
            MctsNode& c = pool.back();
            c.move      = e.m;
            c.prior     = e.s / sum;
        }
    };

    // ---- playout loop ----
    uint64_t playouts = 0;
    while (!threads.stop.load(std::memory_order_relaxed))
    {
        if ((playouts & 63) == 0)
        {
            if (budget >= 0 && now() - start >= budget)
                break;
            if (limits.nodes && nodes >= uint64_t(limits.nodes))
                break;
        }

        // Selection
        path.clear();
        played.clear();
        uint32_t cur = 0;
        path.push_back(0);

        while (pool[cur].expanded && !pool[cur].terminal && played.size() + 2 < stStack.size())
        {
            const MctsNode& node  = pool[cur];
            const Color     us    = pos.side_to_move();
            const double    sqrtN = std::sqrt(double(node.visits) + 1.0);
            const int       kMax  = widened(node.visits, node.nChildren);
            const float     parentMean =
              node.visits ? float(node.totalW / node.visits) : 0.0f;
            const float fpuBase = (us == WHITE ? parentMean : -parentMean) - FPU_LOSS;

            uint32_t bestIdx = 0xFFFFFFFF;
            double   bestU   = -1e18;
            for (int i = 0; i < kMax; ++i)
            {
                const MctsNode& c = pool[node.firstChild + i];
                const float     q =
                  c.visits ? (us == WHITE ? float(c.totalW / c.visits)
                                              : -float(c.totalW / c.visits))
                               : fpuBase;
                const double u = q + CPUCT * c.prior * sqrtN / (1.0 + c.visits);
                if (u > bestU)
                {
                    bestU   = u;
                    bestIdx = node.firstChild + i;
                }
            }
            if (bestIdx == 0xFFFFFFFF)
                break;

            const Move m = pool[bestIdx].move;
            pos.do_move(m, stStack[played.size()], nullptr);
            ++nodes;
            played.push_back(m);
            path.push_back(bestIdx);
            cur = bestIdx;
        }

        // Expansion + evaluation
        float valueW;
        if (!pool[cur].terminal && !pool[cur].expanded)
            expand(cur);
        if (pool[cur].terminal)
            valueW = pool[cur].termValue;
        else
        {
            const Value v = evaluate(pos);
            valueW        = pos.side_to_move() == WHITE ? to_unit(v) : -to_unit(v);
        }

        // Backup (white POV everywhere) and unwind
        for (uint32_t idx : path)
        {
            pool[idx].totalW += valueW;
            pool[idx].visits += 1;
        }
        for (size_t i = played.size(); i-- > 0;)
            pos.undo_move(played[i]);

        ++playouts;
        if (pool.size() + 4 >= PoolCap && !pool[0].expanded)
            break;  // degenerate safety
    }

    // UCI contract: never move before 'stop'/'ponderhit' in infinite/ponder
    while (!threads.stop && (main_manager()->ponder || limits.infinite))
    {}
    threads.stop = true;

    // Best child of the root by visits (ties by mean value)
    const MctsNode& root = pool[0];
    Move            best = Move::none(), ponder = Move::none();
    if (root.expanded && root.nChildren)
    {
        uint32_t bi = root.firstChild;
        for (uint32_t i = root.firstChild; i < root.firstChild + root.nChildren; ++i)
            if (pool[i].visits > pool[bi].visits
                || (pool[i].visits == pool[bi].visits && pool[i].totalW > pool[bi].totalW))
                bi = i;
        best = pool[bi].move;

        const MctsNode& b = pool[bi];
        if (b.expanded && b.nChildren)
        {
            uint32_t pi = b.firstChild;
            for (uint32_t i = b.firstChild; i < b.firstChild + b.nChildren; ++i)
                if (pool[i].visits > pool[pi].visits)
                    pi = i;
            if (pool[pi].visits)
                ponder = pool[pi].move;
        }
    }
    if (best == Move::none())
        best = rootMoves[0].pv[0];  // pool exhaustion fallback: any legal move

    main_manager()->updates.onBestmove(Notation::move(best, pos.is_chess960()),
                                       ponder != Move::none()
                                         ? Notation::move(ponder, pos.is_chess960())
                                         : "");
}

}  // namespace Stockfish
