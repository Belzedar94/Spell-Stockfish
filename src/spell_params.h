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

// Score freeze gates by what they actually freeze. Two changes, both aimed at
// the same defect: a gate whose zone holds no enemy piece can never be
// searched (is_useless_spell drops it), yet it still competes for the
// MaxFreezeGates budget — and it wins, because SpellGateKingRingBonus fires on
// a bare zone/king-ring overlap. Measured on a K+R vs K endgame: 16 of the 17
// budgeted gates freeze nothing. With this on, empty gates never enter the
// budget and the ring bonus needs an enemy piece in the overlap.
extern int SpellFreezeGateEffectOnly;  // 0 (off)

// Extend the freeze-gate domination filter to the CAPTURES stage. That stage
// has no gate budget at all, so it expands every gate against every base
// capture; the domination argument (superset frozen, subset blocked) holds
// there exactly as it does for quiets.
extern int SpellDominateCaptures;  // 0 (off)

// ---------------------------------------------------------------------------
// Explicit spell-hand value (docs/spell-game-first-strategy.md, axis 2).
//
// Measured against run5rl before writing a line of it, comparing the engine's
// own static eval (SpellNNUE::evaluate_scaled, i.e. exactly what
// Search::Worker::evaluate returns) against its depth-14 score, ablating one
// side's holding and averaging the White-side and Black-side deltas:
//
//   * The net already prices the hand, with the right sign and a phase decay
//     of its own: 47.6cp per freeze at startpos, 45.4cp with the queens off,
//     28-30cp in a bare-board ending. Per jump: 42-95cp opening/middlegame,
//     27-36cp in an ending.
//   * At the margin that price is CORRECT. Over the four interior steps
//     (5->4, 4->3, 3->2, 2->1 freezes) on two positions the depth-14 search
//     averages 47.0cp against a static 47.5cp. A linear hand term would
//     therefore double-count a quantity the net already has right, which is
//     why SpellHandFreezeValue/SpellHandJumpValue default to 0.
//   * The one step the net gets wrong is the LAST one. Its pocket features are
//     linear, so it charges the same 47.6cp for the fifth freeze and for the
//     only one left, while the search pays ~233cp (startpos, d14) and ~99cp
//     (Italian, d10) to keep a freeze rather than run out. Jumps: static
//     107-124cp against a search 127-177cp. Running dry is a cliff a linear
//     pocket cannot see, and it is where the doc's ~70cp-per-freeze figure
//     (a five-freeze ablation, 357/5) actually comes from.
//
// Hence the shape below: a linear part (off by default, kept because it is the
// primitive SPSA can shape) plus a RESERVE bonus for still holding at least one
// of a spell type, both applied to the hand DIFFERENCE, both phase-scaled. With
// the defaults the term is exactly zero whenever both sides still hold a spell
// of each type, so it cannot perturb the net's balance in a normal position —
// it only fires on the depletion asymmetry it was measured on.
//
// Units are internal eval units, the same the NNUE output is in: at full
// material one displayed centipawn is ~3.82 of them (UCIEngine::to_cp).
extern int SpellHandFreezeValue;    // 0:   per freeze of hand difference
extern int SpellHandJumpValue;      // 0:   per jump of hand difference
extern int SpellHandFreezeReserve;  // 230: ~60cp for still holding a freeze
extern int SpellHandJumpReserve;    // 115: ~30cp for still holding a jump

// Phase scale, percent of the full-material weight left on a bare board. The
// measured residual on the last freeze is +54cp (Italian, d10) to +186cp
// (startpos, d14) with a starting army and +6 to +11cp in the B+3P ending, a
// steeper decay than the 0.61 the net's own hand value shows, so the floor
// sits below it.
extern int SpellHandPhaseEndPct;  // 25

// Non-pawn material of the starting array, the top of the phase ramp:
// 2 x (2N + 2B + 2R + Q) with SF piece values. Kings are commoners and are
// deliberately excluded from non_pawn_material().
constexpr int SpellHandPhaseFullNpm = 16604;

}  // namespace Stockfish

#endif  // #ifndef SPELL_PARAMS_H_INCLUDED
