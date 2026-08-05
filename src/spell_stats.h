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

#ifndef SPELL_STATS_H_INCLUDED
#define SPELL_STATS_H_INCLUDED

#include <atomic>
#include <iomanip>
#include <sstream>
#include <string>

#include "types.h"

namespace Stockfish {

// Coarse class of the MovePicker stage that emitted a move. Only used by the
// picker instrumentation, which is why it is a flat list and not the internal
// staging enum: the question is "which tranche paid", not "which case ran".
enum PickClass : u8 {
    PICK_TT,
    PICK_GOOD_CAPTURE,
    PICK_QUIET,
    PICK_SPELL,
    PICK_BAD_CAPTURE,
    PICK_BAD_QUIET,
    PICK_QCAPTURE,
    PICK_OTHER,
    PICK_CLASS_NB
};

// Firing counters for the gate picker, behind the SpellPickerStats option.
// Debug instrumentation: global and relaxed, never read by the search.
namespace SpellPickerStat {

inline std::atomic<u64> best[PICK_CLASS_NB][2];  // [stage class][bestMove is a cast]
inline std::atomic<u64> castRankSum, castRankCnt;
inline std::atomic<u64> baseRankSum, baseRankCnt;
inline std::atomic<u64> gateNodes, gateUniqueSum, gateMoveSum;

inline void record_best(PickClass cls, bool isSpell, u64 rank) {
    best[cls][isSpell].fetch_add(1, std::memory_order_relaxed);
    if (isSpell)
    {
        castRankSum.fetch_add(rank, std::memory_order_relaxed);
        castRankCnt.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        baseRankSum.fetch_add(rank, std::memory_order_relaxed);
        baseRankCnt.fetch_add(1, std::memory_order_relaxed);
    }
}

inline void record_gates(u64 uniqueGates, u64 gatedMoves) {
    gateNodes.fetch_add(1, std::memory_order_relaxed);
    gateUniqueSum.fetch_add(uniqueGates, std::memory_order_relaxed);
    gateMoveSum.fetch_add(gatedMoves, std::memory_order_relaxed);
}

inline void clear() {
    for (auto& row : best)
        for (auto& cell : row)
            cell.store(0, std::memory_order_relaxed);
    castRankSum = castRankCnt = baseRankSum = baseRankCnt = 0;
    gateNodes = gateUniqueSum = gateMoveSum = 0;
}

inline std::string report() {

    static const char* names[PICK_CLASS_NB] = {"tt",          "good_capture", "quiet",    "spell",
                                               "bad_capture", "bad_quiet",    "qcapture", "other"};

    const auto ratio = [](u64 num, u64 den) { return den ? double(num) / double(den) : 0.0; };

    u64 totalBase = 0, totalCast = 0;
    for (int c = 0; c < PICK_CLASS_NB; ++c)
    {
        totalBase += best[c][0].load(std::memory_order_relaxed);
        totalCast += best[c][1].load(std::memory_order_relaxed);
    }

    std::ostringstream os;
    os << std::fixed;
    os << "Spell picker stats\n";
    os << "  stage           bestMove(base)   bestMove(cast)   cast share\n";
    for (int c = 0; c < PICK_CLASS_NB; ++c)
    {
        const u64 b = best[c][0].load(std::memory_order_relaxed);
        const u64 s = best[c][1].load(std::memory_order_relaxed);
        if (!b && !s)
            continue;
        os << "  " << std::left << std::setw(16) << names[c] << std::right << std::setw(14) << b
           << std::setw(17) << s << std::setw(13) << std::setprecision(2) << 100.0 * ratio(s, b + s)
           << "%\n";
    }
    os << "  total           " << std::right << std::setw(14) << totalBase << std::setw(17)
       << totalCast << std::setw(13) << std::setprecision(2)
       << 100.0 * ratio(totalCast, totalBase + totalCast) << "%\n";

    os << "  mean bestMove rank: cast " << std::setprecision(1)
       << ratio(castRankSum.load(std::memory_order_relaxed),
                castRankCnt.load(std::memory_order_relaxed))
       << "   base "
       << ratio(baseRankSum.load(std::memory_order_relaxed),
                baseRankCnt.load(std::memory_order_relaxed))
       << "\n";

    const u64 nodes = gateNodes.load(std::memory_order_relaxed);
    os << "  gated nodes: " << nodes << "   unique gates/node: " << std::setprecision(2)
       << ratio(gateUniqueSum.load(std::memory_order_relaxed), nodes)
       << "   gated moves/node: " << std::setprecision(1)
       << ratio(gateMoveSum.load(std::memory_order_relaxed), nodes) << "\n";

    return os.str();
}

}  // namespace SpellPickerStat
}  // namespace Stockfish

#endif  // #ifndef SPELL_STATS_H_INCLUDED
