# Connect 4: rung two, and what it is actually worth

## A correction to the seed, before any code

The seed says: *"John Tromp strong-solved the game in 1995 by mapping the perfect
game-theoretic value for every one" of 4,531,985,219,092 positions.*

Two things are true and they are worth separating:

- **4,531,985,219,092 is real.** It is the count of legal 7x6 positions under the
  *broader* position convention, from OEIS **A212693**, credited to **Edelkamp & Kissmann**
  (2008), not to Tromp. Tromp's own playground page notes that Googling the number turned
  up their result and that he had not expected independent verification.
- **Tromp's actual published 1995 artifact is the 8-ply subset** — 67,557 unfinished,
  unforced positions. The cumulative count to 12 plies is 18,102,767.

So "4.5 trillion positions, attributed to Tromp, published as a dataset" is three true
things composed into a claim nobody can act on. **The tractable, citable ground truth is
8-12 plies**, and that is what this should build on.

## The pre-registered prediction

`pie-minimax` measured, exactly, that a linear model on a solved board reaches 0.1807
against a floor of 0.1431. Optimal play is *compositional* — "if I block here, they play
there, and then I am losing on a line I did not see" — and a linear map is a sum of
independent per-cell votes with no composition between cells.

Connect 4 asks the same question of a real network and a real search. The prediction,
written down before running:

> **Split the test set by how much composition the position requires.**
> **SIMPLE** — the side to move has 0 or 1 immediate winning move. Locating one winning move
> is a local geometric pattern, "count four", and a single linear term can express it.
> **COMPOSED** — two or more simultaneous winning moves, i.e. a fork, which forces a threat
> count. That count *is* parity, and it is exactly what a sum of independent votes cannot do.
>
> If the tic-tac-toe finding carries, accuracy on COMPOSED sits near the floor while SIMPLE
> is much higher.

The seed's competing claim is that a network prunes to 10% and still plays well, which
would mean the composition was never needed. The two cannot both be true, and the
composition count makes the test decidable rather than rhetorical.

## The solver is correct, and the representation had to be abandoned first

`c4.py` is a flat tuple-of-tuples with explicit heights — deliberately not the 49-bit
bitboard, because **five versions of the bitboard all failed in ways that looked like
results**:

1. `legal_cols = (p0+p1) & BOARD` is 0 on an empty board, so the enumerator produced
   exactly one position and reported no error.
2. `BOTTOM_MASK` at row H-1 instead of row 0, so pieces stacked from the second-from-top.
3. Sentinel rows excluded from `BOARD`, so the `+ bottomMask` carry spilled into the next
   column and a full column looked empty.
4. `wins()` not masking the sentinel rows, which manufactures vertical four-in-a-rows.
5. `new_board()` returning lists — unhashable, so the memo could not work at all.

Each one ran, and each produced a plausible number. **A representation you cannot reason
about is a measurement instrument you cannot check.**

Verified once the representation was made obvious: the empty board evaluates to −1 (the
second player wins, the known result) and three-in-the-centre evaluates to +1.

## What is not delivered, and why

**No dataset, and no accuracy numbers.** The flat solver is correct and the full game does
not finish in a session in pure Python — the 13-ply horizon on the empty board is billions
of nodes. Generating 8-12 ply ground truth needs the canonical **C bitboard solver with an
opening database**, which is exactly what exists and exactly what my five failed Python
versions were failing to re-derive.

That failure is itself a result worth recording: **the obvious implementation is the one
that is wrong, and the correct one is the one you import.** Same shape as the linter that
flags everything, and the control that cannot fail.

**The next step is not a better Python solver. It is the C one plus a ply-bounded export.**

## What carries to the rest of the ladder

The **label-set problem** gets worse as the board grows, not better. In tic-tac-toe 14.7% of
positions had several equally-correct moves. Connect 4 has a 7-wide move space and common
draws, so the set-valued loss `-log Σ_{m ∈ Opt} softmax(z)_m` is not a refinement — it is
the difference between training on truth and training on noise. **Any rung of this ladder
that distils from a solver inherits it**, and it will present as "the network is bad at
Connect 4" rather than "the labels were wrong".
