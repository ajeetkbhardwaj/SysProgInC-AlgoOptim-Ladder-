# 2. Systems background

The hardware units that decide your GFLOPS, and the exact syscalls/libc
calls we use to talk to them.

## 2.1 Cache hierarchy (L1 → L2 → L3 → DRAM)

Typical latencies: L1 ~1ns/32KB, L2 ~3ns/512KB, L3 ~12ns/32MB,
DRAM ~100ns/GBs. Our box (EPYC 7763 VM slice): 32K / 512K / 32M.
N=384 needs 3×1.2MB — fits L3, so optim_2 already flies. N=1536 needs
3×19MB > L3 — spills to DRAM, needs tiling/packing (optim_4/7) and THP
(optim_8). **Lesson: the winning optimization depends on N vs cache.**

## 2.2 TLB — the hidden cache

Every memory access translates virtual→physical via the TLB (~2048 entries
× 4KB = 8MB reach). N=1024 matrices (24MB) need 6144 translations — TLB
thrashing. 2MB huge pages stretch reach to 4GB. We request them with
`mmap` + `madvise(MADV_HUGEPAGE)` on 2M-aligned memory (`alloc_thp` in
`src/matopt.c`), plus parallel first-touch so pages land near their threads.
Rule of thumb: `dTLB-miss/load > 5–10%` ⇒ hugepages help (check with
`perf stat -e dTLB-loads,dTLB-load-misses` where permitted).

## 2.3 SIMD + FMA (one instruction, many doubles)

AVX2 YMM registers hold 4 doubles; `vfmadd231pd` does 4 multiply-adds =
8 flops/cycle per core per FMA unit (×2 units = 16 flops/cycle peak).
`src/matopt.c:packed_avx2_range` keeps a 4×8 C tile in 8 YMM registers
across the whole k-loop: C is written once, B is loaded once and reused
across 4 rows. Requirements: `<immintrin.h>`, `loadu/storeu` (never fault
on alignment), `_mm256_broadcast_sd` for A elements.

## 2.4 Threads + affinity + first-touch

`optim_5/6/11/12` split rows across `cpu_count()` threads
(`sysconf(_SC_NPROCESSORS_ONLN)` / `GetSystemInfo`). Two refinements:
**pinning** (`pthread_setaffinity_np`, Linux-only) stops migration so hot
L1/L2 stay hot; **first-touch** (`first_touch_parallel`) faults each page
from the thread that will use it, so NUMA placement is local. Threads are
always joined; count is capped at N.

## 2.5 Clock, the honest ruler

All timing uses `clock_gettime(CLOCK_MONOTONIC)` (`now_sec`), never
`gettimeofday` (wall clock can jump). Cloud VMs jitter ±30–50%, so report
best-of-3. Reps=3 per rung in `bench/bench.c` average out the worst spikes.

## 2.6 Syscall/libc map

| Need | Call | Where |
|---|---|---|
| anon memory | `mmap/munmap`, `VirtualAlloc` (Win) | `mem_reserve/release` |
| huge pages | `madvise(MADV_HUGEPAGE)` (Linux) | `mem_hint_huge` |
| threads | `pthread_create/join`, `_beginthreadex` (Win) | `mt_create/join` |
| pinning | `pthread_setaffinity_np` (Linux) | `mt_pin` |
| time | `clock_gettime`, `QueryPerformanceCounter` (Win) | `now_sec` |
| file-backed C | `mkstemp/ftruncate/mmap(MAP_SHARED)/msync` | `ooc_matmul` |
| CPU detect | `__builtin_cpu_supports`, `__cpuid` (MSVC) | `cpu_has_avx2_fma` |
