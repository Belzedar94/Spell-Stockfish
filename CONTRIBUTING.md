# Contributing to Spell-Stockfish

Thanks for your interest in the project. This page describes how a change gets
from an idea to `main`.

## Proposing a patch

- Branch off `main`. It is the default branch and the baseline every test runs
  against.
- Name the branch `SB<n>-<slug>`, for example `SB4-spell-see`.
- Keep one idea per branch. Independent branches are far easier to test and
  review than a stack of changes that only makes sense as a whole, and a
  single-idea diff gives a clean answer about whether that idea works.
- Explain in the pull request what the change does and why you expect it to
  help.

## Building and testing locally

Build a fresh binary and run the rules suite against it:

```sh
cd src
make -j build ARCH=x86-64-bmi2
```

```sh
cd tests
python3 spell_tests.py ../src/stockfish
```

`x86-64-bmi2` assumes a CPU with BMI2/PEXT support; `make help` lists the other
architecture targets. For a wider check before opening a pull request,
`python3 run_suite.py --quick` adds the protocol, reproducibility and depth-1
perft gates that CI also runs.

## Testing for strength

Functional changes are measured with an SPRT on our OpenBench instance at
<https://belzedar.duckdns.org>. Run the short time control first; if it passes,
run the long one on the same branch.

| Stage | Time control | SPRT bounds  |
| ----- | ------------ | ------------ |
| STC   | 8+0.08s      | [0.00, 3.00] |
| LTC   | 40+0.4s      | [0.00, 2.50] |

Link both tests in the pull request.

Non-functional changes — refactors, comments, documentation, build fixes — do
not need a strength test. Say so in the pull request description.

Rules fixes are a separate case. When a change makes the engine match the
reference behaviour (chess.com's Spell Chess), an A/B against a base that does
not implement the rule cannot measure anything meaningful. Those changes are
accepted on correctness evidence instead: the perft numbers, the parity run
against the reference, and whatever else shows the new behaviour is the right
one. Include that evidence in the pull request.

## Bench numbers

Two different numbers are involved, and mixing them up is the most common
mistake:

- The `Bench:` trailer in the commit message is the number from a **plain**
  build, with no network assigned.
- When you register the test on OpenBench, use the bench measured with the
  network that test will use — an `EVALFILE=<net>` build made after
  `make clean`. Building on top of stale object files produces a bench that
  looks plausible and is wrong, which then fails the build check on the worker.

The neural network is not stored in the repository. Tests get the current
champion net assigned to them.

## Pull requests

- Open the pull request against `main`.
- CI has to be green before it can be merged.
- Once merged, `main` becomes the new baseline for every later test.

## Code style

C++ changes should follow the style in [`.clang-format`](.clang-format). Running
`make format` from `src` applies it for you.

## License

By contributing you agree that your contributions are licensed under the GNU
General Public License v3.0, the same as the rest of the project. See
[Copying.txt](Copying.txt).
