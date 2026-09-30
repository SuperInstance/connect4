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

**No dataset yet.** The pure-Python solver is correct and does not finish the full game in a
session. The next step is the canonical C bitboard solver, not a better Python one.
