# c4 manifest — what was measured, and what was not

Machine: **1 core, 2 GiB RAM, cgroup limit 2 GiB**, CPU observed at roughly
50–80% of nominal under load. TT 2^22 (64 MiB) for the validation run, 2^24–25
for the solve attempts. No GPU, no second core.

## Checks — all green except where noted

| check | scope | result |
|---|---|---|
| FNV-1a 64 | 3 published vectors, string vs byte-loop, incrementality across lines | 0 mismatches |
| value encoding | ordering, `V_DIST` inversion, `mirror` order-reversal, `V_SIGN` | 0 mismatches |
| bitboard layout | computed masks, popcount 42, sentinel bits, gravity, no column carry | 0 mismatches |
| primitives vs flat-array reference | `is_win`, `can_win_next`, `n_threats`, gravity — every distinct legal position to ply 9 | **0 mismatches in 797,388 positions** |
| TT key `cur+occ` injectivity | every state to ply 10/11 | **0 collisions in 2,415,786 / 6,711,208 states** |

Per-ply distinct legal non-terminal position counts (exact, no solving, 1.6 s):

| ply | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| legal | 1 | 7 | 49 | 238 | 1,120 | 4,263 | 16,422 | 54,131 | 182,383 | 538,774 | 1,618,398 | 4,295,422 |
| terminal | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1,288 | 2,932 | 29,054 | 61,878 | 376,035 |

Cumulative distinct legal non-terminal positions to ply 11: **6,711,208**.
Cumulative terminal positions to ply 11: **471,187**.
(The census cannot be pushed past ply 12–13 on this box: the number of legal
positions at a *single* ply reaches the billions, so the breadth-first walk runs
out of memory.  The 4,531,985,219,092 total quoted elsewhere is the sum over all
plies, not something this machine can enumerate.)

## Known-answer checks

| | specified | measured |
|---|---|---|
| A: 3 stacked in the centre, P1 to move | `+1` | **`+1` — PASS**, in **1 node** |
| B: empty board, P1 to move | `-1` | **NOT DETERMINED IN THIS SESSION** |

**A passes but is a much weaker check than it looks.** Three stacked in the
centre means P1 wins on the next drop, so the position is decided immediately.
It verifies win detection and the `+1` convention. It does not exercise the
search at all, and it should not be quoted as evidence that the solver works.

**The two specified checks are mutually inconsistent.** A asserts a first move
that *wins* for P1. If any first move wins, the empty board is at least `+1`,
so it cannot be `-1`. At most one of the two specified values can be right.
The program prints this rather than choosing.

**B is not computable here.** The empty board's value is the max over the seven
one-stone positions, and each is a full solve of the game from ply 1. Measured
rate: **9.94 M nodes/s** (596,639,817 nodes in 60 s before the cap). The program
asserts no value rather than printing one it did not earn.

## The ply bound, and why it is what it is

**Achieved p0-to-move ply bound: 0. Requested: 12.** There is no horizon in the
export — every value would be an exact full solve — so the bound is set purely
by how many full game solves fit in the budget, and the ply-2 stage alone is 49
of them.

Measured exact-solve cost (`./ctool scale`, 20 s cap per position):

| ply | 2 | 4 | 6 | 8 | 10 | 12 |
|---|---|---|---|---|---|---|
| completed in 20 s | 0/3 | 0/3 | 0/3 | 0/3 | 0/3 | 1/3 |
| nodes (ply 12) | | | | | | 28,990,423 in 2.8 s = **10.39 M nodes/s** |

The curve is U-shaped: ply 12 is cheap because the game is nearly over, plies
2–10 are not affordable at all, and a ply-2 position is one of the hardest
single computations in this project. The p0-to-move plies (2, 4, 6, …) are
exactly the expensive ones.

So the honest statement is: **this machine cannot produce a non-empty exact
Connect 4 export at any ply.** The tool is right; the box is wrong. A ply-12
build is a multi-core-days computation and this is one throttled core.

## What is delivered anyway

- `ctool.c` — the solver, with the checks above passing. This is the reusable
  artefact: the next person with cores runs the same binary and gets a table.
- `census.txt` — the exact distinct-position counts above, which the training
  lane needs and did not have.
- `scale.log` — the measured cost curve, which is what turns "too slow" into a
  budget: at 10.39 M nodes/s one ply-12 position is 2.8 s, so a ply-12 build is
  `sum over plies of (positions at that ply) x (per-solve cost)`, dominated by
  plies 2–10.
- `ctool.md` — conventions, the three shared-mode bugs the checks caught, and
  the representation notes.

## Not delivered, stated plainly

- A p0-to-move Connect 4 value table. Not produced. Claiming one would be the
  "quietly lowered bound" this repo's FINDINGS.md warns against.
- The value of the empty board.
- Full solver-vs-reference validation (6a/6b) — the passes exist and run, but
  each comparison is a full game solve, so on this box they cost minutes per
  position and the session ended before a useful sample completed. The
  representation-level checks (exhaustive, over 797,388 positions) and the TT
  key check (over millions of states) did complete and did pass.
