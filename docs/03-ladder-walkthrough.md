# 3. Ladder walkthrough

Each rung: what changed, why it helps, where it lives, what it measured.
Run `make run` and follow along with `bench/bench.c`.

## optim_1 — naive `ijk` (`matopt.c:optim_1_naive`)

Baseline. B walked column-wise: one useful double per 64B cache line.
~1 GFLOPS. Everything is measured against this.

## optim_2 — `ikj` + `restrict` (`optim_2_reorder`)

Same math, sequential B rows + `restrict` lets gcc auto-vectorize.
~9x. The single biggest algorithmic win; always do this first.

## optim_3 — `ikj` + `fma` + 4x unroll (`optim_3_blocked`)

Explicit fused multiply-add (one rounding, one instruction) and 4-wide
j-unroll exposing instruction-level parallelism. ~10x. Fastest serial rung.

## optim_4 — blocked tile, BS=32 (`optim_4_unrolled`)

Tiles the nest so the working set sits in L1. At N≤768 it ties optim_3 —
deliberately kept as a teaching rung: naive tiling *without packing* only
adds loop overhead (see `04-peak-and-roofline.md` for why). ~8x.

## optim_5 — pthreads row-split (`optim_5_parallel` + `run_jobs`)

Rows split over all online cores. ~10x on 2 cores (memory-bound, not 2x —
see §4.3 on scaling).

## optim_6 — pinned threads (`optim_6_parallel_blocked`)

Each thread locked to its core via `mt_pin`. Same speed, lower jitter:
no migration ⇒ hot caches stay hot.

## optim_7 — packed 4×8 AVX2 micro-kernel (`optim_7_packed`)

*The* rung (Goto/BLIS-style): B copied once into contiguous 8-column
panels (`pack_b8`, tails zero-padded), then C accumulates in 8 YMM
registers across the k-loop — C traffic drops ~N×, B loads are reused ×4
rows. Runtime CPU dispatch + scalar fallback without AVX2/FMA.
**~21x, 21 GFLOPS.** Fringe rows/cols handled scalar (bench N=100 proves it).

## optim_8 — THP alloc + first-touch (`alloc_thp`, `first_touch_parallel`)

2M-aligned `mmap` + advisory `MADV_HUGEPAGE` + per-thread page faulting.
Invisible at N=384 (fits TLB reach), +10–30% at N≥1024. Verify alignment
in the bench output (`aligned=1/1/1`).

## optim_9 — kernel autotuner (`autotune`)

Times {optim_2, optim_3, optim_7} on a 256-probe (2 reps, wall-clock only —
`perf_event_open` needs privileges we don't assume) and returns the winner.
Self-tuning binary, no rebuild.

## optim_10 — out-of-core (`ooc_matmul`)

C computed in a 256MB-capped, `mkstemp`+unlinked, `MAP_SHARED` mapping,
then copied out. Slower (page faults + `msync`) but RAM-bounded. Verified
bit-identical vs in-RAM (`err=0.0`).

## optim_11 — parallel packed (`optim_11_parallel_packed`)

B packed once (shared read-only), rows split over pinned threads, each
zeroing/first-touching its own C rows. ~22–28x.

## optim_12 — peak (`optim_12_peak`)

optim_11 + thread-count autotune over {1,2,4,…,ncpu} (skipped for N<512
where tuning costs more than it saves). Best measured: 35 GFLOPS, 45% of
box peak under clang. See `04-peak-and-roofline.md` for the last mile.
