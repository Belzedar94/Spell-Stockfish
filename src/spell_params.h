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

#ifndef SPELL_PARAMS_H_INCLUDED
#define SPELL_PARAMS_H_INCLUDED

namespace Stockfish {

// Search-policy constants for spell selectivity. These do NOT affect the
// legal move universe (perft, UCI validation) — only which gated moves the
// search stages consider and how deep they are searched. All of them are
// SPSA-tunable: plain globals registered with the TUNE machinery in
// spell_params.cpp (initial values from the public reference baseline).

// Cap on candidate gate squares per spell in the QUIETS generation stage
// (unlimited while an enemy freeze zone is active, and in-check nodes use
// the full EVASIONS universe anyway).
extern int MaxFreezeGates;  // 12
extern int MaxJumpGates;    // 6

// Gate impact scoring bonuses
extern int SpellGateKingBonus;      // 10000: zone covers the enemy king
extern int SpellGateKingRingBonus;  // 50000: zone touches the enemy king ring

// Depth penalty (plies) for gated moves: the reference searches spell moves
// shallower (PotionDepthPenaltyTactical/Quiet), which is where a large share
// of its strength comes from.
extern int SpellDepthPenaltyTactical;  // 1
extern int SpellDepthPenaltyQuiet;     // 2

// LMR: tactical spells are reduced this much less (millidepth, 1024 = 1 ply)
extern int SpellTacticalLmrBonus;  // 1024

// Cap on the linear moveCount discount inside LMR: the chess-tuned term
// r -= moveCount * 62 runs away at spell move counts (500-3000) and would
// EXTEND late moves without this cap.
extern int SpellLmrMoveCountCap;  // 24

// GateHistory weights in quiet ordering and in the LMR statScore
extern int SpellGateHistOrderWeight;  // 2
extern int SpellGateHistStatWeight;   // 2

// Relevance gate for the SPELL stage: a cast is worth at most about a
// tempo plus bounded tactics, so nodes whose static eval sits further
// than this below alpha skip the gated-quiet expansion entirely
// (PV nodes and nodes with our king under attack always search spells).
extern int SpellStageMargin;  // 400

// Below this depth, non-PV nodes with a safe king expand only TACTICAL
// quiet spells: quiet casts at the horizon feed the branching explosion
// for at most about a tempo of value. 0 = no restriction (historical).
extern int SpellQuietMinDepth;  // 0

// ---------------------------------------------------------------------------
// SPRT candidate toggles (2026-07-13): every default preserves the current
// behavior EXACTLY (bench-identical), so each idea ships as an options-diff
// SPRT on the tower. The shared hypothesis: SF-master heuristics are tuned
// for chess trees (branching ~35) and misfire at spell branching ~1650.

// Generate gated quiets together with the base quiets and order them in one
// list (FSF-style interleaving) instead of the lazy late SPELL stage. Trades
// the laziness win for first-visit ordering of the variant's key resource.
extern int SpellMergedOrdering;  // 0 (off)

// Skip null-move pruning while the OPPONENT has a freeze available: the
// null-move assumption (a free tempo keeps beta) is unsound when the reply
// can freeze our answer.
extern int SpellNullMoveGuard;  // 0 (off)

// Percent scale on the late-move-count pruning threshold (100 = stock).
// Chess-tuned LMP at 1650-branching nodes skips almost everything.
extern int SpellLmpScalePct;  // 100

// Percent scale on the parent futility margin (100 = stock).
extern int SpellFutilityScalePct;  // 100

// Do not apply the spell depth penalties on PV nodes.
extern int SpellNoPenaltyPV;  // 0 (off)

// Percent scale on the initial aspiration window (100 = stock): spell evals
// swing harder than chess evals, and re-search storms are costly.
extern int SpellAspirationPct;  // 100

// Extra margin (cp) added to the SEE pruning threshold of GATED captures:
// positive values prune fewer spell captures.
extern int SpellCaptureSeeMargin;  // 0

// Disable internal iterative reductions: at huge branching, a missing TT
// move is common and IIR compounds the ordering weakness.
extern int SpellNoIIR;  // 0 (off)

// Do not update continuation histories with spell moves: gated moves share
// the (piece, to) key with their base move and pollute its stats.
extern int SpellContHistSkip;  // 0 (off)

// Skip razoring while WE can still cast: razoring drops straight into a
// qsearch that cannot see spells, so positions with a saving cast get
// misjudged.
extern int SpellRazorGuard;  // 0 (off)

// Structural roadmap pillar B: cast decomposition. The SPELL stage emits ONE
// declaration sentinel per viable (spell, gate) instead of the full gated
// product (~130 vs ~1650 edges); the search expands the quiet completions
// at the same ply through the classic do_move (equivalence proven by
// perftdec). Gated captures keep flowing through the capture stages. The
// root never decomposes (UCI surface untouched). 0 = off (bench-identical).
extern int SpellDecompose;  // 0 (off)

}  // namespace Stockfish

#endif  // #ifndef SPELL_PARAMS_H_INCLUDED
