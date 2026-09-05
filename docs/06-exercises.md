# 6. Exercises

Build the next rungs. Each has an acceptance bar — don't merge until it passes.

## optim_13 — padded leading dimension (the proven 2x)

*Problem:* N=512 runs at half speed (§4.4): 4KB row stride aliases cache sets.
*Task:* copy A/C into stride-`S` buffers (`S = N+8` doubles), generalize the
packed kernel to `(lda, ldc)` strides, copy C back.
*Accept:* N=512 ≥ 80% of N=256 GFLOPS; fringe still OK; ASan clean.

## optim_14 — `kc`-blocking for N > L3

*Problem:* N=1536 spills 57MB out of 32MB L3.
*Task:* split k into slabs of `kc` (try 256), accumulate C across slabs.
*Accept:* N=1536 ≥ N=1024 GFLOPS on THP memory; `err < 1e-9`.

## optim_15 — single-precision `sgemm`

*Task:* duplicate the packed kernel in `float` (8 per YMM, 2x flops/cycle).
*Accept:* ≥ 1.6x the double-precision GFLOPS at N=1024, same harness.

## optim_16 — benchmark discipline

*Task:* add `--best-of=N --csv` output to the bench; record a 5-run table
for your machine into `docs/results-<host>.md`.
*Accept:* variance visible and documented; no rung regresses vs README table.

## Research prompts (applied math + AI)

1. Which rung helps *your* workload most — attention softmax (memory-bound,
   like optim_2's lesson) or matmul (compute-bound, like optim_7's)?
2. At what N does your machine flip from memory- to compute-bound?
   (Read it off the roofline curve.)
3. Reproduce the 512-trap on your hardware: sweep N in steps of 16 and plot.
