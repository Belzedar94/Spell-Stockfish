# Training tournament: A vs v2 vs legacy

Three NNUE architectures trained on one corpus under one regime, so that the
SPRT that follows measures the architecture and not the recipe.

| Target | Trainer | Feature set | Net format |
|---|---|---|---|
| **A** | `tools/spellnnue-pytorch/train_a.py` (branch `spell-a-flat`) | `SpellAv2`, 1,182 inputs per perspective, flat | SPLA `0x53504C41` |
| **v2** | `tools/spellnnue-pytorch/train_overfit.py` (`master`) | `SpellKAv2`, 87,630 inputs, 32 king buckets, FullThreats | SPL3 `0x53504C33` |
| **legacy** | `../Spell-nnue-pytorch/train.py` (FSF era) | `HalfKAv2^`, 96,256 inputs | SF `0x7AF32F20` |

Phase 1 (the overfit gate for A) is done and recorded below. Phase 2 (the three
full runs) is specified but deliberately not launched.

## 1. Environment

Verified on the host that produced the phase 1 numbers.

| Item | Value |
|---|---|
| Python | `C:\Users\djime\AppData\Local\Programs\Python\Python312\python.exe` (the default `python` on PATH) |
| torch | 2.7.1+cu118, `torch.cuda.is_available()` true |
| GPU | NVIDIA GeForce RTX 3080, 10,240 MiB, about 870 MiB held by the desktop |
| CPU / RAM | Ryzen 5950X, 31.9 GB |
| Engine build | MSYS2 mingw, `ARCH=x86-64-avx2 COMP=mingw` |

No extra package was installed. The training scripts import only `torch`,
`numpy` and the standard library.

### Engine build workaround

`make` on this host exports an empty `TMP`/`TEMP`/`TMPDIR` to its children, so
GCC falls back to `C:\WINDOWS\` for scratch files and every compile dies with
`Cannot create temporary file in C:\WINDOWS\: Permission denied`. Force the
variables through make:

```bash
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
BT=/c/path/to/a/writable/dir && mkdir -p "$BT"
cd src && make -j6 build ARCH=x86-64-avx2 COMP=mingw --eval="export TMP := $BT
export TEMP := $BT
export TMPDIR := $BT"
```

Compiling the same translation unit directly from the shell works without this,
which is what isolates the fault to make and not to the toolchain.

## 2. Phase 1 result: the A overfit gate

Exactly the invocation prescribed by `docs/spell-nnue-a.md` par. 7.2, on the
audited 1M record run7 file.

```bash
python tools/spellnnue-pytorch/train_a.py \
  --data <hf_dataset>/positions_1786692314.run7 \
  --records 1000000 --epochs 2 --start-lambda 1.0 --end-lambda 1.0 \
  --out .scratch/spell-a-overfit.nnue \
  --curve .scratch/a-overfit-curve.json \
  --checkpoint .scratch/spell-a-overfit.pt
```

**PASS.** Loss 0.05040193 to 0.00591638, ratio **0.117x** against a gate of
0.90. 2,000,000 positions in 978 steps, 157.9 s wall, batch 2048, seed 1, no
NaN anywhere in the curve.

| Epoch | Step | Positions | Loss |
|---|---|---|---|
| 1 | 1 | 2,048 | 0.06070114 |
| 1 | 150 | 307,200 | 0.03412665 |
| 1 | 300 | 614,400 | 0.00910814 |
| 1 | 450 | 921,600 | 0.00851230 |
| 2 | 675 | 1,380,928 | 0.00673907 |
| 2 | 900 | 1,841,728 | 0.00545897 |
| 2 | 975 | 1,995,328 | 0.00611348 |

The drop is concentrated in the first epoch and flattens into a noisy floor
around 0.006, which is the expected shape for a pure eval target.

### Post-training parity

```bash
python tools/spellnnue-pytorch/parity_a.py --engine src/stockfish.exe \
  --net .scratch/spell-a-overfit.nnue \
  --data <hf_dataset>/positions_1786692314.run7 --count 1000
```

**PASS.** 1,000 real positions, 200 with a live jump zone, 200 with a live
freeze zone, **0 feature mismatches, 0 eval mismatches, max diff 0 cp**.
Repeated at `--count 5000 --min-live-jump 1000 --min-live-freeze 1000`, also
zero diffs. Engine `evala` and the integer Python reference agree exactly.

### Measured throughput and memory

Both figures come from this host at the trainers' default batch of 2048.

| Trainer | Throughput | Peak VRAM (includes desktop) | Fits at batch 2048 |
|---|---|---|---|
| A | about 12,000 positions/s | 4,778 MiB | yes, with wide margin |
| v2 | about 4,100 positions/s | 5,952 MiB | yes |

Neither came near the 10 GB limit, so **no batch reduction is needed** and the
full runs below keep the default 2048. A is roughly 3x faster than v2 per
position, which is consistent with dropping FullThreats and the king buckets.

## 3. Phase 2 corpus preparation (shared by all three)

The corpus is 200 bz2 chunks of 250,000 run7 records each plus the 1M record
run7 file, 51,000,000 records total.

| Source | Records | Location |
|---|---|---|
| run9-50m | 50,000,000 | `C:\Users\djime\Documents\Chess_variants\spell-data\run9-50m\chunk_*.bz2` |
| HF 1M | 1,000,000 | `<scratchpad>\hf_dataset\positions_1786692314.run7` |

`train_a.py` and `train_overfit.py` mmap a single run7 file, so the chunks must
first be merged into one deterministically shuffled file. Stage every input in
one directory under a common `chunk_*` name so the merge glob picks up both the
compressed chunks and the plain file, then merge:

```bash
STAGE=/c/path/to/stage && mkdir -p "$STAGE"
cp C:/Users/djime/Documents/Chess_variants/spell-data/run9-50m/chunk_*.bz2 "$STAGE"/
cp <scratchpad>/hf_dataset/positions_1786692314.run7 "$STAGE"/chunk_200.run7

python tools/spellnnue-pytorch/merge_shuffle.py \
  --chunks "$STAGE/chunk_*" --out "$STAGE/run9-51m.run7" --seed 20260815
```

Expected output: `51,000,000 registros, 2,244,000,032 bytes`. The merge reads
`.bz2` and plain `.run7` in the same pass and sorts by the digits in the file
name, so `chunk_200.run7` lands last before the shuffle.

Budget: the merge holds the whole corpus in RAM more than once, peaking near
5 GB, which is comfortable at 31.9 GB but not something to run beside another
large job. Disk needs about 2.1 GB for the merged file, plus about 3.6 GB more
if the legacy conversion of section 5 is also produced.

Validated on a 3 chunk subset (2 bz2 plus the plain 1M file): merged to
1,500,000 records / 66,000,032 bytes, read back cleanly by `run7.py`, and
`train_a.py` trained on it without complaint.

## 4. The common regime, and why

The three trainers are different codebases, so "identical" can only mean
identical where it is causal. The invariants held fixed are:

1. **Same corpus.** The same merged 51M record file feeds all three (converted
   for legacy, see section 5).
2. **Same lambda, constant 1.0.**
3. **Same total exposure, about 200M training positions**, which is roughly
   four passes over the corpus.

**Why lambda 1.0.** `docs/spell-nnue-a.md` par. 7.4 prescribes exactly this for
net 1, a pure eval bootstrap. `docs/nnue-training-guide.md` opens with the rule
that lambda outranks epochs and that the right lambda is found by match and LOS
per variant, which makes lambda a second axis and not something to vary inside
an architecture comparison. Fixing it at 1.0 has a third benefit specific to
this tournament: at lambda 1.0 the `game_result` field is never read, so the
legacy conversion cannot silently penalize the legacy entry through a bad
result backfill.

Recorded caveat: the guide's empirical priors, above all Atomic at 0.15 to 0.25
for a tactical and decisive variant, suggest the eventual best lambda for spell
is well below 1.0. That sweep belongs on the winning architecture, after this
tournament, not inside it.

**Why about four passes.** The guide's extinction recipe ran `--max_epochs 9`
at `--epoch-size 20000000`, that is 180M positions, and reports that the best
checkpoints for a 100M corpus landed around epochs 5 and 6. It also states that
differences between epoch counts are normally small. Four passes over 51M
records is 204M positions, which sits in that same productive band while
keeping the A run inside a single working session.

**What cannot be equalized.** Optimizer, learning rate, batch size and the
position skipping policy are baked into each trainer and tuned together. A and
v2 share a chassis (SparseAdam plus AdamW, lr 1e-3, batch 2048); legacy uses
its own optimizer at batch 16384. These stay at each trainer's default and are
listed here as known confounds rather than papered over.

| Knob | A | v2 | legacy |
|---|---|---|---|
| lambda | 1.0 constant | 1.0 constant | 1.0 constant |
| Total positions | 204M | 204M | 200M |
| Batch | 2048 | 2048 | 16384 |
| lr | 1e-3 | 1e-3 | trainer default |
| Seed | 1 | 1 | 1 |
| Estimated wall time | about 5 h | about 14 h | not measured here |

## 5. The three full runs

Paths below use `$DATA` for the merged file from section 3 and assume the
current directory is the repository root of each trainer.

### A (this branch, `spell-a-flat`)

```bash
python tools/spellnnue-pytorch/train_a.py \
  --data "$DATA" \
  --records 51000000 --epochs 4 \
  --start-lambda 1.0 --end-lambda 1.0 \
  --batch-size 2048 --lr 1e-3 --seed 1 --device cuda \
  --out .scratch/spell-a-net1.nnue \
  --curve .scratch/spell-a-net1-curve.json \
  --checkpoint .scratch/spell-a-net1.pt
```

Expected net size about 3.0 MB.

### v2 (SPL3 pipeline, `master`)

```bash
python tools/spellnnue-pytorch/train_overfit.py \
  --data "$DATA" \
  --records 51000000 --epochs 4 \
  --start-lambda 1.0 --end-lambda 1.0 \
  --batch-size 2048 --lr 1e-3 --seed 1 --device cuda \
  --out .scratch/spell-v2-net1.nnue \
  --curve .scratch/spell-v2-net1-curve.json \
  --checkpoint .scratch/spell-v2-net1.pt
```

`--curve` is required by this driver, unlike `train_a.py`. Expected net size
about 99 MB. This driver raises `RuntimeError` when the convergence gate fails,
so a non zero exit is a real result and not a crash.

### legacy (FSF era trainer)

Run from **inside** the trainer directory, see the DLL note in section 5.3.

```bash
cd "C:/Users/djime/Documents/Chess_variants/Codex/Fairy-Stockfish organization/Spell-nnue-pytorch"
python train.py \
  "$BIN/run9-51m-train.bin" "$BIN/run9-51m-val.bin" \
  --lambda 1.0 \
  --max_epochs 10 --epoch-size 20000000 --validation-size 1000000 \
  --batch-size 16384 --random-fen-skipping 0 \
  --num-workers 8 --threads 8 --gpus 1 --seed 1 \
  --default_root_dir logs/tournament-legacy

python serialize.py \
  logs/tournament-legacy/lightning_logs/version_0/checkpoints/last.ckpt \
  spell-legacy-net1.nnue --features "HalfKAv2^"
```

`--random-fen-skipping 0` is deliberate. The trainer's default of 3 keeps
roughly one position in four, which would give legacy a differently sampled
diet from A and v2 over the same corpus. `--smart-fen-skipping` is a no-op in
this fork and is omitted.

10 epochs at an epoch size of 20M is 200M positions, the closest round match to
the 204M of the other two.

## 6. Legacy conversion plan (not executed)

The legacy trainer cannot read run7. This section is what a converter has to
do; **no conversion was run**.

### 6.1 What legacy accepts

`.bin` and nothing else. `lib/nnue_training_data_stream.h` registers a single
reader, `BinSfenInputStream`, with `extension = "bin"`, and returns `nullptr`
for any other suffix. There is no `.binpack` and no `.plain` path in that fork.

The record is `PackedSfenValue`, **76 bytes**, little endian, read as a flat
struct copy. Our own producer wrote this exact format for the run6 series, and
it is already normative in `docs/spell-bin-v1.md`, byte verified against the
FSF spell reference tools. The converter targets that document.

| Offset | Size | Field |
|---|---|---|
| 0 | 64 | `sfen`, a 512 bit packed position |
| 64 | 2 | `score`, i16, side to move POV, clamp +/-32000, never a mate score |
| 66 | 2 | padding, always 0 |
| 68 | 4 | `move`, u32 engine encoding |
| 72 | 2 | `gamePly`, u16 |
| 74 | 1 | `gameResult`, i8, +1/0/-1 from the POV of the side to move |
| 75 | 1 | padding, always 0 |

`sfen` bit order, LSB first inside each byte: side to move (1), white king
square (7), black king square (7), board scan rank 8 down to 1 and file a to h
skipping both king squares (1 bit `0` for empty, else a 5 bit Huffman code plus
1 colour bit), hands as 2 colours x 8 slots x 5 bits, **potion blocks as 2
colours x 2 types x (1 bit present, 7 bit zone centre, 16 bit cooldown)**,
castling (4), en passant present (1, plus 7 for the square), rule50 low bits
(6), fullmove low byte then high byte (8 + 8), rule50 bit 6 (1).

### 6.2 Conversion checklist

1. **Source** is the 44 byte run7 record defined by
   `tools/spellnnue-pytorch/run7.py`; read it with that module rather than
   reimplementing the unpack.
2. **The potion block sits between the hand counts and the castling rights.**
   Anything that omits it desynchronizes every later field.
3. **Zones are stored as a centre, not as a mask.** The legacy decoder expands
   it: freeze is the 3x3 neighbourhood clipped to the board, jump is the single
   centre square. Confirm run7's `gates` values are centres under the same
   convention before trusting the round trip.
4. **Resolve the hand slot convention explicitly.** The run6 producer wrote
   slot 6 as Freeze and slot 7 as Jump, but the legacy feature loop stops at
   `King == MaxPiece == 7`, so slot 7 never becomes a feature while
   `variant.py` documents a different order. Left as is, Jump in hand is
   invisible to the legacy net. This is a real handicap to decide on
   deliberately, not to discover after an SPRT.
5. `score` clamps to +/-32000 and must contain no mate scores.
6. `gameResult` is unused at lambda 1.0 but should still be filled correctly so
   the artifact stays usable for a later lambda sweep.
7. **Validate before training** with `tools/psv_decode.py`, the existing
   validator for this format, and diff a handful of decoded positions against
   the FEN that `run7.to_fen` produces for the same record.
8. Produce two files: `run9-51m-train.bin` from all 51M records, about 3.6 GB,
   and `run9-51m-val.bin` from the 1M HF slice, about 76 MB. The legacy trainer
   requires a validation positional argument. That slice is also inside the
   training corpus, so its validation loss is diagnostic only and must not be
   used to pick a checkpoint; selection is by SPRT.
9. No header is involved. The legacy reader consumes a bare concatenation of
   records, so shards can simply be appended.

### 6.3 Two traps in that repo

- `nnue_dataset.py` globs `./*training_data_loader.*` **relative to the process
  working directory** and takes the first hit. There is an older, pre potion
  DLL one directory above the trainer. Launching from the wrong place silently
  loads a loader that decodes the 76 byte record with the wrong schema, no
  potion block and 6 piece types. Always launch from inside
  `Spell-nnue-pytorch`.
- Variant configuration is compile time. Changing it means editing `variant.h`
  and rebuilding via `compile_data_loader.bat`. The shipped DLL already matches
  the spell configuration (`PIECE_TYPES 8`, `POCKETS true`, `HAS_POTIONS 1`,
  `DATA_SIZE 512`), so it should be used as is.

This is the same failure mode as the cautionary tale in
`docs/nnue-training-guide.md`, where the first spell NNUE attempt trained on a
dataset with no potions at all because `variant.h` was still in chess mode.
Validate the decode before training, every time.

## 7. Gates after each full run

1. **Parity, mandatory before any SPRT.** For A,
   `parity_a.py --net <net> --data <run7> --count 1000`, zero diffs required.
   For v2 the equivalent is `parity.py`. Both compare the engine against the
   integer Python reference, so a mismatch means the exported file and the
   engine disagree and the net is not testable.
2. **Bench sanity**, that the engine loads the net and reports a plausible nps.
3. **SPRT** against the current default, `[0, 5]` first to find out whether the
   architecture is competitive at all, then `[1, 6]` and the formal 3 TC panel
   if it passes.
