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

#ifndef MOVEPICK_H_INCLUDED
#define MOVEPICK_H_INCLUDED

#include "history.h"
#include "movegen.h"
#include "types.h"

namespace Stockfish {

class Position;

// The per-thread bump arena the MovePickers claim their move buffers from.
// It is sized for a nesting nobody ever reaches (one MAX_MOVES slot per live
// picker, at every ply), so the far end of it is never written: the arena
// reserves its address space once and commits only up to the high-water mark
// of the bump pointer. Slots are claimed and released strictly LIFO.
class MoveArena {
   public:
    MoveArena()                            = default;
    MoveArena(const MoveArena&)            = delete;
    MoveArena& operator=(const MoveArena&) = delete;
    ~MoveArena();

    void reserve(usize slots);
    void reset() { top = base; }

    ExtMove* claim() {
        ExtMove* slot = top;
        top += MAX_MOVES;
        if (top > committed)
            commit_through(top);
        return slot;
    }
    void release() { top -= MAX_MOVES; }

   private:
    void commit_through(ExtMove* want);

    ExtMove* base      = nullptr;
    ExtMove* top       = nullptr;
    ExtMove* committed = nullptr;  // end of the memory actually paid for
    ExtMove* end       = nullptr;  // end of the reservation
};

// The MovePicker class is used to pick one pseudo-legal move at a time from the
// current position. The most important method is next_move(), which emits one
// new pseudo-legal move on every call, until there are no moves left, when
// Move::none() is returned. In order to improve the efficiency of the alpha-beta
// algorithm, MovePicker attempts to return the moves which are most likely to get
// a cut-off first.
class MovePicker {

   public:
    MovePicker(const MovePicker&)            = delete;
    MovePicker& operator=(const MovePicker&) = delete;
    // Spell chess: the caller provides the buffers via a per-thread bump
    // arena (a MAX_MOVES ExtMove slot claimed in the constructor, released
    // in the destructor — MovePicker lifetimes are strictly nested, so LIFO
    // reuse is safe even when a singular verification search re-enters
    // search() at the same ply) plus a MAX_MOVES Move generation scratch,
    // consumed within each next_move() call. Keeping them off the C++ stack
    // keeps search frames small — a stack-local MAX_MOVES array would
    // page-probe ~256KB on every construction.
    MovePicker(const Position&,
               Move,
               Depth,
               const ButterflyHistory*,
               const LowPlyHistory*,
               const GateHistory*,
               const CapturePieceToHistory*,
               const PieceToHistory**,
               const SharedHistories*,
               int,
               MoveArena*,
               Move*,
               bool allowSpells        = true,
               bool onlyTacticalSpells = false);
    MovePicker(const Position&, Move, int, const CapturePieceToHistory*, MoveArena*, Move*);
    ~MovePicker() { arena->release(); }
    Move next_move();
    void skip_quiet_moves();

   private:
    template<typename Pred>
    Move select(Pred);
    template<GenType T>
    ExtMove* score(const Move* begin, const Move* end);

    const Position&              pos;
    const ButterflyHistory*      mainHistory;
    const LowPlyHistory*         lowPlyHistory;
    const GateHistory*           gateHistory;
    const CapturePieceToHistory* captureHistory;
    const PieceToHistory**       continuationHistory;
    const SharedHistories*       sharedHistory;
    Move                         ttMove;
    ExtMove *cur, *endCur, *endBadCaptures, *endCaptures, *endGenerated, *endSpells;
    int      stage;
    int      threshold;
    Depth    depth;
    int      ply         = 0;  // the ProbCut constructor takes none: 0 = the root
    bool     skipQuiets  = false;
    bool     allowSpells = true;
    // Shallow non-PV nodes may restrict the SPELL stage to tactical casts;
    // the royal context for that classification is computed lazily once
    // per node in SPELL_INIT (mirrors the search's own precompute).
    bool onlyTacticalSpells = false;
    // SpellMergedOrdering generated the gated quiets inside QUIET_INIT, so
    // the SPELL stage must not regenerate them
    bool      mergedSpells        = false;
    Bitboard  spellRoyalAttackers = 0;
    Square    spellOurRoyal = SQ_NONE, spellEnemyRoyal = SQ_NONE;
    MoveArena* arena;
    ExtMove*   moves;
    Move*     genScratch;
};

}  // namespace Stockfish

#endif  // #ifndef MOVEPICK_H_INCLUDED
