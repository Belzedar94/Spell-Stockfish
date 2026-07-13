// ffish-spell.js (CommonJS) test suite — mirrors tests/pyffish_test.py.
// Usage: node ffish_test.js   (after scripts/build_ffishjs.sh)
"use strict";

const factory = require("./cjs/ffish_spell.js");

const START_FEN_RAW =
  "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR[JJFFFFFjjfffff] w KQkq - 0 1";

let fails = 0;
function check(name, cond, detail) {
  if (!cond) fails++;
  console.log(`${cond ? "PASS" : "FAIL"}  ${name}${cond || !detail ? "" : "  [" + detail + "]"}`);
}

factory().then(ffish => {
  check("variants", ffish.variants() === "spell-chess", ffish.variants());
  check("startingFen", ffish.startingFen("spell-chess") === START_FEN_RAW);
  check("capturesToHand", ffish.capturesToHand("spell-chess") === false);
  check("validateFen ok", ffish.validateFen(START_FEN_RAW) === 1);
  check("validateFen bad", ffish.validateFen("garbage") === 0);
  check("info", ffish.info().length > 0);

  const board = new ffish.Board("spell-chess");
  const legal = board.legalMoves().split(" ");
  check("legal startpos == 1878 (perft d1)", legal.length === 1878, String(legal.length));
  check("numberLegalMoves", board.numberLegalMoves() === 1878);
  check("gated move in list", legal.includes("f@e7,e2e4"));
  check("turn white", board.turn() === true);
  check("pocket white", board.pocket(true) === "fffffjj", board.pocket(true));
  check("pocket black", board.pocket(false) === "fffffjj", board.pocket(false));

  check("push gated", board.push("f@e7,e2e4") === true);
  check("push invalid rejected", board.push("e7e5") === false);
  check("turn black", board.turn() === false);
  check("moveStack", board.moveStack() === "f@e7,e2e4", board.moveStack());
  check("fen has spell block", board.fen().includes("F@e7:3") || board.fen().includes("F@e7:"),
        board.fen());
  check("pocket after cast", board.pocket(true) === "ffffjj", board.pocket(true));

  const st = JSON.parse(board.spellState());
  check("spellState hand", st.w.freeze.hand === 4 && st.b.freeze.hand === 5,
        board.spellState());
  check("spellState gate", st.w.freeze.gate === "e7" && st.w.jump.gate === null);
  check("spellState cooldown", st.w.freeze.cooldown > 0);

  board.pop();
  check("pop restores stack", board.moveStack() === "");
  check("pop restores legal count", board.numberLegalMoves() === 1878);

  board.pushMoves("e2e4 e7e5 f@d4,g1f3");
  check("pushMoves", board.moveStack() === "e2e4 e7e5 f@d4,g1f3", board.moveStack());
  check("gamePly", board.gamePly() === 3);
  check("fullmoveNumber", board.fullmoveNumber() === 2);
  check("isCheck false", board.isCheck() === false);
  check("isGameOver false", board.isGameOver() === false);
  check("result ongoing", board.result() === "*");
  board.delete();

  // capture-the-king terminal: black king gone, black to move -> white won
  const done = new ffish.Board(
    "spell-chess",
    "rnbq1bnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR[JJFFFFFjjfffff]"
      + " {F@-:0,J@-:0,f@-:0,j@-:0} b - - 0 1");
  check("terminal isGameOver", done.isGameOver() === true);
  check("terminal result", done.result() === "1-0", done.result());
  done.delete();

  const cap = new ffish.Board("spell-chess");
  cap.pushMoves("e2e4 d7d5");
  check("isCapture exd5", cap.isCapture("e4d5") === true);
  check("isCapture e4e5", cap.isCapture("e4e5") === false);
  cap.delete();

  console.log("\n" + (fails ? "SUITE FAIL" : "SUITE PASS"));
  process.exit(fails ? 1 : 0);
});
