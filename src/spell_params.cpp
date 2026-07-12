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

// Defaults from the first SPSA session (600 self-play pairs at 150k
// nodes/move, 2026-07-12 — see AUDIT.md phase 4.5)
int MaxFreezeGates = 9;
int MaxJumpGates   = 4;

int SpellGateKingBonus     = 11418;
int SpellGateKingRingBonus = 57185;

int SpellDepthPenaltyTactical = 1;
int SpellDepthPenaltyQuiet    = 3;

int SpellTacticalLmrBonus = 1272;

int SpellLmrMoveCountCap = 43;

int SpellGateHistOrderWeight = 1;
int SpellGateHistStatWeight  = 1;

int SpellStageMargin = 400;

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

}  // namespace Stockfish
