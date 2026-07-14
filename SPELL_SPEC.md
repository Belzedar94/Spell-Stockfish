# Spell Chess — Rules & Behavioral Specification

This document is the single source of truth for what Spell-Stockfish must implement.

**Authority order** for any ambiguity:
1. The frozen reference binary: `FSF_Spell_test_baseline.exe`, built from
   `Belzedar94/Fairy-Stockfish @ 8868ab43a47a3971fee05f136a392a93194635c3` (branch `spell/nnue-potions`,
   profile-build `ARCH=x86-64-bmi2 COMP=mingw largeboards=yes all=yes`). Behavioral parity with this
   binary (move universe, FEN handling, game termination) is the acceptance criterion.
2. chess.com official articles ([terms/spell-chess](https://www.chess.com/terms/spell-chess),
   [support article](https://support.chess.com/en/articles/8588083-what-is-spell-chess)).

Internal representation is unconstrained (we deliberately do NOT inherit Fairy-Stockfish internals);
only externally observable behavior must match.

## 1. Game overview

Spell Chess is standard chess plus two spell types per player:

| Spell  | Count per player | Cooldown | Effect |
|--------|-----------------|----------|--------|
| Freeze (`F`/`f`) | 5 | 3 own moves | Freezes a 3x3 area (clipped at board edges) centered on the gate square |
| Jump (`J`/`j`)   | 2 | 3 own moves | Makes one occupied square "transparent" for sliding movement of BOTH sides |

- A spell is cast **together with a regular move, in the same ply** ("gated move"). You cannot cast
  without moving, and you cannot move twice.
- Win conditions: checkmate **or capturing the enemy king**. The reference models the king as a
  royal COMMONER with extinction value −MATE: capturing it ends the game immediately.
- **Self-check is legal** (`allow_self_check() == true` in the reference: extinction value set, royal
  piece in extinction set, not pseudo-royal). Moves that leave or place the own king in check are
  legal; the punishment is that the king can be captured. "Check" still exists as a concept
  (e.g. castling restrictions, search bonuses) but does not restrict move legality — **with one
  exception, verified against the reference: the king may not MOVE onto an attacked square**
  (attackers evaluated spell-aware: frozen attackers — including those frozen by the very cast
  carried by the king move — do not count, and jump transparency can open new attack lines).
  Self-check by any other means (breaking a pin, zone expiry, discovered attacks) is legal.
  **Sub-exception (found in match play, verified against the reference binary): a king move that
  CAPTURES the enemy king is legal even onto a defended square** — the game ends with the royal
  capture, nothing gets to recapture (e.g. adjacent kings: Kb4xc3 with a white knight guarding
  c3 is legal and immediately winning, gated copies included).

## 2. Spell state model

Per color `c` and spell type `P ∈ {FREEZE, JUMP}` the reference tracks in `StateInfo`:
- `potionZones[c][P]` — bitboard of the active zone (empty if none).
  - FREEZE zone: 3x3 area centered on gate square, clipped to the board.
  - JUMP zone: exactly the gate square.
- `potionCooldown[c][P]` — integer 0..3.
- Spells in hand — tracked as pocket counts (start: 5×F + 2×J each).

All of the above are part of the **Zobrist key** (zones per-square keys; cooldown hashed bitwise),
so repetition detection distinguishes spell states. Start position: all zones empty, cooldowns 0,
hands full.

### 2.1 Tick discipline (critical)

Inside `do_move` by side `us` (after the base move is applied), the reference does exactly:

1. For `us`: if this move casts spell `P`:
   - `cooldown[us][P] = 3` and `zone[us][P] = cast zone`.
   - (If not casting and `cooldown[us][P] == 0`, zone is cleared defensively.)
2. For `opp = ~us`, for each `P` with `zoneLifetime = cooldown_config − 1 = 2`:
   - if `cooldown[opp][P] > 0`: decrement it; if now `≤ zoneLifetime`, clear `zone[opp][P]`.
   - else clear `zone[opp][P]`.

Consequences:
- A player's cooldown ticks once per **full move** (during the opponent's `do_move`).
- A cast zone is active during: the **casting ply itself** (the base move of the cast already sees
  the zone) and the **opponent's single reply**. It expires at the end of the opponent's `do_move`
  (before the caster's next turn).
- After casting at own move N, the same spell type is castable again at own move N+3.
- `checkersBB` is recomputed **after** the potion tick block, from scratch
  (`attackers_to(royal)`), never trusting a `givesCheck` hint — checks can (dis)appear due to zone
  expiry alone.

### 2.2 FEN normalization on parse

After parsing, for each color/spell: if `cooldown == 0` → zone cleared; if
`cooldown < 2` → zone cleared; if `cooldown == 2 && side_to_move == caster` → zone cleared.
This makes `parse(fen(pos))` idempotent and rejects impossible zone/cooldown combinations.

## 3. Effects of active zones

### 3.1 Freeze
- A freeze zone cast by color `c` affects **only `~c`'s pieces** on subsequent plies, and restricts
  the **origin** square: a piece standing inside the enemy zone cannot move. Moving **into** the
  zone is legal. Frozen pieces can be captured normally.
- **Frozen pieces do not attack**: `attackers_to` excludes pieces standing in an enemy freeze zone.
  They give no check, defend nothing, and do not prevent castling.
- On the **casting ply**, the caster's own base move may not originate from anywhere in the
  **full 3x3 zone** of the new gate (clipped at the edges) — the caster's pieces inside the zone
  are unavailable for the accompanying move. **Verified empirically on chess.com 2026-07-14**
  (analysis board: after 1.e4 c5 with freeze staged at d2, the e1 king — diagonal to the gate —
  cannot move; c2 with an outside destination cannot move either, so the block is origin-based;
  out-of-zone pieces complete the turn normally, PGN "freeze@d2 Nf3"). Startpos legal moves:
  **1814**. (History: the old private line had this right; the public reference commit
  `5264a3f7` "relaxed" it to plus-shaped orthogonal adjacency, which was the actual bug — both
  this engine and upstream FSF inherited it until RainRat's report, PR #5.)
### 3.1b Castling (all rules verified empirically against the frozen baseline)
- No castling while **in check**, evaluated on the **pre-cast state only**: a candidate freeze does
  NOT silence the checker (the check would reappear on zone expiry), and a candidate jump's
  transparency does NOT create a disqualifying check either (`j@d2,e1g1` is legal even when the
  transparency opens a latent diagonal onto e1).
- **Path squares** (king destination up to, but excluding, the king's origin) must not be attacked —
  evaluated WITH the candidate context: freezing the path-attacker with the very cast legalizes the
  castling (allowed freeze gates = exactly the 3x3 neighborhood of the attacker, minus the
  restrictions below), and candidate transparency can open new attack lines onto the path.
- Frozen king or frozen castling rook: no castling.
- A **jump-transparent rook cannot castle** (phased out); a jump-transparent KING still can —
  including casting `j@<king-square>` together with the castling in the same ply.
- Freeze-gated castling: the gate may not be in the block zone of the king's origin (general
  caster rule) **nor equal the king's destination square**.
- Jump-gated castling: the gate may not equal the base move's `to` square (the rook's square, per
  the general jump rule).
- Path emptiness uses the standard physical occupancy.

### 3.2 Jump
- The jump gate square must be **occupied** (either color). While the zone is active, that square is
  removed from occupancy for **sliding attack/movement computation of BOTH colors** (the opponent
  can use the transparency during their reply ply).
- The piece standing on the gate square may itself move normally.
- No move may **land on** the gate square while gated in the same cast (`to == gate` is filtered for
  the casting ply's combined moves); capturing the jumped piece without the spell is of course a
  normal base move.
- **Transparent-square landings** (verified empirically against the reference binary):
  - Sliding RAYS: transparent squares never block (removed from occupancy), whether physically
    occupied or empty.
  - Pieces (knight/bishop/rook/queen/king) may NOT land quietly on a jump-transparent square,
    whether it is physically empty or occupied. Capturing a piece that stands on one is allowed.
  - **Pawn pushes use a PHASE-FLIPPED occupancy**: a transparent square inverts its state — an
    occupied one counts as empty (the push may land on it; the reference resolves the landing as a
    forward "capture"), an empty one counts as solid (no push may land there, e.g. after the jumped
    piece moved away while the zone persists). Applies to single-push landings and both squares of
    a double push. Pawn captures target physical enemies as usual.
- **En passant king-safety** (the one non-king move with a legality filter, inherited from the
  reference): an ep capture is illegal if, with both pawns removed and the capturer placed on the
  ep square, the own king is attacked — attackers evaluated spell-aware (jump transparency can
  create such a "pin through the transparent square"; frozen attackers do not count). The ep square
  itself is recorded in FEN/state whenever an enemy pawn pseudo-attacks it and it is physically
  empty, independent of this capture-time filter.

## 4. Move universe (perft-relevant)

A legal move is either:
- a **normal move** (standard chess rules, restricted by active freeze origins; sliders see
  jump-transparency of any active zone), or
- a **gated move** `spell@gate,basemove`:
  - Base move must be of type NORMAL or CASTLING. **Promotions and en-passant captures cannot be
    combined with a spell cast.** (Jump-enabled extra moves: NORMAL only, no castling.)
  - FREEZE gates: any board square (occupied allowed, `potionDropOnOccupied = true`). Filters on the
    casting ply: base `from != gate`, base `from` outside the new block zone, base `from` not
    already frozen (enemy zone), castling `to` not frozen.
  - JUMP gates: any occupied square. Filters: base `to != gate`. Additionally, with the gate removed
    from occupancy, **new slider/pawn moves** that were previously impossible become available
    (recomputed for all non-pawn/non-royal rider types plus pawns, target-restricted per GenType;
    duplicates of existing base moves and moves landing on the gate are excluded).
  - Casting requires: spell in hand and `cooldown[us][P] == 0`.
- When in check (EVASIONS), potion moves are still generated: freeze casts are generated against the
  full NON_EVASIONS base list (freezing the checker turns non-evasions into legal continuations);
  under double check by non-riders, only king base moves are extended (`allowNonKing` rule).
- Search-level gate limiting (`MaxFreezePotionGates = 12`, `MaxJumpPotionGates = 6`, king-ring
  overrides) applies **only** to the QUIETS generation stage used by the search — never to the
  legal universe (perft, UCI move validation, evasions, urgent defense, or when an enemy freeze is
  active).

### 4.1 Reference perft (frozen baseline, startpos)

```
depth 1: 1878   (= 20 normal + 1188 freeze-gated + 670 jump-gated)
depth 2: 3287752
```
Full root divide captured in `tests/reference/perft_divide_startpos_d1.txt` / `_d2.txt`.
Extended suite: `tests/reference/perft_spell.csv` (generated by
`tests/reference/gen_perft_suite.py` against the frozen baseline).

**Known historical divergence — RE-RESOLVED 2026-07-14**: earlier private builds recorded
`d1 = 1814` (full-3x3 caster block); the public branch "relaxed" this to orthogonal adjacency
(commit `5264a3f7`), and 1878/3287752 became the reference numbers. RainRat's PR #5 challenged
this and a direct chess.com experiment CONFIRMED the full-3x3 block (diagonal king blocked,
origin-based, out-of-zone free). The authority is chess.com: **1814** is correct, the plus-shape
was the bug, and the frozen FSF baseline binary carries it (kept for historical parity of every
OTHER rule; cross-engine matches against it now have divergent rules on this point).

## 5. Wire formats

### 5.1 FEN
```
rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR[JJFFFFFjjfffff] w KQkq - 0 1
```
- Holdings in brackets after the board: uppercase = White's spells, lowercase = Black's
  (`J`×2 + `F`×5 each at start). Order inside brackets is not significant; the reference emits
  jumps first.
- Active spell state is emitted as a `{...}` block between the holdings and the side to move —
  exact emission order (confirmed against reference output):
  `<board>[holdings] {state} <stm> <castling> <ep> <rule50> <fullmove>`, e.g.
  `r1bqkbnr/1ppppp1p/p1n3p1/8/1P1P4/5P2/PBP1P1PP/RN1QKBNR[JJFFFFjjfffff] {F@e3:3,J@-:0,f@-:0,j@-:0} b KQkq - 0 4`.
  Per color/spell entry: `<char>@<zone-center-square or '-'>:<cooldown>`. The reference **always**
  emits the block (even when all entries are `-:0`); the parser must accept a missing block and
  any subset of entries (the startpos config FEN omits it entirely).
- Freeze zone center: the square whose (clipped) 3x3 zone equals the stored zone.
- **Invariant (community observation, Discord help 2026-07-10)**: since zones live for the
  cast ply plus exactly one reply, a legal BETWEEN-MOVES position can carry **at most one
  active zone**, and it always belongs to the side that just moved. A compressed dialect
  could therefore emit a single owner-less zone (cooldowns still need all four entries).
  Our wire format keeps the explicit four-entry block — it is the de-facto community
  format (quoted verbatim in the upstream FEN-standardization discussion) and byte-parity
  with the frozen oracle is a project gate — but parsers should be aware the four-zone
  space is mostly unreachable.

### 5.2 UCI moves
- Normal moves: standard UCI (`e2e4`, `e7e8q`, castling `e1g1`).
- Gated moves: `f@<gate>,<basemove>` / `j@<gate>,<basemove>` (lowercase spell char regardless of
  color), e.g. `f@e7,e2e4`, `j@d6,d7d5`, `f@g8,e1g1`.

### 5.3 UCI options
Keep accepting `UCI_Variant` (value `spell-chess` only) and `VariantPath` (parsed and ignored) so
existing GUIs/harnesses work unchanged.

## 6. NNUE contract (compat-first phase)

Spell-Stockfish's first NNUE implementation must load existing reference nets
(e.g. `spell-chess_run5rl_e10_l07.nnue`) and produce **identical evaluations** to the baseline:
- Version `0x7AF32F20`, feature hash `0x6a8f3c12` (HalfKAv2Variants + potions).
- Feature planes: piece-square (piece order: pawn, knight, bishop, rook, queen, freeze, jump,
  commoner-as-king; colorless king plane), **spells in hand as pocket features**, **potion zone
  planes and cooldown planes** (indices from the reference `nnuePotionZoneIndexBase` /
  `nnuePotionCooldownIndexBase` derivation).
- Architecture: 512×2 → 16 → 32 → 1, 8 PSQT buckets, 8 layer stacks, bucket index
  `(pieceCount − 1) × 8 / 46` (`MaxPieces = 46` = 32 board pieces + 14 spells in start FEN).
- Eval scale: hybrid classical/NNUE combination as in the reference
  (`903 + 32·pawns + 32·nonPawnMaterial/1024` scaling family) — verify exact formula during F3.
- Acceptance: eval-parity harness over ≥10k varied positions (active zones, cooldowns, reduced
  material) — identical outputs including bucket selection.

Architecture evolution (larger L1, new features) is allowed **after** M1, gated by matches, with an
explicit version bump in our own serializer.

## 6b. Training-data format (PackedSfenValue + potions)

Byte contract verified against the reference tools binary
(`Spell Project/variant-nnue-tools/src/stockfish_tools_v20_x86-64-bmi2.exe`,
`generate_training_data`); decoder/validator: `tools/psv_decode.py`. Record = **76 bytes**
(`DATA_SIZE = 512`):

- `sfen[64]` — LSB-first bitstream:
  1. side to move (1 bit, 0 = white)
  2. white king square (7 bits), black king square (7 bits); a captured king writes the
     out-of-board sentinel 64
  3. board scan rank 8→1, file a→h, skipping kings: `0` = empty; otherwise 5-bit huffman code
     (LSB always 1; code = 2·idx+1 with idx in variant order P N B R Q = 0..4) followed by
     1 color bit (1 = black)
  4. hands: for each color (white, black): **8 × 5-bit counts in FSF piece-id order
     P N B R Q K F J** — only F (slot 6) and J (slot 7) are ever nonzero
  5. potion blocks: for each color × {FREEZE, JUMP}: `has_zone` (1 bit) + zone-center square
     (7 bits, 0 when none) + cooldown (16 bits). The gate square is the center; any center
     producing the same zone is feature-equivalent.
  6. castling KQkq (4 bits), en-passant flag (1 bit) + square (7 bits when set)
  7. rule50 low 6 bits, fullmove low 8 + high 8 bits, rule50 high bit
- `score` (i16) — search value, side-to-move POV, internal units
- 2 alignment bytes
- `move` (u32) — PV first move; in OUR data this is the engine's native 32-bit encoding
  (spell payload in bits 16+). The trainer only uses it for teacher-match filtering, so the
  encoding must merely be self-consistent within a data set.
- `gamePly` (u16), `game_result` (i8, +1 = side to move eventually wins), padding (u8)

Generation: `datagen out FILE count N nodes M [seed S] [random_plies R] [eval_limit E]
[ply_limit P]` — self-play from the spell startpos, uniformly random legal moves for the first
R plies (spells included), then fixed-node searches; positions written only for searched plies;
a decisive score (|v| ≥ eval_limit or mate) ends the game and sets the result. Trainer-side
requirements (F6): variant.h with PIECE_TYPES/POCKETS/HAS_POTIONS/DATA_SIZE=512 in spell mode.

## 7. Verification protocol

Every major step: profile build (bmi2) → `bench` (NPS + node signature logged in `BENCH_LOG.md`) →
perft suite (`tests/reference/perft_spell.csv`) → python UCI test suite. Strength-affecting changes:
VSTC (≥100 games) then STC/LTC in parallel (`-T 4`), early stop only at LOS 0/100; a change passes
only with LOS 100% at all three TCs (>100 games each). Before launching the 3-TC battery, check for
other running tests on the machine (shared box) — if more than one test is already running, wait.

Baseline reference numbers (2026-07-11): bench `spell-chess 16 1 13 default depth NNUE` with run5rl
→ **262,449 NPS** (4,415,452 nodes, 16.8 s).

## 8. Items to verify empirically during F1 (against the frozen baseline)

- [ ] Halfmove clock (rule50) semantics for gated moves (does a quiet cast reset it? expected: no —
      base-move semantics only) and n-fold repetition claim rules.
- [ ] Stalemate value when the side to move has no legal moves (expected: standard draw; rare given
      self-check legality).
- [ ] En-passant square lifecycle when the double push is a gated base move (`j@d6,d7d5` → is the
      resulting EP capture available and combinable?).
- [ ] Castling-rights / frozen-rook interaction details (all four rights, Chess960 off).
- [ ] Exact FEN emission field order + `{...}` omission rules; round-trip on all suite positions.
- [ ] 50-move / material draw adjudication defaults used by the harness.
- [ ] Behavior when a player has zero legal moves vs. extinction priority.
