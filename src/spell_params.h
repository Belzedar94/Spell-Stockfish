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
// Cooldown-window penalty (docs/spell-game-first-strategy.md, axis 2).
//
// A cast sets that spell's cooldown to SPELL_COOLDOWN, and the cooldown ticks
// once per full move, so casting buys a window of up to three of the caster's
// OWN turns in which that spell cannot answer anything. The remaining cooldown
// is exactly the length of that window, measured in the owner's turns. The
// evaluation has no term for it: the resource spent is priced, the window is
// not.
//
// Measured on run5rl before writing it, same protocol as everything else here
// (SpellNNUE::evaluate_scaled, which is what Search::Worker::evaluate returns
// for a legacy spell net, against the depth-10/12 score) on a BARE cooldown —
// the zone has already expired, so only the wait is left. Two positions, cost
// to the owner in displayed cp:
//
//     our freeze cooldown    static           d10           d12
//     cd = 1                 -7.6 / +4.2    -11 /  -56    -80 /  +1
//     cd = 2                -61.2 / -58.3     +8 /  -56   -51 / +85
//     cd = 3                -70.4 / -60.4     -6 / -115  -125 / -57
//
// The net does charge for a cooldown, but as a STEP and not as a ramp: cd=1 is
// nearly free (~5cp) while cd=2 already costs ~60cp. That is the shape of its
// input rather than of the game — spell_nnue.cpp hashes the cooldown BITWISE
// into two planes, so bit 1 (cd in {2,3}) can carry the learned weight and
// bit 0 (cd in {1,3}) almost none. The search rows are one position each and
// far too noisy to fit a curve on, but they never show cd=1 being free.
//
// So the term is a small penalty LINEAR in the remaining cooldown, which is
// the one shape the net demonstrably does not have. Deliberately at the low
// end: the point of the patch is the shape, SPSA sets the size.
extern int SpellCooldownPenalty;  // 38 (~10cp) per remaining cooldown tick

// The window only costs something if the opponent can play into it. Gated on
// the opponent still HOLDING the symmetric spell rather than on can_cast():
// the window lasts up to three of our turns, so what matters is whether they
// have one to spend somewhere inside it, not whether they can cast this very
// ply. A cooldown on a spell we have run out of is not scored at all — there
// is nothing left to wait for.
//
// This knob is the percent of the penalty that survives when the opponent
// cannot answer in kind: 0 keeps the gate, 100 removes it. Left for SPSA
// because the direct measurement of the gate is confounded — emptying the
// opponent's holding to disarm them changes far more than their readiness.
extern int SpellCooldownDisarmedPct;  // 0

}  // namespace Stockfish

#endif  // #ifndef SPELL_PARAMS_H_INCLUDED
