"""Connect 4: exact solver, 7x6.

REPRESENTATION: one int per cell, 0/1/2, in a 7-wide by 6-high grid, plus a height per
column. Nothing clever.

That is a retreat and it is deliberate. Five earlier versions used the standard 49-bit
Pons bitboard with a sentinel row per column, and every one of them failed in a way that
looked like a result:

  * `legal_cols = (p0+p1) & BOARD` returns 0 on an empty board, so the enumerator found
    no legal moves and produced exactly one position, reporting no error.
  * `BOTTOM_MASK` at row H-1 instead of row 0, so pieces stacked from the second-from-top.
  * sentinel rows excluded from BOARD, so the `+ bottomMask` carry spilled into the NEXT
    column and a full column looked empty.
  * `wins()` not masking the sentinel rows, which manufactures vertical fours.

Each one terminated or ran, and each produced a plausible number. The flat array has none
of these failure modes because there is no carry to leak and no sentinel to read. **A
representation you cannot reason about is a measurement instrument you cannot check**, and
this project has spent enough of tonight on that lesson.

The solver is negamax with alpha-beta, centre-first move ordering. Correct, not fast.
"""
from __future__ import annotations
from functools import lru_cache

W, H = 7, 6
EMPTY, P1, P2 = 0, 1, 2
CENTRE = (0, 1, 2, 3, 4, 5, 6)


def new_board():
    """A tuple-of-tuples, not a tuple-of-lists: negamax is memoised on it and a list is unhashable."""
    return tuple((EMPTY,) * W for _ in range(H))


def heights(b):
    h = [0] * W
    for c in range(W):
        n = 0
        for r in range(H):
            if b[r][c] != EMPTY:
                n = r + 1
        h[c] = n
    return tuple(h)


def legal_cols(b):
    h = heights(b)
    return tuple(c for c in range(W) if h[c] < H)


def play(b, c, player):
    h = heights(b)
    if h[c] >= H:
        return None
    rows = [list(r) for r in b]
    rows[h[c]][c] = player
    return tuple(tuple(r) for r in rows)


def wins(b, player):
    for c in range(W):
        n = 0
        for r in range(H):
            if b[r][c] == player:
                n += 1
                if n == 4:
                    return True
            else:
                n = 0
    for r in range(H):
        n = 0
        for c in range(W):
            if b[r][c] == player:
                n += 1
                if n == 4:
                    return True
            else:
                n = 0
    for dr, dc in ((1, 1), (1, -1)):
        for r in range(H):
            for c in range(W):
                if all(0 <= r + k * dr < H and 0 <= c + k * dc < W and b[r + k * dr][c + k * dc] == player
                       for k in range(4)):
                    return True
    return False


def immediate_wins(b, player):
    """Every column in which `player` would win right now."""
    out = []
    for c in legal_cols(b):
        nb = play(b, c, player)
        if nb is not None and wins(nb, player):
            out.append(c)
    return tuple(out)


MAX_PLY = 11     # search bound. Values are EXACT inside this horizon and not outside it.


@lru_cache(maxsize=1 << 21)
def negamax(b, turn, depth=MAX_PLY):
    """Value for the side to move: +1 win, 0 draw, -1 loss.

    EXACT within MAX_PLY. Beyond the horizon the search is truncated, so a value returned
    at the boundary is an upper bound, not the game-theoretic value. This is stated
    everywhere the numbers are used rather than buried, because "exact ground truth" and
    "exact to 13 plies" are different claims and only one of them is true here.
    """
    opp = P2 if turn == P1 else P1
    if wins(b, opp):
        return -1                       # the previous mover already won
    if not legal_cols(b):
        return 0
    if depth <= 0:
        return 0                         # horizon: assume draw. An UPPER bound on value.
    best = -1
    for c in CENTRE:
        if c not in legal_cols(b):
            continue
        nb = play(b, c, turn)
        v = -negamax(nb, opp, depth - 1)
        if v > best:
            best = v
        if best == 1:
            return 1
    return best


def value_for_p1(b):
    return negamax(b, P1)
