/*
  Spell-Stockfish lab: offline feature dump (see spell_lab.h).
*/

#include "spell_lab.h"

#include <ostream>

#include "movegen.h"
#include "position.h"
#include "spell.h"
#include "spell_order.h"
#include "spellpolicy/spell_policy.h"
#include "uci.h"

namespace Stockfish::SpellLab {

void dump_features(const Position& pos, std::ostream& os) {

    const Color us   = pos.side_to_move();
    const Color them = ~us;

    const Square ourRoyal   = pos.count<KING>(us) ? pos.square<KING>(us) : SQ_NONE;
    const Square enemyRoyal = pos.count<KING>(them) ? pos.square<KING>(them) : SQ_NONE;

    const Bitboard ourKAtt = ourRoyal != SQ_NONE ? pos.attackers_to(ourRoyal) & pos.pieces(them) : 0;
    const bool     ourKJmpAtt =
      ourRoyal != SQ_NONE && pos.can_cast(them, SPELL_JUMP) && spell_jump_snipers(pos, us);
    const bool ekAttByUs =
      enemyRoyal != SQ_NONE && (pos.attackers_to(enemyRoyal) & pos.pieces(us));
    const bool ekJmpAttByUs =
      enemyRoyal != SQ_NONE && pos.can_cast(us, SPELL_JUMP) && spell_jump_snipers(pos, them);

    int jumpGain[SQUARE_NB];
    jump_gate_scores(pos, us, enemyRoyal, jumpGain);

    Move list[MAX_MOVES];
    Move* end = generate<SPELL_QUIETS>(pos, list);

    for (Move* it = list; it != end; ++it)
    {
        const Move m = *it;
        if (!m.is_spell())
            continue;

        const SpellType sp   = m.spell_type();
        const Square    gate = m.gate_sq();

        bool royalFreeze = false;
        int  silencedVal = 0, enemyMatZone = 0, attackedMatZone = 0;

        if (sp == SPELL_FREEZE)
        {
            const Bitboard zone = FreezeZoneBB[gate];
            royalFreeze = enemyRoyal != SQ_NONE && (zone & square_bb(enemyRoyal));

            for (Bitboard b = zone & ourKAtt; b;)
                silencedVal = std::max(silencedVal, int(PieceValue[pos.piece_on(pop_lsb(b))]));

            for (Bitboard b = zone & pos.pieces(them); b;)
            {
                const Square s = pop_lsb(b);
                const int    v = PieceValue[pos.piece_on(s)];
                enemyMatZone += v;
                if (pos.attackers_to(s) & pos.pieces(us))
                    attackedMatZone += v;
            }
        }

        const int gain = sp == SPELL_JUMP ? jumpGain[gate] : 0;
        const int logit1000 =
          int(1000.0f * SpellPolicy::gate_logit(pos, us, sp, gate));
        const bool v1 = is_tactical_spell(pos, m, ourKAtt, enemyRoyal, ourRoyal);

        os << UCIEngine::move(m, pos.is_chess960()) << ';' << (sp == SPELL_FREEZE ? 'F' : 'J')
           << ';' << int(gate) << ';' << int(royalFreeze) << ';' << int(bool(ourKAtt)) << ';'
           << int(ourKJmpAtt) << ';' << int(ekAttByUs) << ';' << int(ekJmpAttByUs) << ';'
           << silencedVal << ';' << enemyMatZone << ';' << attackedMatZone << ';' << gain << ';'
           << logit1000 << ';' << int(v1) << '\n';
    }

    os << "done\n";
}

}  // namespace Stockfish::SpellLab
