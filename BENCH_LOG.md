# Bench log

Every entry: date, binary/commit, bench command, nodes, NPS, notes.
Reference machine: AMD Ryzen 9 5950X, Windows 10, MSYS2 mingw64 g++, ARCH=x86-64-bmi2, 1 thread.

| Date | Binary @ commit | Command | Nodes | NPS | Notes |
|------|-----------------|---------|-------|-----|-------|
| 2026-07-11 | FSF_Spell_test_baseline @ 8868ab43 (FSF spell/nnue-potions) | `bench spell-chess 16 1 13 default depth NNUE` (run5rl) | 4,415,452 | 262,449 | Frozen reference baseline. bestmove f@e7,e2e4 |
| 2026-07-12 | Spell-Stockfish @ phase-1-core-rules+review3 | `bench 16 1 13 default depth` (embedded chess nets) | 2,729,943 | 323,338 | Review round 3: MAX_MOVES 32768 + heap MovePicker arena (stack frames would cost ~40% NPS), pseudo_legal transparency fixes, phase-flip promo partition, Syzygy no-op. Tree changed vs F2 (pseudo_legal + promo partition). |
| 2026-07-12 | Spell-Stockfish @ phase-3-nnue+review3 (incremental acc + review fixes) | `bench 16 1 10 default depth` (run5rl) | 819,199 | 199,950 | New signature: tree changed by pseudo_legal transparency + promo partition fixes. NPS: incremental accumulator 208k minus ~4% arena cost. |
| 2026-07-12 | Spell-Stockfish @ phase-4-strength 6cb7ae34 | `bench 16 1 10 default depth` (run5rl) | 2,468,562 | 397,770 | Lazy spell staging + useless-spell filter + r-clamp(mn24) + tacticalSpell/GateHistory ports + king commoner value 700. NPS now ABOVE the 262k baseline. |
| 2026-07-12 | Spell-Stockfish @ phase-4-strength 10f6ea3e (SPSA session-1 defaults) | `bench 16 1 10 default depth` (run5rl) | 2,395,529 | ~370k | S5 signature registered (tests/signature_test.py). Tree changed vs 6cb7ae34 by SPSA defaults 58562a0e + SpellStageMargin. Netless same command: 2,785,455 (CI mode). |
| 2026-07-12 | Spell-Stockfish @ phase-4-strength 10f6ea3e | `bench` (plain, default depth, netless) | 13,456,297 | 379,200 | OpenBench worker protocol (Client/bench.py runs plain `bench`, 60s timeout; took 36s). Commit discipline for OpenBench: `Bench: <plain-bench-nodes>`. |
