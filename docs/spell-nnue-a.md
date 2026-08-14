# Spell-NNUE A ("SSNNa"): the flat architecture, no king buckets

> Status: implemented in the engine and the trainer, not yet trained.
> ADDITIONAL format: SPL3 (v2) and the run5rl adapter keep loading unchanged.
> Target net file: ~3 MB against run5rl's 101.8 MB.

## 0. Where the idea comes from

sscg13 (author of the shatranj nets, trained on 10 billion positions) and
Dean hold the same view: **king buckets do not pay for themselves until data
volumes we are never going to have**. A bucket multiplies the piece block by
32; with 50M old d2 positions plus 10M being generated, each bucket sees 3%
of the data. The same holds, worse, for FullThreats: 60,720 entries x 1,024 =
62 million parameters in a single block, designed for SF's corpus (billions
of positions).

Spell chess's branching factor means that corpus does not exist and will not
exist. The practical conclusion: spend the parameters where there is signal.

## 1. What changes relative to v2

| Block | v2 (`SpellKAv2`) | A (`SpellAv2`) |
|---|---|---|
| Pieces | 22,528 (32 king buckets, a-h mirror, kings on a shared plane) | **768** (12 planes of 64, no bucket, **one plane per king**) |
| Threats (`FullThreats`) | 60,720 | **out** |
| Freeze zones | 4,096 (king-bucketed) | **128** (flat) |
| Jump zones | 128 | 128 |
| Frozen | 128 | 128 |
| Spell globals | 30 | 30 |
| **Total per perspective** | **87,630** | **1,182** |

Everything else is identical: L1=1024 with pairwise output, 16 stacks
32/64/32/128, 16 PSQT buckets over the material x potions grid, v2's
quantization and scales, the hand/cooldown thermometers with their
precomputed delta rows, and the explicit `frozen` block.

**The kings get their own planes.** In HalfKA both kings share `PS_KING`
because the own king is already encoded in the bucket. Without buckets, a
shared plane would hide which king stands where: hence A uses 12 planes
(own P N B R Q K, enemy P N B R Q K) instead of 11.

## 2. Why the threats leave too

Not a separate idea; the same one, with the same arithmetic:

1. **Size.** Threats are 90% of the file (62 MB of i8). Keeping them, no
   reasonable L1 gets below 30 MB.
2. **Data.** 62M parameters in one block for 60M positions. Each weight sees,
   on average, less than one position.
3. **Speed.** The threat block dominates the accumulator cost (up to 128 rows
   of 1,024 i8 per node) **and** is the only reason a live jump gate forces a
   refresh in v2: by changing slider occupancy it alters threats of pieces
   the move never touched.

Removing them leaves a strong structural property: **no `SpellAv2` index
depends on a king square, so no move can invalidate an already accumulated
row**. `RequiresRefresh` is `false` at compile time, the whole search is
incremental, and the Finny table shrinks from `[64 squares][2]` to `[2]`
entries (~290 KB to ~4.5 KB per thread).

## 3. Measured size and speed

File sizes (same chassis, random nets produced by the `gen_random*` tools):

| Net | Bytes |
|---|---|
| run5rl (v1, FSF) | 101,788,576 |
| random SPL3 v2 | 92,281,360 (88.0 MiB) |
| **random SPLA** | **1,813,201** |

A trained A net weighs somewhat more than the random one (LEB128 weights grow
to 2 bytes): **~3.0 MB raw**, against ~99 MB for a trained v2. Factor ~33x
against run5rl.

Speed, single-thread `bench` on the same binary and machine (random nets, so
the trees differ; nps is the comparable metric):

| Loaded net | nps |
|---|---|
| stock (no spell net) | 447,379 |
| run5rl | 386,002 |
| random SPL3 v2 | 356,967 |
| **random SPLA** | **433,958** |

**+21.6% nps over v2** and +12.4% over the run5rl adapter.

## 4. The `SPLA` format

Magic `0x53504C41`. The loader routes it before SPL3 and displaces any other
active spell net: a single evaluation path at a time.

```
u32 version = 0x53504C41
u32 net_hash          (ft_hash ^ arch_hash)
u32 desc_len, desc
u32 ft_hash           (0x4F234CB8 ^ (L1*2))
LEB128  biases   i16[1024]
LEB128  weights  i16[1182][1024]
LEB128  psqt     i32[1182][16]
16 x { u32 arch_hash, raw fc0/fc1/fc2 }   (identical to SPL2)
```

The stacks are byte-for-byte SPL2's, so `spla.py` reuses the `spl2.py`
helpers.

## 5. Trainer side

Everything in `tools/spellnnue-pytorch/`, parallel to the v2 modules so the
path that already passed its P1 gate stays untouched:

| File | Role |
|---|---|
| `features_a.py` | pure-python `SpellAv2` extraction; reuses `normalized_gates` and the output bucket from `features.py` |
| `spla.py` | SPLA writer/reader and hash chain |
| `model_a.py` | `SpellNNUEA` (one embedding table per head, no factorizer) and the integer `quantized_forward` reference |
| `train_a.py` | training driver (same loss, lambda schedule and optimizers as the v2 one) |
| `serialize_a.py` | `.pt` checkpoint to SPLA `.nnue` |
| `gen_random_a.py` | structurally valid random net for gates |
| `parity_a.py` | engine vs python parity, with a synthetic position generator |

**There is no factorizer.** In v2 the 32 freeze gate buckets factored down to
64 virtual rows; without buckets there is nothing left to factor.

## 6. Validation already done

- MSYS2 build `ARCH=x86-64-avx2 COMP=mingw`, zero warnings.
- `bench` with run5rl loaded through UCI: **4,958,980 nodes**, identical to
  main.
- Perft suite: **336/336**.
- `parity_a.py` over 1,000 synthetic positions (426 with a live jump zone,
  390 with a live freeze zone): **0 feature diffs, 0 eval diffs, max diff
  0 cp** against the integer python reference.
- **Incremental vs forced refresh**: with a temporary patch that refreshes at
  every node, `bench` with the A net produces exactly the same 8,863,608
  nodes as the incremental path. This is the diff machinery's gate:
  reproducible by replacing the body of `evaluate_a_side` with a direct call
  to `update_accumulator_refresh_cache_a`.

## 7. Proposed training and test plan

1. **Data**: the same that would feed v2 (50M old d2 positions + the 10M
   being generated, run7 format). A changes neither the data format nor the
   generator.
2. **Overfit gate**: `train_a.py --records 1000000 --epochs 2
   --start-lambda 1.0 --end-lambda 1.0`. Must converge like v2's P1 gate.
3. **Post-training parity**: `parity_a.py --net <net> --data <run7>` with at
   least 1,000 real positions before any SPRT. Zero diffs.
4. **Net 1**: full corpus, `lambda` per the guide
   (`docs/nnue-training-guide.md`: lambda outranks epochs; start at 1.0 for
   pure eval bootstrap).
5. **SPRT** against the current default (run5rl), STC and LTC, `[0, 5]`
   first to detect whether the architecture is even competitive; if it
   passes, `[1, 6]` and the formal 3-TC panel.
6. **Ablations, one variable at a time and only after the first pass**:
   - L1 1024 to 1536 or 2048 (file grows to ~4.5 MB; requires a format bump)
   - bucket grid 16 to 8 (mat 4 x potions 2) if data runs short
   - bring FullThreats back on top of the flat feature set, to measure what
     they actually contribute at our data scale

## 8. Known risks

- **It may run short of capacity.** 1.2M FT parameters against run5rl's
  ~49M. If net 1 loses by a lot, the first lever is L1, not going back to
  buckets.
- **Without threats, the net has to learn tactics from the piece planes.**
  That is exactly the hypothesis to falsify; the SPRT will tell.
- **The 16-bucket grid divides the data by 16** (risk already noted in
  `docs/spell-nnue-v2.md` par. 9). A does not make it worse, but does not fix
  it either.
- The `frozen` block stays explicit and the thermometer stays monotone: both
  v2 wins are preserved intact.
