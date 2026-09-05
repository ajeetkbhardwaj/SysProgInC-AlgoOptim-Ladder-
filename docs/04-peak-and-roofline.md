# 4. Peak & roofline

How to know how fast you *could* go, and what stands in the way.

## 4.1 Peak arithmetic

```
peak = cores × GHz × flops/cycle
     = 2 × 2.45 × 16 (AVX2 FMA: 2 units × 4 doubles × 2 flop)
     = 78.4 GFLOPS            <- the number in our roofline printout
```

Our best (clang, N=1024): 35 GFLOPS ≈ 45% of peak. Production BLAS cites
70–85% on bare metal. The gap decomposes below.

## 4.2 Roofline method (built into the bench)

`bench/bench.c` probes `memcpy` bandwidth (~13–16 GB/s here), then prints
per-N: `GFLOPS (% peak)`, with `roof = min(peak, AI × BW)`. Ours is
compute-bound at every N (AI = N/12 ≥ 21), so the roof is flat at 78.4 and
we sit 12–30% under it. If your `% peak` *falls* with N, you're spilling
out of L3/TLB — that's the signal to tile, pack, or hugepage.

## 4.3 Why packing beat tiling (the key paper result)

Loop tiling without packing keeps stride-N accesses: at N=2048 every A/B
load misses cache and a 4×8 kernel collapses (measured in literature:
74 → 15 GFLOPS). Copying tiles into contiguous *packed* buffers makes every
load an L1 hit (15 → 69 GFLOPS). Our optim_4 → optim_7 jump is exactly this
lesson. References: Goto & van de Geijn, *Anatomy of High-Performance
Matrix Multiplication*; Van Zee & van de Geijn, *The BLIS Framework*.

## 4.4 Known anomalies on this box

- **N=512 dip (~half of neighbors).** 512×8B = 4KB stride = page size ⇒
  rows alias the same cache sets (associativity trap). Fix: padded leading
  dimension (see Exercises, optim_13).
- **Thread scaling ~1.3x on 2 vCPUs**, not 2x. Shared L3 slice, one memory
  channel, no turbo control. Physics, not a bug.
- **±30–50% run jitter.** Shared cloud VM. Report best-of-3.

## 4.5 What remains (the last mile)

1. Padded `lda` (kills the 512 trap, ~2x there).
2. `kc`-blocking for N > L3 (working set per k-slab).
3. Larger register tiles on AVX-512/AMX hardware.
4. Bare-metal validation (turbo + full L3 + real pinning).
