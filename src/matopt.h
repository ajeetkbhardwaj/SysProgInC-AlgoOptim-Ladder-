#pragma once
#include <stddef.h>

// C = A @ B, NxN row-major double. optim_1 slowest -> optim_6 fastest.
void optim_1_naive(const double *A, const double *B, double *C, int N);
void optim_2_reorder(const double *restrict A, const double *restrict B,
                     double *restrict C, int N);
void optim_3_blocked(const double *restrict A, const double *restrict B,
                     double *restrict C, int N); // = ikj+fma+unroll4 (fastest serial)
void optim_4_unrolled(const double *restrict A, const double *restrict B,
                      double *restrict C, int N); // = cache-blocked tile (tiling demo)
void optim_5_parallel(const double *restrict A, const double *restrict B,
                      double *restrict C, int N); // = threads, unpinned
void optim_6_parallel_blocked(const double *restrict A, const double *restrict B,
                              double *restrict C, int N); // = threads, pinned per-core

// ---- novelty rungs (all unprivileged, zero-dep) ----
int cpu_has_avx2_fma(void); // runtime dispatch, 0 => scalar fallback
void optim_7_packed(const double *A, const double *B, double *C, int N);
void optim_11_parallel_packed(const double *A, const double *B, double *C, int N);
void optim_12_peak(const double *A, const double *B, double *C, int N);

// 2M-aligned THP alloc (advisory MADV_HUGEPAGE only). Returns aligned ptr;
// base/len needed to free. NULL on failure or overflow.
void *alloc_thp(size_t bytes, void **base_out, size_t *len_out);
void free_thp(void *base, size_t len);
void first_touch_parallel(void *p, size_t bytes); // fault pages on worker cores

typedef void (*matmul_fn)(const double *, const double *, double *, int);
typedef struct { const char *name; matmul_fn fn; } cand_t;
cand_t autotune(int N); // wall-clock only, bounded probe

// Out-of-core C = A@B: C computed in a size-capped (256MB) mkstemp-backed
// shared mmap, then copied out. Returns 0 ok, -1 refused/error. File unlinked.
int ooc_matmul(const double *A, const double *B, int N, double *C);

// sys utils
void *alloc_mmap(size_t bytes);
void free_mmap(void *p, size_t bytes);
double now_sec(void);
void pin_cpu0(void);
