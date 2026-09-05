# 5. Portability & security

## 5.1 OS × CPU matrix

| OS | CPU | Status |
|---|---|---|
| Linux x86_64 | AVX2+FMA | full speed |
| Linux ARM | NEON | correct, compiler-vectorized fallback |
| macOS Intel | AVX2 | full (`make CC=clang`); pin/THP are no-ops |
| macOS Apple Silicon | NEON | correct fallback, `immintrin.h` excluded |
| Windows MinGW-w64 | AVX2 | full-ish (ooc is RAM-backed, pinning no-op) |
| Windows MSVC | AVX2 | builds with `cl /arch:AVX2` |

## 5.2 Shim design (`src/matopt.c` top)

One file, no `#ifdef` soup at call sites — all divergence lives in the
platform layer: `mt_create/mt_join/mt_pin` (pthreads vs `_beginthreadex`),
`mem_reserve/release` (`mmap` vs `VirtualAlloc`), `cpu_count`
(`sysconf` vs `GetSystemInfo`), `now_sec` (`clock_gettime` vs QPC),
`mem_hint_huge` (`MADV_HUGEPAGE` or no-op), SIMD behind `HAVE_X86_SIMD`
(override with `-DMATOPT_NO_SIMD` to test the ARM path anywhere).

## 5.3 Safety audit (re-run before every release)

1. `grep -rnE 'system\(|execve|popen|ptrace|setuid|mlock|MAP_HUGETLB' src/ bench/`
   → expect zero code hits (comments don't count).
2. ASan+UBSan build must exit 0 with no `ERROR`/`runtime error` lines.
3. Fringe test (N=100) `err < 1e-9`; ooc test `rc=0, err=0.0`.
4. No `/tmp/ooc*` files survive a run.

Design guarantees: advisory THP only (fails open to 4K pages, no pool, no
`mlock`); `loadu/storeu` + zero-padded pack + scalar tails (no alignment or
OOB faults); `mkstemp` 0600 + immediate `unlink` + 256MB cap + N≤2048
(no symlink races, no disk DoS, no data survival); threads ≤ N, always
joined; all sizes overflow-checked; tuners take no environment input.
