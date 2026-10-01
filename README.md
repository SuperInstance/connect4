# connect4

Rung two of the training ladder: can a network absorb a solved game with **zero search**,
and if not, what exactly is it missing?

- `c4.py` — exact negamax solver with a documented 13-ply horizon. Verified on the two
  positions that pin it down.
- `truth.py` — the ground-truth generator and the composition classifier the test needs.
- `FINDINGS.md` — the citation correction, the pre-registered prediction, the composition
  test, and five implementation failures that each looked like a result.

**The composition test is the experiment.** Split positions by how much composition they
require — one immediate win (local, "count four") against two or more (a fork, which forces
a threat count, which *is* parity) — and ask whether accuracy collapses on the second.
`pie-minimax` already showed a linear model cannot compose, exactly, on a board where the
optimum is computed. This asks the same question of a real network.

## Status: the C solver is here; the Connect 4 table is not

- `ctool.c` — the canonical C bitboard solver: 49-bit Pons bitboards, negamax +
  alpha-beta + a transposition table, self-contained C11, no dependencies.
  **It verifies itself before it does anything else**, and the checks pass:
  win/threat detection against a flat-array reference on every distinct legal
  position to ply 9 (797,388 positions, 0 mismatches), TT-key injectivity over
  every state to ply 11, FNV-1a against published vectors.
- `ctool.md` — conventions, and the three shared-mode bugs the checks caught.
- `MANIFEST.md` — the measured numbers, including what was **not** achieved.
- `census.txt`, `scale.log` — the exact distinct-position counts and the
  measured exact-solve cost curve.
- `Makefile` — `make selftest`, `make count`, `make export`.

**Still no p0-to-move table, and the reason is the machine, not the tool.** The
export has no horizon — every value would be an exact full solve — so each ply
stage is N full game solves, and the p0-to-move plies are exactly the expensive
ones. On the one throttled core this ran on, 0 of 3 sampled positions finished
an exact solve in 20 s at plies 2, 4, 6, 8 and 10, while ply 12 finished one in
2.8 s. The achieved bound is reported as 0, not quietly lowered.

**The 4x4 rung now has complete ground truth** — see `../ga4444`, 9,067,975
positions, every legal position at every ply, verified against the game's own
recursion with 0 violations. The composition test can start there.
