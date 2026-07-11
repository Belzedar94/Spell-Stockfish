# Spell-Stockfish — Project Audit Log

Running log of all iterations. Every implementation/iteration/fix (including discarded attempts)
gets an entry: hypothesis → files changed → validation → decision → learnings.

---

## Phase 1 — Core rules on Stockfish master (2026-07-11/12)

**Goal**: full Spell Chess rules on the SF core; acceptance = behavioral parity with the frozen
baseline over the 61-position suite.

**Done**:
- 32-bit `Move` (spell type bits 16-17, gate square bits 18-23; base move = low 16 bits, so all of
  SF's from/to/promotion machinery works untouched on gated moves). `MAX_MOVES` = 8192.
- TT: 12-byte entries, 5-entry / 64-byte clusters (`tt.cpp`).
- Spell state in `StateInfo` (12 bytes: gates + cooldowns + hands per color/spell), zone geometry
  as constexpr tables (`spell.h`), full Zobrist coverage, tick discipline in `do_move`, FEN
  parse/emit incl. `[holdings]` and `{state}` block with reference normalization rules.
- Spell-aware attacks (`attackers_to` excludes frozen pieces, sliders see through transparent
  gates), movegen with gated expansion (freeze: 64 gates × filtered bases; jump: gated bases +
  newly-enabled slider/pawn moves via `newAttacks & ~oldAttacks`), self-capture support in
  `do_move`, kingless (terminal) position tolerance end-to-end.
- UCI: `f@e7,e2e4` move syntax; spell startpos; history tables masked to 16-bit base moves.
- Tools: `tests/reference/compare_perft.py` (auto divide-descent isolation) and
  `tests/spell_tests.py` (20 rule unit tests).

**Validation**: 61/61 suite positions pass (d1+d2 everywhere, d3 where present) with exact FEN
emission parity; 20/20 unit tests. Speed snapshot: `go perft 2` startpos 423 ms vs reference
2204 ms (~5.2x, process startup included). `bench` (search) deliberately deferred to Phase 2.

**Learnings / empirically discovered rules** (all encoded in SPELL_SPEC.md — the divide-descent
comparator against the frozen binary was the key tool; 10 iterations from 4/61 to 61/61):
1. The king may not MOVE onto an attacked square (candidate freeze CAN silence the attacker; the
   rest of self-check is legal).
2. En passant keeps the classic king-safety filter, evaluated spell-aware — a jump-transparent
   pawn can create a "pin through the gate".
3. Pawn pushes use PHASE-FLIPPED occupancy on transparent squares (occupied→landable as capture,
   even onto OWN pieces = self-capture; empty→solid). Pieces may never land quietly on transparent
   squares; captures onto occupied ones are fine.
4. Castling: in-check test on pre-cast state only; path test WITH candidate context; transparent
   ROOK can't castle, transparent KING can; freeze gate ∉ block(king-from) ∪ {king-destination}.
5. `needsEvasion = checkers && !allow_self_check` in the reference → LEGAL never uses evasion
   staging in spell chess; in-check universe stays full.
6. SF's `go` re-validates the position from FEN, so `set()` must accept kingless (terminal)
   positions.

**Decision**: Phase 1 accepted. Next: Phase 2 (playable search + provisional eval + spell bench).

---

## Phase 0 — Setup, frozen baseline & spec (2026-07-11)

**Goal**: reproducible baseline, behavioral spec, perft parity suite, working match harness.

**Done**:
- Repo seeded from `official-stockfish/Stockfish @ 9a8dd81dd7f98cbf02f16c59b4377d174d6eb4b5`
  (master, 2026-07-11, "Revert 'Scale Null Move Pruning reduction'"); remote `upstream` = official
  Stockfish, `origin` = github.com/Belzedar94/Spell-Stockfish (private).
- Sibling layout: `FSF-spell-baseline/` (public `Belzedar94/Fairy-Stockfish @ 8868ab43`, branch
  `spell/nnue-potions`), `Spell-nnue-pytorch/` (fork of `spell_nnue-potion` branch),
  `upstream/{Fairy-Stockfish, variant-nnue-tools, variant-nnue-pytorch}` (reference clones).
  `Spell Project/` left untouched as historical reference.
- Baseline frozen: profile-build bmi2 (`largeboards=yes all=yes`), copied to
  `Match script/FSF_Spell_test_baseline.exe` and `FSF-spell-baseline/FSF_Spell_test_baseline.exe`.
- Verified: run5rl NNUE loads and evaluates; perft startpos d1 = 1878, d2 = 3,287,752; bench NNUE
  = 262,449 NPS (see BENCH_LOG.md).
- Smoke match (20 games VSTC, baseline vs baseline, run5rl both sides): completed cleanly,
  W11 L8 D1 — harness OK end-to-end.
- `SPELL_SPEC.md` written from reference source analysis (tick discipline, move universe, wire
  formats, NNUE contract, verification protocol).
- Perft parity suite generated against the frozen baseline
  (`tests/reference/gen_perft_suite.py` → `tests/reference/perft_spell.csv`, 61 positions:
  active zones of both types, cooldowns 1–3, spent holdings, checks, endgames; d1+d2 for all,
  d3 for small ones), plus full root divides for startpos d1/d2.

**Learnings / context for future sessions**:
- The public branch's legal universe **differs from the private historical line**: startpos
  d1 = 1878 (public) vs 1814 (old private logs) — exactly +64 = one extra freeze-gated base move
  per gate. Parity target is the PUBLIC baseline. Never mix perft expectations from
  `Spell Project/` logs into this repo.
- `allow_self_check() == true` in spell-chess (extinction royal commoner): self-check is legal,
  the game is capture-the-king. This must shape movegen/legality from day one.
- Spells cannot be combined with promotions or en-passant base moves (reference filters base move
  types to NORMAL/CASTLING only).
- Gate limiting (`MaxFreezePotionGates=12` / `MaxJumpPotionGates=6`) exists in the PUBLIC baseline
  movegen but only in the QUIETS path (search policy) — legal universe is unlimited.
- Baseline's depth-13 startpos choice is `f@e7,e2e4` — freeze the enemy king area + take the
  center. Spell tempo is real.

**Decision**: Phase 0 accepted. Next: Phase 1 (core rules on SF master).
