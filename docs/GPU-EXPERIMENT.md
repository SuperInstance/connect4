# GPU experiment brief — `connect4`

**Set-valued labels on a 7-wide move space**

This is the slice of the master queue that concerns this repository. The full document,
with all ten experiments and their decision trees, is at
[`SuperInstance/fleet-triage` → `docs/GPU-EXPERIMENTS.md`](https://github.com/SuperInstance/fleet-triage/blob/main/docs/GPU-EXPERIMENTS.md).

1. **State whether CUDA or CPU actually ran.** A CPU fallback reported as a GPU result is a
   fabricated measurement. Record the device string with the number.
2. **Report the variance, not just the mean.** If the measured quantity has `std == 0` over
   the sample, the result is **INCONCLUSIVE, never PASSED**. This is a hard rule: an earlier
   sharding experiment scored `PASSED` on the literal comparison `0 < 0`.
3. **The ground truth must be computed, not asserted.** Every game number below is exact.
   If a label is approximate, say by how much.
4. **Non-degeneracy is a precondition, not a result.** Assert that the data has variance
   *before* evaluating any relational claim about it.
5. **A control must vary the thing it audits, by a different path than the audited thing.**
   The most expensive mistake in this project's history: a control built from the same call
   path as the probes, which therefore confirmed a fault instead of auditing it.
6. **A ratio whose denominator can be zero is a construction, not a measurement.** It always
   produces a finding, and the finding is always false.
7. **Seed everything and record the seed.** Two runs of the same experiment that differ are
   a finding about the seed, not about the method.
8. **Cross-port arithmetic must preserve the reference loop/summation order.** Float addition
   is not associative; a port that vectorises changes the answer.

---

### Experiment 7 — Set-valued labels on a 7-wide move space

**Repo:** `connect4`. This is the **ladder-wide consequence**, and it gets worse, not
better, at each rung.

3×3 has 14.7% of states with multiple optimal moves. Connect 4 has a 7-wide move space and
common draws, so the multi-optimal fraction is far higher. A label set that assigns **one**
optimal move relabels every other optimal move as an error.

**This is the difference between training on truth and training on noise**, and it presents
as "the network is bad at Connect 4."

| Result | What it means |
|---|---|
| Set-recall stays high while top-1 drops | The network learned the value function and not a single arbitrary policy. **This is a success, and reporting only top-1 would call it a failure.** |
| Set-recall collapses too | The representation genuinely cannot represent "any of these are right". Then a set-aware loss is required and is a real contribution. |
| Adding a set-aware loss (`−log Σ_{m∈Opt} softmax(z)_m`) fixes it | Confirms the label-set diagnosis and gives a method. **The best available outcome on this rung.** |

**Report top-1 and set-recall together, always.** Reporting one alone is how a working model
gets described as broken.

---

## 4. Reporting format

Every experiment reports, in this order:

1. **Device.** Did CUDA actually run? Paste the device string.
2. **Data provenance.** Which commit, which digest, how many states/positions, and the
   FNV-1a 64 of the input file.
3. **The ceiling.** What is perfect, and what did you get as a fraction of it.
4. **Variance.** Mean ± std over N seeds, with the seeds listed. **std == 0 means INCONCLUSIVE.**
5. **The branch.** Which row of which decision tree above you landed on, quoted.
6. **Controls.** What ran that could have failed. If nothing could have failed, say that —
   it is the finding.
