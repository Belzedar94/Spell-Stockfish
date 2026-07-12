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

}  // namespace Stockfish

#endif  // #ifndef SPELL_PARAMS_H_INCLUDED
