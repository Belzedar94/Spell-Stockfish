# Bench log

Every entry: date, binary/commit, bench command, nodes, NPS, notes.
Reference machine: AMD Ryzen 9 5950X, Windows 10, MSYS2 mingw64 g++, ARCH=x86-64-bmi2, 1 thread.

| Date | Binary @ commit | Command | Nodes | NPS | Notes |
|------|-----------------|---------|-------|-----|-------|
| 2026-07-11 | FSF_Spell_test_baseline @ 8868ab43 (FSF spell/nnue-potions) | `bench spell-chess 16 1 13 default depth NNUE` (run5rl) | 4,415,452 | 262,449 | Frozen reference baseline. bestmove f@e7,e2e4 |
| 2026-07-12 | Spell-Stockfish @ phase-1-core-rules+review3 | `bench 16 1 13 default depth` (embedded chess nets) | 2,729,943 | 323,338 | Review round 3: MAX_MOVES 32768 + heap MovePicker arena (stack frames would cost ~40% NPS), pseudo_legal transparency fixes, phase-flip promo partition, Syzygy no-op. Tree changed vs F2 (pseudo_legal + promo partition). |
| 2026-07-12 | Spell-Stockfish @ phase-3-nnue+review3 (incremental acc + review fixes) | `bench 16 1 10 default depth` (run5rl) | 819,199 | 199,950 | New signature: tree changed by pseudo_legal transparency + promo partition fixes. NPS: incremental accumulator 208k minus ~4% arena cost. |
