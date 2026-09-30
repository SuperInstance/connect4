"""Ground truth, and the ONE MEASUREMENT that carries the whole experiment.

THE PREDICTION, pre-registered from pie-minimax. Tic-tac-toe showed a linear model gets
0.18 against a floor of 0.14 -- it cannot compose. Tic-tac-toe optimal play is
COMPOSITIONAL: "if I block here, they play there, and then I am losing on a line I did not
see". Connect 4 asks the same question with a real network and a real search.

So: SPLIT THE TEST SET BY HOW MUCH COMPOSITION THE POSITION REQUIRES.

    SIMPLE      the current player has 0 or 1 immediate winning move.
                Locating one winning move is a LOCAL pattern: a visual, geometric,
                "count four" object. A single linear term over the board can express it.

    COMPOSED    the current player has 2 or more immediate winning moves, or the opponent
                has an unstoppable double threat. Answering these requires counting
                threats ACROSS the board and reasoning about which one the opponent gets
                after your reply. That is parity, and it is exactly the composition a
                linear map cannot represent.

If the tic-tac-toe finding carries, accuracy on COMPOSED positions will be near the floor
while SIMPLE positions are much higher. That is the prediction. The seed's competing
claim is that a deep network prunes to 10% and still plays well -- which would mean the
composition was never needed.

The seed names the failure as PARITY. This measures the same thing in a way that cannot be
argued with: the number of simultaneous winning moves IS the parity count.
"""
from __future__ import annotations
import random, sys, time
sys.path.insert(0, '/workspace/projects/connect4')
import c4


def winning_moves(p0, p1):
    """Every column in which the side to move would win immediately."""
    free = c4.legal_cols(p0, p1) & c4.BOARD
    out = []
    m = free
    while m:
        b = m & -m
        c = b.bit_length() - 1
        c //= (c4.H + 1)
        out.append(c)
        m ^= b
    return out


def generate(max_plies=11, n_target=40000, seed=0):
    """Enumerate legal positions to a ply limit, keeping only balanced (unforced) ones,
    and record the exact value plus the composition class."""
    rng = random.Random(seed)
    seen, rows = set(), []

    # negamax(p0, p1) is the value with P0 TO MOVE, so only those positions are recorded.
    # The first version recursed with a broken conditional and returned ZERO positions in
    # 0.0s with no error. A generator that yields nothing and reports nothing is the same
    # failure as a control that cannot fire.
    def legal(p0, p1):
        return [c for c in range(c4.W) if c4.can_play(c, p0, p1)]

    def walk(p0, p1, plies, p0_to_move):
        if plies >= max_plies or len(rows) >= n_target:
            return
        if c4.wins(p0) or c4.wins(p1):
            return
        if p0_to_move:
            my_moves = winning_moves(p0, p1)
            opp_moves = winning_moves(p1, p0)
            v = c4.negamax(p0, p1)
            k = (p0, p1)
            if k not in seen:
                seen.add(k)
                cells = []
                for c in range(c4.W):
                    for r in range(c4.H):
                        bit = 1 << (c * (c4.H + 1) + r)
                        if p0 & bit: cells.append((r, c, 1))
                        elif p1 & bit: cells.append((r, c, -1))
                # SIMPLE  = at most one immediate win available to EITHER side. Finding one
                # winning move is a local geometric pattern a linear term can express.
                # COMPOSED = two or more, which is a fork and forces a threat count.
                composed = (len(my_moves) >= 2) or (len(opp_moves) >= 2)
                rows.append({"p0": p0, "p1": p1, "plies": plies, "value": v,
                             "my_moves": my_moves, "opp_moves": opp_moves,
                             "composed": composed, "cells": cells})
            for c in legal(p0, p1):
                walk(c4.play(p0, c), p1, plies + 1, False)
        else:
            for c in legal(p0, p1):
                walk(p0, c4.play(p1, c), plies + 1, True)
    walk(0, 0, 0, True)
    return rows


if __name__ == "__main__":
    t = time.time()
    rows = generate(max_plies=11, n_target=30000, seed=0)
    comp = sum(1 for r in rows if r["composed"])
    print(f"  generated {len(rows)} positions in {time.time()-t:.1f}s")
    print(f"    COMPOSED (>=2 simultaneous wins either side): {comp}  ({comp/max(1,len(rows)):.1%})")
    print(f"    SIMPLE                                  : {len(rows)-comp}")
    import collections
    print(f"    value histogram: {dict(sorted(collections.Counter(r['value'] for r in rows).items()))}")
