# 1. Math background

You only need four ideas to follow the whole ladder.

## 1.1 Work vs traffic

`C = A @ B` for N×N matrices does `2N³` floating-point ops (multiply + add
per inner iteration). It touches `3N²` doubles = `24N²` bytes minimum
(read A, B; write C). The ratio is the **arithmetic intensity**:

```
AI = flops / bytes = 2N³ / 24N² = N / 12   flops per byte
```

N=384 → AI=32. N=1024 → AI≈85. Bigger N reuses each byte more times, so
big GEMMs are compute-bound and small ones are memory-bound. Every cache
optimization in this repo is just a trick to raise the *effective* AI by
re-reading from cache instead of DRAM.

## 1.2 Big-O is not the story here

Naive and optimized GEMM are all Θ(N³). The ladder never changes complexity —
it changes the **constant**: cache misses (100ns each vs 1ns hits), vector
width (1 vs 4 doubles per instruction), core count (1 vs ncpu). At N=1024,
`2N³ = 2.1 GFLOP`: 1 GFLOPS takes 2s, 20 GFLOPS takes 0.1s. Same Big-O,
20x apart. Systems optimization lives in constants.

## 1.3 Why loop order matters (the math of locality)

```
ijk: C[i][j] += A[i][k] * B[k][j]   # B strided by N (column walk)
ikj: C[i][j] += A[i][k] * B[k][j]   # B sequential (row walk), same math
```

Both compute the identical sum. But row-major B rows are contiguous: `ikj`
walks 64-byte cache lines end-to-end, `ijk` touches one double per line and
throws the other 7 away. Same flops, ~9x time difference (optim_1 → optim_2).

## 1.4 Floating-point error budget

Reordering sums changes rounding. Our bench checks max abs diff vs optim_1:
typical `~3e-13` at N=384. Rule of thumb: expect error growth ~`N·eps`
(`eps = 2.2e-16`); anything `< 1e-9` here is a pass. FMA (`fma(a,b,c)` does
one rounding instead of two) is usually *more* accurate than separate
multiply+add — speed and accuracy align for once.

## 1.5 What to read next

`02-systems-background.md` maps each hardware unit (cache, TLB, vector
unit) to the rung that exploits it.
