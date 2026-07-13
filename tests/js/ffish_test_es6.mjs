// ffish-spell ES-module smoke: same wasm surface via import.
// Usage: node ffish_test_es6.mjs
import factory from "./esm/ffish_spell.js";

let fails = 0;
const check = (name, cond, detail) => {
  if (!cond) fails++;
  console.log(`${cond ? "PASS" : "FAIL"}  ${name}${cond || !detail ? "" : "  [" + detail + "]"}`);
};

const ffish = await factory();

check("variants", ffish.variants() === "spell-chess");
const board = new ffish.Board("spell-chess");
check("legal startpos == 1878", board.numberLegalMoves() === 1878,
      String(board.numberLegalMoves()));
check("push gated", board.push("f@e7,e2e4") === true);
check("spellState", JSON.parse(board.spellState()).w.freeze.gate === "e7");
board.delete();

console.log("\n" + (fails ? "SUITE FAIL" : "SUITE PASS"));
process.exit(fails ? 1 : 0);
