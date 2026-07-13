/*
  pyffish_spell: Python bindings for the Spell-Stockfish rules layer.

  Drop-in replica of the pyffish surface (Fairy-Stockfish, based on
  Jean-Francois Romang's pyfish) restricted to the single variant
  "spell-chess", minus SAN (phase 2 — not spell-aware even upstream),
  plus a spell_state() extension. Stateless like the reference: every
  call rebuilds the position from (variant, fen, moveList).

  Builds against the rules-only closure (SPELL_RULES_ONLY): position,
  movegen, bitboard, attacks, notation, misc, memory, spell_params,
  tune, ucioption — no threads, no TT, no search, no NNUE runtime.

  Documented deviations from upstream pyffish (fixtures cover them):
  - variants() == ["spell-chess"]; two_boards/captures_to_hand False.
  - set_option/load_variant_config accept anything and do nothing
    (there is no engine behind the bindings).
  - get_fen ignores sfen/showPromoted/countStarted (not applicable).
  - game_result follows capture-the-king semantics (mirrors the CECP
    adapter): side to move without king or stalled while in check
    loses; quiet stall is a draw.
  - has_insufficient_material always (False, False): extinction chess
    has no safe insufficiency theory (a lone king still captures).
  - get_san/get_san_moves/get_fog_fen are absent.
*/

#include <Python.h>

#include <deque>
#include <memory>
#include <sstream>
#include <string>

#include "attacks.h"
#include "bitboard.h"
#include "movegen.h"
#include "notation.h"
#include "position.h"
#include "types.h"

using namespace Stockfish;

namespace {

constexpr const char* VariantName = "spell-chess";

bool check_variant(const char* variant) {
    if (std::string(variant) == VariantName)
        return true;
    PyErr_SetString(PyExc_ValueError,
                    (std::string("Unsupported variant '") + variant + "' (only spell-chess)")
                      .c_str());
    return false;
}

using StateListPtr = std::unique_ptr<std::deque<StateInfo>>;

// Rebuilds pos from (fen, moveList). Returns false with a Python error
// set on invalid variant, FEN or move.
bool build_position(Position&    pos,
                    StateListPtr& states,
                    const char*  variant,
                    const char*  fen,
                    PyObject*    moveList,
                    bool         chess960) {
    if (!check_variant(variant))
        return false;

    states = StateListPtr(new std::deque<StateInfo>(1));

    std::string fenStr = std::string(fen) == "startpos" ? StartFEN : fen;
    if (auto err = pos.set(fenStr, chess960, &states->back()))
    {
        PyErr_SetString(PyExc_ValueError,
                        (std::string("Invalid FEN '") + fenStr + "': " + err->what()).c_str());
        return false;
    }

    const Py_ssize_t numMoves = PyList_Size(moveList);
    for (Py_ssize_t i = 0; i < numMoves; i++)
    {
        PyObject* enc = PyUnicode_AsEncodedString(PyList_GetItem(moveList, i), "UTF-8", "strict");
        if (!enc)
            return false;
        std::string moveStr(PyBytes_AS_STRING(enc));
        Py_XDECREF(enc);

        const Move m = Notation::to_move(pos, moveStr);
        if (m == Move::none())
        {
            PyErr_SetString(PyExc_ValueError,
                            (std::string("Invalid move '") + moveStr + "'").c_str());
            return false;
        }
        states->emplace_back();
        pos.do_move(m, states->back());
    }
    return true;
}

// Spell-chess game result from the side to move's point of view
// (capture-the-king semantics, mirrored from the CECP adapter).
Value spell_result(Position& pos) {
    const Color us = pos.side_to_move();
    if (!pos.count<KING>(us))
        return -VALUE_MATE;
    if (!pos.count<KING>(~us))
        return VALUE_MATE;
    if (MoveList<LEGAL>(pos).size() == 0)
        return pos.checkers() ? -VALUE_MATE : VALUE_DRAW;
    return VALUE_NONE;
}

}  // namespace

extern "C" PyObject* pyffish_version(PyObject*) { return Py_BuildValue("(iii)", 0, 1, 0); }

extern "C" PyObject* pyffish_info(PyObject*) {
    return Py_BuildValue("s", engine_info().c_str());
}

extern "C" PyObject* pyffish_variants(PyObject*, PyObject*) {
    return Py_BuildValue("[s]", VariantName);
}

// Accepted for pyffish drop-in compatibility; there is no engine behind
// the bindings, so options have nothing to configure.
extern "C" PyObject* pyffish_setOption(PyObject*, PyObject* args) {
    const char* name;
    PyObject*   valueObj;
    if (!PyArg_ParseTuple(args, "sO", &name, &valueObj))
        return NULL;
    Py_RETURN_NONE;
}

extern "C" PyObject* pyffish_loadVariantConfig(PyObject*, PyObject* args) {
    const char* config;
    if (!PyArg_ParseTuple(args, "s", &config))
        return NULL;
    Py_RETURN_NONE;
}

extern "C" PyObject* pyffish_startFen(PyObject*, PyObject* args) {
    const char* variant;
    if (!PyArg_ParseTuple(args, "s", &variant))
        return NULL;
    if (!check_variant(variant))
        return NULL;
    return Py_BuildValue("s", StartFEN);
}

extern "C" PyObject* pyffish_twoBoards(PyObject*, PyObject* args) {
    const char* variant;
    if (!PyArg_ParseTuple(args, "s", &variant))
        return NULL;
    if (!check_variant(variant))
        return NULL;
    Py_RETURN_FALSE;
}

extern "C" PyObject* pyffish_capturesToHand(PyObject*, PyObject* args) {
    const char* variant;
    if (!PyArg_ParseTuple(args, "s", &variant))
        return NULL;
    if (!check_variant(variant))
        return NULL;
    Py_RETURN_FALSE;
}

// INPUT variant, fen, move list
extern "C" PyObject* pyffish_legalMoves(PyObject*, PyObject* args) {
    PyObject*   moveList;
    const char *fen, *variant;
    int         chess960 = false;
    if (!PyArg_ParseTuple(args, "ssO!|p", &variant, &fen, &PyList_Type, &moveList, &chess960))
        return NULL;

    Position     pos;
    StateListPtr states;
    if (!build_position(pos, states, variant, fen, moveList, chess960))
        return NULL;

    PyObject* legalMoves = PyList_New(0);
    for (const auto& m : MoveList<LEGAL>(pos))
    {
        PyObject* moveStr = Py_BuildValue("s", Notation::move(m, pos.is_chess960()).c_str());
        PyList_Append(legalMoves, moveStr);
        Py_XDECREF(moveStr);
    }
    return legalMoves;
}

// INPUT variant, fen, move list
extern "C" PyObject* pyffish_getFEN(PyObject*, PyObject* args) {
    PyObject*   moveList;
    const char *fen, *variant;
    int chess960 = false, sfen = false, showPromoted = false, countStarted = 0;
    if (!PyArg_ParseTuple(args, "ssO!|pppi", &variant, &fen, &PyList_Type, &moveList, &chess960,
                          &sfen, &showPromoted, &countStarted))
        return NULL;

    Position     pos;
    StateListPtr states;
    if (!build_position(pos, states, variant, fen, moveList, chess960))
        return NULL;
    return Py_BuildValue("s", pos.fen().c_str());
}

// INPUT variant, fen, move list
extern "C" PyObject* pyffish_givesCheck(PyObject*, PyObject* args) {
    PyObject*   moveList;
    const char *fen, *variant;
    int         chess960 = false;
    if (!PyArg_ParseTuple(args, "ssO!|p", &variant, &fen, &PyList_Type, &moveList, &chess960))
        return NULL;

    Position     pos;
    StateListPtr states;
    if (!build_position(pos, states, variant, fen, moveList, chess960))
        return NULL;
    return PyBool_FromLong(pos.checkers() != 0);
}

// INPUT variant, fen, move list, move
extern "C" PyObject* pyffish_isCapture(PyObject*, PyObject* args) {
    PyObject*   moveList;
    const char *variant, *fen, *move;
    int         chess960 = false;
    if (!PyArg_ParseTuple(args, "ssO!s|p", &variant, &fen, &PyList_Type, &moveList, &move,
                          &chess960))
        return NULL;

    Position     pos;
    StateListPtr states;
    if (!build_position(pos, states, variant, fen, moveList, chess960))
        return NULL;

    const Move m = Notation::to_move(pos, move);
    if (m == Move::none())
    {
        PyErr_SetString(PyExc_ValueError, (std::string("Invalid move '") + move + "'").c_str());
        return NULL;
    }
    return PyBool_FromLong(pos.capture(m));
}

// INPUT variant, fen, move list
extern "C" PyObject* pyffish_pieceToPartner(PyObject*, PyObject* args) {
    PyObject*   moveList;
    const char *fen, *variant;
    int         chess960 = false;
    if (!PyArg_ParseTuple(args, "ssO!|p", &variant, &fen, &PyList_Type, &moveList, &chess960))
        return NULL;
    if (!check_variant(variant))
        return NULL;
    return Py_BuildValue("s", "");
}

// INPUT variant, fen, move list
extern "C" PyObject* pyffish_gameResult(PyObject*, PyObject* args) {
    PyObject*   moveList;
    const char *fen, *variant;
    int         chess960 = false;
    if (!PyArg_ParseTuple(args, "ssO!|p", &variant, &fen, &PyList_Type, &moveList, &chess960))
        return NULL;

    Position     pos;
    StateListPtr states;
    if (!build_position(pos, states, variant, fen, moveList, chess960))
        return NULL;
    return Py_BuildValue("i", spell_result(pos));
}

// INPUT variant, fen, move list
extern "C" PyObject* pyffish_isImmediateGameEnd(PyObject*, PyObject* args) {
    PyObject*   moveList;
    const char *fen, *variant;
    int         chess960 = false;
    if (!PyArg_ParseTuple(args, "ssO!|p", &variant, &fen, &PyList_Type, &moveList, &chess960))
        return NULL;

    Position     pos;
    StateListPtr states;
    if (!build_position(pos, states, variant, fen, moveList, chess960))
        return NULL;

    // Immediate end = a king is gone (extinction); stalls need the move
    // list and are reported by game_result instead.
    const bool over =
      !pos.count<KING>(WHITE) || !pos.count<KING>(BLACK);
    return Py_BuildValue("(Oi)", over ? Py_True : Py_False,
                         over ? int(spell_result(pos)) : int(VALUE_NONE));
}

// INPUT variant, fen, move list
extern "C" PyObject* pyffish_isOptionalGameEnd(PyObject*, PyObject* args) {
    PyObject*   moveList;
    const char *fen, *variant;
    int         chess960 = false, countStarted = 0;
    if (!PyArg_ParseTuple(args, "ssO!|pi", &variant, &fen, &PyList_Type, &moveList, &chess960,
                          &countStarted))
        return NULL;

    Position     pos;
    StateListPtr states;
    if (!build_position(pos, states, variant, fen, moveList, chess960))
        return NULL;

    // Claimable draws: 50-move rule or repetition (spell chess keeps both)
    const bool claim = pos.is_draw(pos.game_ply());
    return Py_BuildValue("(Oi)", claim ? Py_True : Py_False, int(VALUE_DRAW));
}

// INPUT variant, fen, move list
extern "C" PyObject* pyffish_hasInsufficientMaterial(PyObject*, PyObject* args) {
    PyObject*   moveList;
    const char *fen, *variant;
    int         chess960 = false;
    if (!PyArg_ParseTuple(args, "ssO!|p", &variant, &fen, &PyList_Type, &moveList, &chess960))
        return NULL;
    if (!check_variant(variant))
        return NULL;
    return Py_BuildValue("(OO)", Py_False, Py_False);
}

// INPUT fen, variant (inverted order, faithful to upstream pyffish)
extern "C" PyObject* pyffish_validateFen(PyObject*, PyObject* args) {
    const char *fen, *variant;
    int         chess960 = false;
    if (!PyArg_ParseTuple(args, "ss|p", &fen, &variant, &chess960))
        return NULL;
    if (!check_variant(variant))
        return NULL;

    Position  pos;
    StateInfo st;
    return Py_BuildValue("i", pos.set(fen, chess960, &st) ? 0 : 1);
}

// EXTENSION — INPUT variant, fen, move list: full spell state so GUIs do
// not have to parse the {F@e4:3,...} FEN block.
extern "C" PyObject* pyffish_spellState(PyObject*, PyObject* args) {
    PyObject*   moveList;
    const char *fen, *variant;
    int         chess960 = false;
    if (!PyArg_ParseTuple(args, "ssO!|p", &variant, &fen, &PyList_Type, &moveList, &chess960))
        return NULL;

    Position     pos;
    StateListPtr states;
    if (!build_position(pos, states, variant, fen, moveList, chess960))
        return NULL;

    PyObject* result = PyDict_New();
    for (Color c : {WHITE, BLACK})
    {
        PyObject* side = PyDict_New();
        for (int sp = 0; sp < SPELL_NB; ++sp)
        {
            const SpellType spell = SpellType(sp);
            const Square    gate  = pos.spell_gate(c, spell);

            PyObject* entry = Py_BuildValue(
              "{s:i,s:i,s:O}", "hand", pos.spells_in_hand(c, spell), "cooldown",
              pos.spell_cooldown(c, spell), "gate",
              gate == SQ_NONE ? Py_None
                              : PyUnicode_FromString(Notation::square(gate).c_str()));
            PyDict_SetItemString(side, spell == SPELL_FREEZE ? "freeze" : "jump", entry);
            Py_XDECREF(entry);
        }
        PyDict_SetItemString(result, c == WHITE ? "w" : "b", side);
        Py_XDECREF(side);
    }
    return result;
}

static PyMethodDef PyFFishMethods[] = {
  {"version", (PyCFunction) pyffish_version, METH_NOARGS, "Get package version."},
  {"info", (PyCFunction) pyffish_info, METH_NOARGS, "Get engine version info."},
  {"variants", (PyCFunction) pyffish_variants, METH_NOARGS, "Get supported variants."},
  {"set_option", (PyCFunction) pyffish_setOption, METH_VARARGS, "Accepted no-op (rules only)."},
  {"load_variant_config", (PyCFunction) pyffish_loadVariantConfig, METH_VARARGS,
   "Accepted no-op (single variant)."},
  {"start_fen", (PyCFunction) pyffish_startFen, METH_VARARGS, "Get starting position FEN."},
  {"two_boards", (PyCFunction) pyffish_twoBoards, METH_VARARGS, "Always False."},
  {"captures_to_hand", (PyCFunction) pyffish_capturesToHand, METH_VARARGS, "Always False."},
  {"legal_moves", (PyCFunction) pyffish_legalMoves, METH_VARARGS,
   "Get legal moves from given FEN and movelist."},
  {"get_fen", (PyCFunction) pyffish_getFEN, METH_VARARGS,
   "Get resulting FEN from given FEN and movelist."},
  {"gives_check", (PyCFunction) pyffish_givesCheck, METH_VARARGS,
   "Get check status from given FEN and movelist."},
  {"is_capture", (PyCFunction) pyffish_isCapture, METH_VARARGS,
   "Get whether given move is a capture."},
  {"piece_to_partner", (PyCFunction) pyffish_pieceToPartner, METH_VARARGS, "Always ''."},
  {"game_result", (PyCFunction) pyffish_gameResult, METH_VARARGS,
   "Get result from given FEN (capture-the-king semantics)."},
  {"is_immediate_game_end", (PyCFunction) pyffish_isImmediateGameEnd, METH_VARARGS,
   "Whether a king is already gone."},
  {"is_optional_game_end", (PyCFunction) pyffish_isOptionalGameEnd, METH_VARARGS,
   "Claimable draw (50-move / repetition)."},
  {"has_insufficient_material", (PyCFunction) pyffish_hasInsufficientMaterial, METH_VARARGS,
   "Always (False, False) under capture-the-king."},
  {"validate_fen", (PyCFunction) pyffish_validateFen, METH_VARARGS,
   "Validate an input FEN (1 = OK, 0 = invalid)."},
  {"spell_state", (PyCFunction) pyffish_spellState, METH_VARARGS,
   "EXTENSION: hands/cooldowns/gates per color and spell."},
  {NULL, NULL, 0, NULL},  // sentinel
};

static PyModuleDef pyffishmodule = {
  PyModuleDef_HEAD_INIT, "pyffish_spell", "Spell-Stockfish rules extension module.",
  -1,      PyFFishMethods,
};

PyMODINIT_FUNC PyInit_pyffish_spell() {
    PyObject* module = PyModule_Create(&pyffishmodule);
    if (module == NULL)
        return NULL;

    PyModule_AddObject(module, "VALUE_MATE", PyLong_FromLong(VALUE_MATE));
    PyModule_AddObject(module, "VALUE_DRAW", PyLong_FromLong(VALUE_DRAW));
    PyModule_AddObject(module, "VALUE_NONE", PyLong_FromLong(VALUE_NONE));

    // Import-compat constants (SAN itself is phase 2)
    PyModule_AddObject(module, "NOTATION_DEFAULT", PyLong_FromLong(0));
    PyModule_AddObject(module, "NOTATION_SAN", PyLong_FromLong(1));
    PyModule_AddObject(module, "NOTATION_LAN", PyLong_FromLong(2));

    PyModule_AddObject(module, "FEN_OK", PyLong_FromLong(1));

    Bitboards::init();
    Attacks::init();
    Position::init();

    return module;
};
