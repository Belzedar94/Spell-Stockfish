# Bench log

Every entry: date, binary/commit, bench command, nodes, NPS, notes.
Reference machine: AMD Ryzen 9 5950X, Windows 10, MSYS2 mingw64 g++, ARCH=x86-64-bmi2, 1 thread.

| Date | Binary @ commit | Command | Nodes | NPS | Notes |
|------|-----------------|---------|-------|-----|-------|
| 2026-07-11 | FSF_Spell_test_baseline @ 8868ab43 (FSF spell/nnue-potions) | `bench spell-chess 16 1 13 default depth NNUE` (run5rl) | 4,415,452 | 262,449 | Frozen reference baseline. bestmove f@e7,e2e4 |
