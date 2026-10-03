# ctool.c -- Connect 4, exact, ply-bounded

Single self-contained C11 program, no dependencies, no libraries beyond libc.
Integer-only arithmetic, so the output is bit-identical on any machine.

    make            # build ./ctool
    make selftest   # representation + layout + encoding checks, no solving
    make count      # distinct legal position census, no solving
    make export     # checks, known-answer checks, solver validation, export

## What it is

The canonical C bitboard solver the Python work in this repo said was needed:
49-bit Pons bitboards, one `uint64_t` per player with a sentinel row per column,
negamax + alpha-beta + a transposition table, and a **ply-bounded ground-truth
export**.

The design principle, inherited from five failed Python bitboards that each ran
and each produced a plausible number while being wrong:

> **A representation you cannot reason about is a measurement instrument you
> cannot check.**

So every derived constant is computed at init and asserted, both bit tricks are
validated against a dead-simple flat-array reference over *every* position to a
reference ply, and the fast search is validated against simpler searches. The
output is not trusted; it is compared.

## The two known-answer checks

| check | specified | measured |
|---|---|---|
| A: three P1 stones stacked in the centre column, P1 to move | `+1` | **`+1`** — PASS |
| B: the empty board, P1 to move | `-1` (second player wins) | see below |

**Check A passes but is much weaker than it looks.** Three stacked in the
centre means P1 wins on the very next drop, so the position is decided in
**1 node**. It verifies win detection and the `+1` convention; it does not
exercise the search at all. Worth saying plainly, because "the known-answer
check passed" is otherwise a much bigger claim than it is.

**The two specified checks are mutually inconsistent.** Check A asserts a first
move that *wins* for P1. If any first move wins, the empty board is at least
`+1` for P1, and therefore cannot be `-1`. At most one of the two specified
values can be right. The program prints this rather than picking one.

**Check B did not complete in this session.** The value of the empty board is
the max over all seven one-stone positions, and each of those is a full solve of
the game from ply 1. On the one-core, 512 MiB-TT sandbox this run had, that is
hours. `--kat-seconds` caps the attempt; when it fires the program prints the
measured node rate and asserts **no** value, rather than reporting a number it
did not earn. See the run log for the rate actually measured.

## What the export achieves, and why that is the honest answer

**Every value in the export is exact.** There is no horizon: each value is the
exact value of the fully solved game, searched to the end of the game tree. No
row is a "draw assumed at the horizon" row. That is also why the ply bound is
low — the ply-2 stage alone is 49 full game solves, and they are not cheap.

`--ply-seconds` abandons a ply that overruns its budget at a position boundary,
unlinks the partial file, and reports the last ply that **completed**. The bound
is reported as achieved, never quietly lowered. If the achieved bound is lower
than requested, the manifest says so in the same line.

The reason the bound is low is worth stating as a number rather than an
adjective: this ran on **1 core with ~2 GiB RAM and a 512 MiB transposition
table**, and it was CPU-throttled to roughly half speed. A ply-12 Connect 4
build is a multi-core-days computation. The tool is the right tool; the box was
not the right box.

## The checks

| check | scope | result |
|---|---|---|
| FNV-1a 64 | published test vectors, string vs byte-loop, incrementality across lines | 0 mismatches |
| value encoding | ordering, `V_DIST` inversion, `mirror` order-reversal, `V_SIGN` | 0 mismatches |
| bitboard layout | computed masks, popcounts, sentinel bits, gravity, no column carry | 0 mismatches |
| primitives vs flat-array reference | `is_win`, `can_win_next`, `n_threats`, gravity — on **every** distinct legal position to ply 9 (797,388 positions) | 0 mismatches |
| TT key `cur+occ` injectivity | **every** state to ply 11 (6.7M states) | 0 collisions |
| 6a threat reductions on vs off | every position to the validation ply, on **independent TT banks** | see manifest |
| 6b production vs TT-free **and** pruning-free | a sample of late-game positions, where a TT-free search is affordable | see manifest |
| 7b Bellman self-consistency | every exported value re-checked against the game's own recursion, using the children the search already cached | see manifest |

### 7b, and why it is the one that matters

The other checks compare solver variants against each other. **That is blind to
a bug the variants share.** Three bugs in this project were shared by every
variant and passed every variant-vs-variant check:

1. `can_win_next(opponent) → LOSS(1)` — unsound; the threatened square can be
   **blocked**. (Fixed in all three programs.)
2. `mirror()` did not add a half-move for the move into the child, so every
   distance-to-win was wrong by one per recursion level. Signs often survived
   by luck.
3. The child call passed `occ ^ cur ^ bit`, handing the stone just played to
   the **opponent**. Every value wrong, and the search ~113x slower than it
   should be.

What catches those is checking the values against the *rules of the game*:

    value(P) = max over moves m of ( m wins now ? +1 : −value(P after m) )

In the sibling 4x4 program, where the table is complete, this is a check of all
9,067,975 positions and it found 4,572,569 violations before bug 3 was fixed
and 0 after. Here the export is ply-bounded so the children are not all in the
table; 7b uses the children's cached TT entries and reports how many positions
it could verify and how many it had to skip, so its coverage is a measured
number and not an implied one.

## Conventions

Also in every export file's `#` header and in the manifest. An unstated
convention is how a ground-truth table becomes a wrong training set.

- **value**: `+1` P0 wins with best play, `0` draw, `-1` P0 loses, from **P0's**
  perspective. The export contains only even plies, which is exactly the set of
  positions with P0 to move, so this is also "the value for the side to move".
  Odd plies are P1 to move and are out of scope for this export.
- **horizon: none.** Stated on every row of every file, because `+1` and
  `+1 within N plies` are different claims. `c4.py` reports `0` on the empty
  board because its 11-ply horizon decided it, not the game.
- **legal position**: reachable by alternating drops, and the game has not
  already ended (the opponent has not just made a four, the board is not full).
  Terminal positions are **excluded** and counted in the manifest.
- **order**: lexicographic in the first column-drop sequence that reaches the
  position, columns 0..6 left to right, deduplicated on the position itself.
  Determined by the board alone, so the file is byte-identical on any machine.
- **line format**: exactly three decimal fields, `p0 p1 value`, newline
  terminated. `#` lines carry the conventions. No fourth column.
- **digest**: FNV-1a 64, hand-rolled, no library, over the exact ASCII bytes of
  every data line of every ply file in ply order, terminating newlines included,
  `#` lines excluded. It is **recomputed from the files on disk** at the end of
  the run, so it always describes the deliverable rather than the intent.

## Three representation notes, each of which was a bug first

- **`STRIDE` is `HEIGHT+1`, not `WIDTH+1`.** 7 bits per column. With 8, the
  masks still "look" right and the failure only shows up as positions no flat
  reference agrees with.
- **In Connect 4 a column's colours are not constrained** beyond gravity — a
  column may read `X O O X`. An early revision assumed alternation (true of
  tic-tac-toe-style placement, false here) and dropped half the positions.
- **`drop_bit` must extract from `possible_mask`, not from `occ + BOTTOM_MASK`
  masked with the column's row-0 bit.** The latter is nonzero exactly when the
  column is *empty*, so it returns a square only for empty columns and silently
  truncates the game tree.

And one that is the reverse — a trick that is correct in one program and wrong
in the other, which is why it is worth writing down: `cur + occ` is injective
for Connect 4 (per-column values are disjoint ranges, max `0x7e7e7e7e7e7e7e`, so
49 bits, not 42 and not 43) but **not** for 4x4 placement, where
`occ=0b01, cur=0b01` and `occ=0b10, cur=0` both sum to `0b10`.
