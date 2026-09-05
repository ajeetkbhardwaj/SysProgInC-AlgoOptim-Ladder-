# matopt — Applied-Math Optimization Ladder in Pure C
Warnning : These bench result onto the codespace 2xcore cpu so thats why these numbers are't great but if your run onto more core /cpu you will get even better.
Note : 
1. Programming Code are written via OpenCode Agents
2. I had provided the resources and contents that i had learned in system programming in c as well programming in c
3. A basic matrix multiplication algorithm
4. Then i prompted it
5. 
6. lets again rethink and deep research what novality we can get at system level to provide more optimization so that we clim another ladder of it.
7. In the last prompt, i just asked can we run onto any device like mac/linux/window if not make it possible.

`C = A @ B` (double-precision GEMM) optimized rung by rung — from naive
triple-loop to a packed AVX2 micro-kernel with pinned threads — using only
**C11 + libc and OS syscalls**. Zero dependencies. One `make`, one binary.

It exists to answer: *how far can systems programming + the C standard
library take one algorithm, and what does each optimization actually buy?*

## The ladder

| Rung | Technique | Typical gain* |
|---|---|---|
| optim_1 | naive `ijk` baseline | 1x (~1 GFLOPS) |
| optim_2 | `ikj` reorder + `restrict` (sequential B rows) | ~9x |
| optim_3 | `ikj` + `fma` + 4x unroll (ILP/vector) | ~10x |
| optim_4 | cache tiling demo (shows why naive tiling alone fails) | ~8x |
| optim_5 | pthreads row-split | ~10x |
| optim_6 | pinned threads (`pthread_setaffinity_np`) | ~10x, less jitter |
| optim_7 | **packed 4×8 AVX2/FMA micro-kernel** (Goto/BLIS-style) | **~21x** |
| optim_8 | 2M-aligned THP alloc + parallel first-touch | +10–30% at N≥1024 |
| optim_9 | wall-clock autotuner (picks best kernel for *this* box) | — |
| optim_10 | capped out-of-core C (file-backed mmap, 256 MB cap) | trades speed for RAM |
| optim_11 | parallel packed-AVX2, shared pack buffer | ~22x |
| optim_12 | optim_11 + thread-count autotune (powers of 2 → ncpu) | **~25–30x** |

\* Measured on 2-vCPU EPYC 7763 (peak ≈ 78 GFLOPS): best 23–35 GFLOPS
(30–45% of peak). Cloud jitter is ±30–50%, so the bench prints best-of-run
numbers; take best-of-3 for comparisons.

Every rung is verified against optim_1 (`err ≤ 3e-13`, fp reassociation
only) plus a fringe test (N=100, not divisible by 4/8). ASan+UBSan clean.

## Layout

```
src/matopt.h   - API: optim_1..optim_12, THP alloc, autotune, ooc
src/matopt.c   - all kernels + platform shim (Linux/macOS/Windows)
bench/bench.c  - ladder bench, fringe test, roofline (BW probe + N curve)
Makefile       - make / make run / make clean
assets/        - C stdlib + Linux syscall references used
```

## Setup & run

### Linux (gcc)

```sh
sudo apt-get install -y build-essential   # gcc + make
make && make run
```

### macOS — Intel and Apple Silicon (clang)

```sh
xcode-select --install   # one-time: compiler toolchain
make CC=clang && make run
```

Notes: Apple Silicon takes the scalar-packed fallback automatically
(no `immintrin.h`, still correct and compiler-vectorized via NEON);
thread pinning and THP hints become safe no-ops (no such APIs on macOS).

### Windows — MinGW-w64

```sh
# in MSYS2/MinGW terminal:
pacman -S --needed mingw-w64-x86_64-gcc make
make CC=x86_64-w64-mingw32-gcc && ./build/bench.exe
```

Portability notes: memory layer uses VirtualAlloc, threads use
`_beginthreadex`; optim_10 is RAM-backed (same API/caps) instead of
file-backed; pinning is a no-op.

### Windows — MSVC (Developer Prompt)

```bat
cl /O2 /arch:AVX2 /Isrc src\matopt.c bench\bench.c /Fe:bench.exe
bench.exe
```

`/arch:AVX2` is required for the packed kernel; without AVX2/FMA the
binary dispatches to the scalar fallback automatically.

### Windows — WSL2 (recommended if MinGW fights you)

```sh
sudo apt-get install -y build-essential
make && make run
```

## Reading the output

```
N=384 GFLOP=0.113  avx2_fma=1
optim_7 packed-avx2  0.005s  21.02 GFLOPS  20.9x  err=2.6e-13
fringe N=100 packed-vs-naive err=2.8e-14 OK
roofline: memcpy-BW=13.4 GB/s peak=78.4 GFLOPS
  N=1024  20.08 GFLOPS (25.6% peak) roof=78 under-roof
```

- `err` = max abs diff vs optim_1. Anything `< 1e-9` is pass.
- `fringe ... OK` = non-multiple-of-8 tails are in-bounds and correct.
- `roofline` = measured memcpy bandwidth, theoretical peak
  (cores × GHz × 16 flops/cycle for AVX2/FMA), and % of peak per N.

## Security design (audited)

- No `setuid`, network, `exec`, `ptrace`, `mlock`, or `MAP_HUGETLB`.
  THP is advisory `MADV_HUGEPAGE` only (fails open to 4K pages).
- AVX2 uses unaligned `loadu/storeu` + zero-padded pack buffer +
  scalar fringe paths; CPU dispatch falls back without AVX2/FMA.
- Out-of-core: `mkstemp` (0600) + immediate `unlink`, 256 MB hard cap,
  N ≤ 2048, `ftruncate`-checked, unmapped before return. No temp files survive.
- Threads ≤ core count, always joined; sizes overflow-checked.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `Permission denied` running `./build/bench` | noexec workspace mount — `make run` already retries via `/tmp` |
| `avx2_fma=0`, slower numbers | pre-2013 CPU or missing `/arch:AVX2` (MSVC) / `-march=native` (gcc/clang) |
| Wild run-to-run variance | shared VM / no turbo control — report best-of-3 |
| N=512 much slower than N=256/1024 | real: 4KB-stride associativity trap (see optim_13 idea below) |

## Next rungs

- **optim_13**: padded leading dimension (stride N+8) to kill the N=512
  associativity trap — the one proven 2x still on the table.
- `kc`-blocking for N > L3, larger register tiles, bare-metal validation
  (this box caps thread scaling at ~1.3x; production BLAS cites 70–85% of peak).
