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

// Transposition-table position superiority for spells in hand, the
// yaneuraou trick for pieces in hand transplanted to the spell reserve.
//
// A spell in hand is pure optionality for its owner. Casting always happens
// together with a regular move, so a bigger hand can only ADD gated moves to
// the side to move; it never removes one and it never touches the opponent's
// options. Every strategy playable with the smaller hand is therefore still
// playable with the bigger one, which makes the value of the superior
// position at least the value of the inferior one, and makes a stored LOWER
// bound of the inferior position a valid lower bound here.
//
// So the search probes a couple of dominated keys (this position with one
// freeze fewer, and with one jump fewer, both for the side to move) and
// accepts them for a fail-high only. Deliberately one-directional:
//   - Upper bounds are never accepted: the extra optionality can push the
//     true value straight through a stored ceiling.
//   - Exact entries qualify only because an exact score is also a lower
//     bound, and they are consumed as a bound, never republished as exact.
//   - Only the hand of the SIDE TO MOVE is varied. A bigger hand for the
//     opponent points the inequality the other way.
//
// The known objection to "extra spells are never bad" is that the static
// evaluation prices the LAST spell specially, and that reserves matter when
// the rival runs dry. That objection is about how the net prices a hand, not
// about which lines exist: it applies to the leaf values of both positions
// alike and never invents a line the superior side cannot copy. The one real
// hole is stalemate, which is a draw here: a cast can also unblock a move (a
// jump makes a slider transparent, a freeze can legalize a castling), so a
// side with an empty hand and a total blockade draws where the same side
// holding one spell would be forced to move. It takes a full blockade, and
// it is the same class of unsoundness null-move pruning already accepts.
//
// SpellHandTTSuperiority switches the probe on; SpellHandTTSupMinDepth keeps
// it away from the horizon, where two extra table lookups per node would be
// pure cost.
extern int SpellHandTTSuperiority;  // 0 (off)
extern int SpellHandTTSupMinDepth;  // 4

}  // namespace Stockfish

#endif  // #ifndef SPELL_PARAMS_H_INCLUDED
