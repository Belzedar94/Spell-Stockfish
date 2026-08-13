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

#include "spell_params.h"

#include "types.h"

namespace Stockfish {

// Defaults from the second SPSA session — the control tower's first tune
// (test #2: 1200 iterations x 8 pairs = 19,200 games at 2.0+0.02,
// 2026-07-13; see AUDIT.md). Notable: both GateHistory weights converged
// to 0 — the learned gate ordering reads as noise at VSTC.
int MaxFreezeGates = 8;
int MaxJumpGates   = 4;

int SpellGateKingBonus     = 11789;
int SpellGateKingRingBonus = 60993;

int SpellDepthPenaltyTactical = 1;
int SpellDepthPenaltyQuiet    = 3;

int SpellTacticalLmrBonus = 1297;

int SpellLmrMoveCountCap = 46;

int SpellGateHistOrderWeight = 0;
int SpellGateHistStatWeight  = 0;

int SpellStageMargin = 365;

int SpellQuietMinDepth = 0;

// SPRT candidate toggles — defaults are behavior-preserving (see header)
int SpellMergedOrdering       = 0;
int SpellNullMoveGuard        = 0;
int SpellLmpScalePct          = 100;
int SpellFutilityScalePct     = 100;
int SpellNoPenaltyPV          = 0;
int SpellAspirationPct        = 100;
int SpellCaptureSeeMargin     = 0;
int SpellNoIIR                = 0;
int SpellContHistSkip         = 0;
int SpellRazorGuard           = 0;
int SpellFreezeGateEffectOnly = 0;
int SpellDominateCaptures     = 1;

// SPSA exposure: every spell search-policy knob becomes a UCI option.
//
// The three gate knobs carry ranges re-cut for the post-domination-filter
// campaign (tools/redundancy_audit.py over 820 positions: the official
// opening book, plus middlegames and endgames derived from it; see
// docs/redundancy-audit.md). Defaults are untouched, so this is an
// options-diff test: same bench, same play until a value is set.
//
//   MaxFreezeGates 2..32 -> 2..20. Since dominant_freeze_gates() runs first,
//   the slots now buy DISTINCT freeze effects, and the audit never found more
//   than 16 of those in a position (mean 9.2 in middlegames, 7.5 in
//   endgames). Above ~16 the extra slots can only be spent on gates that
//   freeze nothing, which is strictly worse than not spending them; 20 keeps
//   a margin over every position sampled. The narrower range also cuts c_end
//   from 1.5 to 0.9, i.e. a perturbation of about one gate, which is the
//   granularity the parameter actually has. Room upward matters: at the
//   current 8 the cut drops a distinct useful effect in 60% of middlegame
//   positions, so "raise it" is the live hypothesis.
//
//   SpellGateKingBonus 1000.. -> 0.., SpellGateKingRingBonus 5000.. -> 0..
//   Both floors are arbitrary and both sit above the hypothesis the audit
//   raises. freeze_gate_score adds the ring bonus on zone-and-king-ring
//   overlap whether or not anything stands there, and at 60993 that is 24x a
//   queen (2538) -- so a gate that freezes NOTHING can outscore one that
//   freezes a queen. Measured consequence: 26% of the freeze slots in
//   middlegames and 70% in endgames go to gates that freeze nothing. Since
//   cc78b0a1 those gates no longer build the gated moves that were then
//   thrown away one by one, so what is left of the waste is exactly the
//   budget slot — which is the thing this range has to be able to price. A
//   floor of 5000 is already 2x a queen, so SPSA cannot currently express
//   "this bonus should be small". Note the knob only reorders: the ringCount
//   override in movegen.cpp is geometric and admits those gates regardless of
//   the bonus, and keeping them out of the budget altogether is what the
//   default-off SpellFreezeGateEffectOnly does.
TUNE(SetRange(2, 20), MaxFreezeGates);
TUNE(SetRange(1, 20), MaxJumpGates);
TUNE(SetRange(0, 30000), SpellGateKingBonus);
TUNE(SetRange(0, 120000), SpellGateKingRingBonus);
TUNE(SetRange(0, 3), SpellDepthPenaltyTactical);
TUNE(SetRange(0, 4), SpellDepthPenaltyQuiet);
TUNE(SetRange(0, 3072), SpellTacticalLmrBonus);
TUNE(SetRange(4, 96), SpellLmrMoveCountCap);
TUNE(SetRange(0, 8), SpellGateHistOrderWeight);
TUNE(SetRange(0, 8), SpellGateHistStatWeight);
TUNE(SetRange(0, 2000), SpellStageMargin);
TUNE(SetRange(0, 8), SpellQuietMinDepth);
TUNE(SetRange(0, 1), SpellMergedOrdering);
TUNE(SetRange(0, 1), SpellNullMoveGuard);
TUNE(SetRange(25, 400), SpellLmpScalePct);
TUNE(SetRange(25, 400), SpellFutilityScalePct);
TUNE(SetRange(0, 1), SpellNoPenaltyPV);
TUNE(SetRange(25, 400), SpellAspirationPct);
TUNE(SetRange(0, 500), SpellCaptureSeeMargin);
TUNE(SetRange(0, 1), SpellNoIIR);
TUNE(SetRange(0, 1), SpellContHistSkip);
TUNE(SetRange(0, 1), SpellRazorGuard);
TUNE(SetRange(0, 1), SpellFreezeGateEffectOnly);
TUNE(SetRange(0, 1), SpellDominateCaptures);

}  // namespace Stockfish
