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
int SpellMergedOrdering   = 0;
int SpellNullMoveGuard    = 0;
int SpellLmpScalePct      = 100;
int SpellFutilityScalePct = 100;
int SpellNoPenaltyPV      = 0;
int SpellAspirationPct    = 100;
int SpellCaptureSeeMargin = 0;
int SpellJumpSeeCost      = 0;
int SpellNoIIR            = 0;
int SpellContHistSkip     = 0;
int SpellRazorGuard       = 0;

// SPSA exposure: every spell search-policy knob becomes a UCI option
TUNE(SetRange(2, 32), MaxFreezeGates);
TUNE(SetRange(1, 20), MaxJumpGates);
TUNE(SetRange(1000, 30000), SpellGateKingBonus);
TUNE(SetRange(5000, 120000), SpellGateKingRingBonus);
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
TUNE(SetRange(0, 600), SpellJumpSeeCost);
TUNE(SetRange(0, 1), SpellNoIIR);
TUNE(SetRange(0, 1), SpellContHistSkip);
TUNE(SetRange(0, 1), SpellRazorGuard);

}  // namespace Stockfish
