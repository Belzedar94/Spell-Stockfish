# Spell-Stockfish — Project Audit Log

Running log of all iterations. Every implementation/iteration/fix (including discarded attempts)
gets an entry: hypothesis → files changed → validation → decision → learnings.

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
  (`tests/reference/gen_perft_suite.py` → `tests/reference/perft_spell.csv`), plus full root
  divides for startpos d1/d2.

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
