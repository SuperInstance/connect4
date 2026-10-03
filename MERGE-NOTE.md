# Merge note — what this merge did and did not establish

Two diverged branches were merged on 2026-10-02. This file exists because the
interesting part of a provenance merge is usually the part that is **not**
verified, and because a merge is the easiest place to accidentally ship a claim
nobody checked.

## What each side carried

| branch | carried |
|---|---|
| `main` (943bc54) | the C solver addendum — **five bugs, each of which produced a plausible number** · `docs/GPU-EXPERIMENT.md` · `c4_ground_truth.txt` |
| `backup-local-2026-10-02` (7c70e93) | the **C11 bitboard solver** (1,387 lines vs 471) · `MANIFEST.md` · `Makefile` · `census.log` / `census.txt` · a `.gitignore` |

## Resolved, and how

- **`ctool.c`** — took the backup's bitboard solver. It is the generator that
  produced the exported ground truth, so it supersedes the 471-line version the
  addendum was written against.
- **`census.txt`, `README.md`, `MANIFEST.md`, `Makefile`, `.gitignore`** —
  merged automatically; no conflict.

## NOT VERIFIED, and this is the part that matters

> **A merge is not a proof that the five known bugs were fixed in the replacement
> solver.**

The addendum documents five bugs found in the 471-line version. The bitboard
solver is a different, larger implementation. **Nothing in this merge
establishes that it does not contain the same five, and no C was executed here
to check.** The word "bugfix" must not be read into this merge.

**How to actually check it**, when a C toolchain is available:

```bash
make            # or: cc -O2 -o ctool ctool.c
./ctool         # does the export still match c4_ground_truth.txt?
diff <(./ctool --export) c4_ground_truth.txt && echo "export unchanged"
```

**If the export does not match byte-for-byte, the bitboard solver has a bug the
addendum does not know about**, and that is the highest-value thing in this repo
to find.

## The five doctrines, which apply to this merge too

From `docs/GPU-EXPERIMENT.md` on `main` — and they are the same five this
project arrived at independently elsewhere, which is itself the interesting part:

1. **State whether CUDA or CPU actually ran.** A fallback reported as the real
   thing is a lie with a config file.
2. **Report the variance, not just the mean.** `std == 0` means INCONCLUSIVE.
3. **The ground truth must be computed, not asserted.**
4. **Non-degeneracy is a precondition, not a result.**
5. **A control must vary the thing it audits, by a different path than the
   audited thing.**

And the rule that governs this file: **a merge is a change of provenance, not a
verification.** Everything a merge does *not* check should be written down where
the next reader will hit it, rather than left implied in a tidy commit message.
