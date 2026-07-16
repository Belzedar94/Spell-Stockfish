/*
  Spell-Stockfish lab: offline feature dump for the staging program
  (docs/spell-staging-program.md). Not for play — feeds the offline
  recall/class-size Pareto analysis. See docs/retroactive-review.md C1.
*/

#ifndef SPELL_LAB_H_INCLUDED
#define SPELL_LAB_H_INCLUDED

#include <ostream>

namespace Stockfish {

class Position;

namespace SpellLab {

// Emits one CSV line per generated spell move of the side to move:
//   uci,sp,gate,royalFreeze,ourKAtt,ourKJmpAtt,ekAttByUs,ekJmpAttByUs,
//   silenced,silencedVal,enemyMatZone,attackedMatZone,jumpGain,logit1000
// Raw values, no thresholds: python explores the AND-matrix offline.
void dump_features(const Position& pos, std::ostream& os);

}  // namespace SpellLab
}  // namespace Stockfish

#endif
