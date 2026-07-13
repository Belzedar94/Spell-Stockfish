/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
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

#ifndef POSITION_H_INCLUDED
#define POSITION_H_INCLUDED

#include <array>
#include <cassert>
#include <deque>
#include <iosfwd>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>

#include "attacks.h"
#include "bitboard.h"
#include "spell.h"
#include "types.h"

namespace Stockfish {

class TranspositionTable;
struct SharedHistories;

// StateInfo struct stores information needed to restore a Position object to
// its previous state when we retract a move. Whenever a move is made on the
// board (by calling Position::do_move), a StateInfo object must be passed.

struct StateInfo {

    // Copied when making a move
    Key    materialKey;
    Key    pawnKey;
    Key    minorPieceKey;
    Key    nonPawnKey[COLOR_NB];
    Value  nonPawnMaterial[COLOR_NB];
    int    castlingRights;
    int    rule50;
    int    pliesFromNull;
    Square epSquare;

    // Spell chess state (see SPELL_SPEC.md): active zone gate squares
    // (SQ_NONE = no zone), cooldowns 0..SPELL_COOLDOWN, spells still in hand.
    // All indexed [color][SpellType] and part of the position key.
    u8 spellGate[COLOR_NB][SPELL_NB];
    i8 spellCooldown[COLOR_NB][SPELL_NB];
    i8 spellHand[COLOR_NB][SPELL_NB];

    // Not copied when making a move (will be recomputed anyhow)
    Key        key;
    Bitboard   checkersBB;
    StateInfo* previous;
    Bitboard   blockersForKing[COLOR_NB];
    Bitboard   pinners[COLOR_NB];
    Bitboard   checkSquares[PIECE_TYPE_NB];
    Piece      capturedPiece;
    int        repetition;

    // Board delta of the move leading to this state, for the spell-NNUE
    // incremental accumulator (spell-state deltas are derived by comparing
    // with the previous state; board changes need explicit ops).
    struct BoardOp {
        u8 add;  // 1 = feature added, 0 = removed
        u8 pc;   // Piece
        u8 sq;   // Square
    };
    BoardOp boardOps[4];
    u8      boardOpCount;  // 0xFF marks "unknown, refresh required" (e.g. after set())

    // Spell-NNUE accumulator cache (lazily computed at evaluation)
    struct SpellAccumulator {
        alignas(64) i16 acc[COLOR_NB][512];
        i32  psqt[COLOR_NB][8];
        u32  gen;  // SpellNNUE::net_generation() this entry was built with
        bool computed[COLOR_NB];
    };
    SpellAccumulator spellAcc;
};


// A list to keep track of the position states along the setup moves (from the
// start position to the position just before the search starts). Needed by
// 'draw by repetition' detection. Use a std::deque because pointers to
// elements are not invalidated upon list resizing.
using StateListPtr = std::unique_ptr<std::deque<StateInfo>>;

// This error should be used whenever a position is suspected to be unsupported
// by the engine. In particular positions that may cause hard errors like segmentation fault.
struct PositionSetError: std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Position class stores information regarding the board representation as
// pieces, side to move, hash keys, castling info, etc. Important methods are
// do_move() and undo_move(), used by the search to update node info when
// traversing the search tree.
namespace Zobrist {
// Pillar B (cast decomposition): overlay distinguishing a pending-cast
// half-node from its parent position in keys/TT
extern Key pendingCast[SPELL_NB][SQUARE_NB];
}

class Position {
   public:
    static void init();

    Position()                           = default;
    Position(const Position&)            = delete;
    Position& operator=(const Position&) = delete;

    // FEN string input/output
    std::optional<PositionSetError> set(const std::string& fenStr, bool isChess960, StateInfo* si);
    std::optional<PositionSetError> set(const std::string& code, Color c, StateInfo* si);
    std::string                     fen() const;

    // Position representation
    Bitboard pieces() const;  // All pieces
    template<typename... PieceTypes>
    Bitboard pieces(PieceTypes... pts) const;
    Bitboard pieces(Color c) const;
    template<typename... PieceTypes>
    Bitboard                            pieces(Color c, PieceTypes... pts) const;
    Piece                               piece_on(Square s) const;
    const std::array<Piece, SQUARE_NB>& piece_array() const;
    Square                              ep_square() const;
    bool                                empty(Square s) const;
    template<PieceType Pt>
    int count(Color c) const;
    template<PieceType Pt>
    int count() const;
    template<PieceType Pt>
    Square square(Color c) const;

    // Castling
    bool   can_castle(CastlingRights cr) const;
    bool   castling_impeded(CastlingRights cr) const;
    Square castling_rook_square(CastlingRights cr) const;

    // Checking
    Bitboard checkers() const;
    Bitboard blockers_for_king(Color c) const;
    Bitboard check_squares(PieceType pt) const;
    Bitboard pinners(Color c) const;

    // Attacks to/from a given square
    Bitboard attackers_to(Square s) const;
    Bitboard attackers_to(Square s, Bitboard occupied) const;
    bool     attackers_to_exist(Square s, Bitboard occupied, Color c) const;
    void     update_slider_blockers(Color c) const;
    template<PieceType Pt>
    Bitboard attacks_by(Color c) const;

    // Properties of moves
    bool  legal(Move m) const;
    bool  pseudo_legal(const Move m) const;
    bool  capture(Move m) const;
    bool  capture_stage(Move m) const;
    bool  gives_check(Move m) const;
    Piece moved_piece(Move m) const;
    Piece captured_piece() const;

    // Doing and undoing moves
    void do_move(Move m, StateInfo& newSt, const TranspositionTable* tt);
    void do_move(Move                      m,
                 StateInfo&                newSt,
                 bool                      givesCheck,
                 DirtyPiece&               dp,
                 DirtyThreats&             dts,
                 const TranspositionTable* tt,
                 const SharedHistories*    worker);
    void undo_move(Move m);
    void do_null_move(StateInfo& newSt);
    void undo_null_move();

    // Static Exchange Evaluation
    bool see_ge(Move m, int threshold = 0) const;

    // Spell chess accessors
    Square   spell_gate(Color c, SpellType sp) const;
    Bitboard spell_zone(Color c, SpellType sp) const;
    int      spell_cooldown(Color c, SpellType sp) const;
    int      spells_in_hand(Color c, SpellType sp) const;
    bool     can_cast(Color c, SpellType sp) const;
    Bitboard frozen_squares(Color c) const;  // squares from which c's pieces cannot move
    Bitboard frozen_pieces() const;          // pieces of either color that are frozen
    Bitboard jump_transparent() const;       // active jump gates (transparent for sliding)
    Bitboard occupied_for_sliding() const;   // pieces() minus transparent gates
    bool     both_kings_on_board() const;

    // Pillar B (cast decomposition): a declared-but-uncompleted cast. Pure
    // bookkeeping — no board/spell state is applied until the completing
    // move runs the CLASSIC do_move path with the composed gated move, so
    // "cast then move" equals "gated move" by construction. Rules-level
    // consumers (movegen, FEN, repetition) never see the declaration.
    bool      has_pending_cast() const { return pendingSpell != int(SPELL_NB); }
    SpellType pending_spell() const { return SpellType(pendingSpell); }
    Square    pending_gate() const { return Square(pendingGate); }
    void      do_cast(SpellType sp, Square gate);
    void      undo_cast();
    Move      compose_pending(Move base) const;

    // Accessing hash keys
    Key key() const;
    Key prefetch_key(Move m) const;
    Key material_key() const;
    Key pawn_key() const;
    Key minor_piece_key() const;
    Key non_pawn_key(Color c) const;

    // Other properties of the position
    Color side_to_move() const;
    int   game_ply() const;
    bool  is_chess960() const;
    bool  is_draw(int ply) const;
    bool  is_repetition(int ply) const;
    bool  upcoming_repetition(int ply) const;
    bool  has_repeated() const;
    int   rule50_count() const;
    Value non_pawn_material(Color c) const;
    Value non_pawn_material() const;
    bool  dtz_is_dtm() const;  // Pawnless && (3-men || 4-men-minors-only)

    // Position consistency check, for debugging
    bool                            pos_is_ok() const;
    bool                            material_key_is_ok() const;
    std::optional<PositionSetError> flip();

    StateInfo* state() const;

    void put_piece(Piece pc, Square s, DirtyThreats* const dts = nullptr);
    void remove_piece(Square s, DirtyThreats* const dts = nullptr);
    void swap_piece(Square s, Piece pc, DirtyThreats* const dts = nullptr);

   private:
    // Initialization helpers (used while setting up a position)
    void set_castling_right(Color c, Square rfrom);
    Key  compute_material_key() const;
    void set_state() const;
    void set_check_info() const;

    // Other helpers
    template<bool ComputeRay = true>
    void update_piece_threats(Piece               pc,
                              bool                putPiece,
                              Square              s,
                              DirtyThreats* const dts,
                              Bitboard            noRaysContaining = -1ULL) const;
    void move_piece(Square from, Square to, DirtyThreats* const dts = nullptr);
    template<bool Do>
    void do_castling(Color               us,
                     Square              from,
                     Square&             to,
                     Square&             rfrom,
                     Square&             rto,
                     DirtyThreats* const dts = nullptr,
                     DirtyPiece* const   dp  = nullptr);
    template<bool AfterMove = false>
    Key adjust_key50(Key k) const;

    // Data members
    std::array<Piece, SQUARE_NB>        board;
    std::array<Bitboard, PIECE_TYPE_NB> byTypeBB;
    std::array<Bitboard, COLOR_NB>      byColorBB;

    int          pieceCount[PIECE_NB];
    int          castlingRightsMask[SQUARE_NB];
    Square       castlingRookSquare[CASTLING_RIGHT_NB];
    Bitboard     castlingPath[CASTLING_RIGHT_NB];
    StateInfo*   st;
    int          gamePly;
    Color        sideToMove;
    bool         chess960;
    // Pillar B: declared pending cast (SPELL_NB = none); see do_cast()
    int          pendingSpell = int(SPELL_NB);
    int          pendingGate  = int(SQ_NONE);
    DirtyPiece   scratch_dp;
    DirtyThreats scratch_dts;
};

std::ostream& operator<<(std::ostream& os, const Position& pos);

inline Color Position::side_to_move() const { return sideToMove; }

inline Piece Position::piece_on(Square s) const {
    assert(is_ok(s));
    return board[s];
}

inline const std::array<Piece, SQUARE_NB>& Position::piece_array() const { return board; }

inline bool Position::empty(Square s) const { return piece_on(s) == NO_PIECE; }

inline Piece Position::moved_piece(Move m) const { return piece_on(m.from_sq()); }

inline Bitboard Position::pieces() const { return byTypeBB[ALL_PIECES]; }

template<typename... PieceTypes>
inline Bitboard Position::pieces(PieceTypes... pts) const {
    return (byTypeBB[pts] | ...);
}

inline Bitboard Position::pieces(Color c) const { return byColorBB[c]; }

template<typename... PieceTypes>
inline Bitboard Position::pieces(Color c, PieceTypes... pts) const {
    return pieces(c) & pieces(pts...);
}

template<PieceType Pt>
inline int Position::count(Color c) const {
    return pieceCount[make_piece(c, Pt)];
}

template<PieceType Pt>
inline int Position::count() const {
    return count<Pt>(WHITE) + count<Pt>(BLACK);
}

template<PieceType Pt>
inline Square Position::square(Color c) const {
    assert(count<Pt>(c) == 1);
    return lsb(pieces(c, Pt));
}

inline Square Position::ep_square() const { return st->epSquare; }

inline Square Position::spell_gate(Color c, SpellType sp) const {
    return Square(st->spellGate[c][sp]);
}

inline Bitboard Position::spell_zone(Color c, SpellType sp) const {
    return spell_zone_bb(sp, spell_gate(c, sp));
}

inline int Position::spell_cooldown(Color c, SpellType sp) const {
    return st->spellCooldown[c][sp];
}

inline int Position::spells_in_hand(Color c, SpellType sp) const { return st->spellHand[c][sp]; }

inline bool Position::can_cast(Color c, SpellType sp) const {
    return st->spellHand[c][sp] > 0 && st->spellCooldown[c][sp] == 0;
}

// A freeze zone cast by ~c restricts c's pieces (origin squares) while active
inline Bitboard Position::frozen_squares(Color c) const { return spell_zone(~c, SPELL_FREEZE); }

// Frozen pieces of either color: they cannot move and give no attacks
inline Bitboard Position::frozen_pieces() const {
    return (pieces(WHITE) & frozen_squares(WHITE)) | (pieces(BLACK) & frozen_squares(BLACK));
}

inline Bitboard Position::jump_transparent() const {
    return spell_zone(WHITE, SPELL_JUMP) | spell_zone(BLACK, SPELL_JUMP);
}

inline Bitboard Position::occupied_for_sliding() const { return pieces() & ~jump_transparent(); }

inline bool Position::both_kings_on_board() const {
    return count<KING>(WHITE) == 1 && count<KING>(BLACK) == 1;
}

inline bool Position::can_castle(CastlingRights cr) const { return st->castlingRights & cr; }

inline bool Position::castling_impeded(CastlingRights cr) const {
    assert(cr == WHITE_OO || cr == WHITE_OOO || cr == BLACK_OO || cr == BLACK_OOO);
    return pieces() & castlingPath[cr];
}

inline Square Position::castling_rook_square(CastlingRights cr) const {
    assert(cr == WHITE_OO || cr == WHITE_OOO || cr == BLACK_OO || cr == BLACK_OOO);
    return castlingRookSquare[cr];
}

inline Bitboard Position::attackers_to(Square s) const { return attackers_to(s, pieces()); }

template<PieceType Pt>
inline Bitboard Position::attacks_by(Color c) const {

    if constexpr (Pt == PAWN)
        return c == WHITE ? pawn_attacks_bb<WHITE>(pieces(WHITE, PAWN))
                          : pawn_attacks_bb<BLACK>(pieces(BLACK, PAWN));
    else
    {
        Bitboard threats   = 0;
        Bitboard attackers = pieces(c, Pt);
        while (attackers)
            threats |= Attacks::attacks_bb<Pt>(pop_lsb(attackers), pieces());
        return threats;
    }
}

inline Bitboard Position::checkers() const { return st->checkersBB; }

inline Bitboard Position::blockers_for_king(Color c) const { return st->blockersForKing[c]; }

inline Bitboard Position::pinners(Color c) const { return st->pinners[c]; }

inline Bitboard Position::check_squares(PieceType pt) const { return st->checkSquares[pt]; }

inline Key Position::key() const {
    return adjust_key50(st->key)
         ^ (has_pending_cast() ? Zobrist::pendingCast[pendingSpell][pendingGate] : 0);
}

template<bool AfterMove>
inline Key Position::adjust_key50(Key k) const {
    return st->rule50 < 14 - AfterMove ? k : k ^ make_key((st->rule50 - (14 - AfterMove)) / 8);
}

inline Key Position::pawn_key() const { return st->pawnKey; }

inline Key Position::material_key() const { return st->materialKey; }

inline Key Position::minor_piece_key() const { return st->minorPieceKey; }

inline Key Position::non_pawn_key(Color c) const { return st->nonPawnKey[c]; }

inline Value Position::non_pawn_material(Color c) const { return st->nonPawnMaterial[c]; }

inline Value Position::non_pawn_material() const {
    return non_pawn_material(WHITE) + non_pawn_material(BLACK);
}

inline int Position::game_ply() const { return gamePly; }

inline int Position::rule50_count() const { return st->rule50; }

inline bool Position::is_chess960() const { return chess960; }

inline bool Position::dtz_is_dtm() const {
    return !count<PAWN>()
        && (count<ALL_PIECES>() == 3 || (count<ALL_PIECES>() == 4 && !pieces(QUEEN, ROOK)));
}

inline bool Position::capture(Move m) const {
    assert(m.is_ok());

    const MoveType mt = m.type_of();

    if (mt == NORMAL || mt == PROMOTION)
        return !empty(m.to_sq());

    return mt == EN_PASSANT;
}

// Returns true if a move is generated from the capture stage, having also
// queen promotions covered, i.e. consistency with the capture stage move
// generation is needed to avoid the generation of duplicate moves.
inline bool Position::capture_stage(Move m) const {
    assert(m.is_ok());

    const MoveType mt = m.type_of();

    if (mt == NORMAL)
        return !empty(m.to_sq());

    if (mt == PROMOTION)
        return !empty(m.to_sq()) || m.promotion_type() == QUEEN;

    return mt == EN_PASSANT;
}

inline Piece Position::captured_piece() const { return st->capturedPiece; }

inline void Position::put_piece(Piece pc, Square s, DirtyThreats* const dts) {
    board[s] = pc;
    byTypeBB[ALL_PIECES] |= byTypeBB[type_of(pc)] |= s;
    byColorBB[color_of(pc)] |= s;
    pieceCount[pc]++;
    pieceCount[make_piece(color_of(pc), ALL_PIECES)]++;

    if (dts)
        update_piece_threats(pc, true, s, dts);
}

inline void Position::remove_piece(Square s, DirtyThreats* const dts) {
    Piece pc = board[s];

    if (dts)
        update_piece_threats(pc, false, s, dts);

    byTypeBB[ALL_PIECES] ^= s;
    byTypeBB[type_of(pc)] ^= s;
    byColorBB[color_of(pc)] ^= s;
    board[s] = NO_PIECE;
    pieceCount[pc]--;
    pieceCount[make_piece(color_of(pc), ALL_PIECES)]--;
}

inline void Position::move_piece(Square from, Square to, DirtyThreats* const dts) {
    Piece    pc     = board[from];
    Bitboard fromTo = from | to;

    if (dts)
        update_piece_threats(pc, false, from, dts, fromTo);

    byTypeBB[ALL_PIECES] ^= fromTo;
    byTypeBB[type_of(pc)] ^= fromTo;
    byColorBB[color_of(pc)] ^= fromTo;
    board[from] = NO_PIECE;
    board[to]   = pc;

    if (dts)
        update_piece_threats(pc, true, to, dts, fromTo);
}

inline void Position::swap_piece(Square s, Piece pc, DirtyThreats* const dts) {
    Piece old = board[s];

    remove_piece(s);

    if (dts)
        update_piece_threats<false>(old, false, s, dts);

    put_piece(pc, s);

    if (dts)
        update_piece_threats<false>(pc, true, s, dts);
}

inline void Position::do_move(Move m, StateInfo& newSt, const TranspositionTable* tt = nullptr) {
    new (&scratch_dts) DirtyThreats;
    do_move(m, newSt, gives_check(m), scratch_dp, scratch_dts, tt, nullptr);
}

inline StateInfo* Position::state() const { return st; }

}  // namespace Stockfish

#endif  // #ifndef POSITION_H_INCLUDED
