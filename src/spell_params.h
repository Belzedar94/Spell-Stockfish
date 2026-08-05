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
// (unlimited while an enemy freeze zone is active).
//
// NOTE: the cap has NO in-check escape hatch. An earlier version of this
// comment claimed "in-check nodes use the full EVASIONS universe anyway";
// that is false for the search. movegen does lift the cap for Type ==
// EVASIONS, but the MovePicker never asks for it: in spell-chess the stage
// machine starts at MAIN_TT/QSEARCH_TT unconditionally (movepick.cpp,
// "no evasion staging — self-check is legal"), so EVASION_INIT/EVASION are
// unreachable from search. In check, quiet gates are capped like anywhere
// else. The reference (Fairy-Stockfish) DOES have the escape hatch
// (urgentPotionDefense); we do not.
//
// NOTE 2: MaxFreezeGates is very nearly a no-op as written. The freeze
// branch raises the cap to the number of gates touching the enemy king ring
// when that count exceeds it, and the ring count is >= 9 whenever an enemy
// king exists, so the effective freeze cap is max(MaxFreezeGates, ringCount).
// Since SpellGateKingRingBonus alone outweighs any non-ring score, the kept
// set is "the ring gates" and little else. Lowering this number does not
// narrow the freeze search; changing the ring bonus does.
extern int MaxFreezeGates;  // 8   (SPSA #2, a130fd74; was 12 in the reference)
extern int MaxJumpGates;    // 4   (SPSA #2, a130fd74; was 6 in the reference)

// Gate impact scoring bonuses
extern int SpellGateKingBonus;      // 11789: zone covers the enemy king
extern int SpellGateKingRingBonus;  // 60993: zone touches the enemy king ring

// Depth penalty (plies) for gated moves: the reference searches spell moves
// shallower (PotionDepthPenaltyTactical/Quiet), which is where a large share
// of its strength comes from.
extern int SpellDepthPenaltyTactical;  // 1
extern int SpellDepthPenaltyQuiet;     // 3   (la referencia usa 2)

// LMR: tactical spells are reduced this much less (millidepth, 1024 = 1 ply)
extern int SpellTacticalLmrBonus;  // 1297

// Cap on the linear moveCount discount inside LMR: the chess-tuned term
// r -= moveCount * 62 runs away at spell move counts (500-3000) and would
// EXTEND late moves without this cap.
extern int SpellLmrMoveCountCap;  // 46

// GateHistory weights in quiet ordering and in the LMR statScore.
// WARNING: SPSA drove BOTH to 0, so gateHistory is written every node and
// multiplied by zero every time it is read. Today gated moves are ordered
// with history fully conflated with their base move. Any rework of the gate
// picker (S-A) has to start here: the table is not being used.
extern int SpellGateHistOrderWeight;  // 0
extern int SpellGateHistStatWeight;   // 0

// Relevance gate for the SPELL stage: a cast is worth at most about a
// tempo plus bounded tactics, so nodes whose static eval sits further
// than this below alpha skip the gated-quiet expansion entirely
// (PV nodes and nodes with our king under attack always search spells).
extern int SpellStageMargin;  // 365  (SPSA #2, a130fd74)

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

}  // namespace Stockfish

#endif  // #ifndef SPELL_PARAMS_H_INCLUDED
