// ============ platform layer: Linux / macOS / Windows ============
#if defined(_WIN32) || defined(_WIN64)
#define MATOPT_WIN 1
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>  // _beginthreadex
#include <malloc.h>   // _aligned_malloc / _aligned_free
#else
#define _GNU_SOURCE
#include <unistd.h>
#include <pthread.h>
#if defined(__linux__)
#include <sched.h>
#include <sys/mman.h>
#elif defined(__APPLE__)
#include <sys/mman.h> // mmap/munmap/madvise (no MADV_HUGEPAGE, no affinity)
#endif
#endif
#include "matopt.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// ---- threads: pthreads on POSIX, _beginthreadex on Windows ----
#ifdef MATOPT_WIN
typedef HANDLE mt_thread_t;
struct mt_start { void *(*fn)(void *); void *arg; };
static unsigned __stdcall mt_tramp(void *v) {
    struct mt_start *s = (struct mt_start *)v;
    void *(*fn)(void *) = s->fn; void *a = s->arg; free(s);
    return (unsigned)(uintptr_t)fn(a); // workers always return NULL
}
static int mt_create(mt_thread_t *t, void *(*fn)(void *), void *arg) {
    struct mt_start *s = (struct mt_start *)malloc(sizeof *s);
    if (!s) return -1;
    s->fn = fn; s->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, mt_tramp, s, 0, NULL);
    if (!h) { free(s); return -1; }
    *t = (HANDLE)h; return 0;
}
static void mt_join(mt_thread_t t) {
    WaitForSingleObject(t, INFINITE); CloseHandle(t);
}
static void mt_pin(int core, int ncpu) { (void)core; (void)ncpu; /* no thread-pinning API on Win/mac */ }
static int cpu_count(void) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 2;
}
#else
typedef pthread_t mt_thread_t;
static int mt_create(mt_thread_t *t, void *(*fn)(void *), void *arg) {
    return pthread_create(t, NULL, fn, arg);
}
static void mt_join(mt_thread_t t) { pthread_join(t, NULL); }
static void mt_pin(int core, int ncpu) {
#if defined(__linux__)
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(core % ncpu, &s);
    pthread_setaffinity_np(pthread_self(), sizeof(s), &s);
#else
    (void)core; (void)ncpu; // macOS/BSD: no thread-pinning API
#endif
}
static int cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 2 : (int)n;
}
#endif

// ---- memory: mmap on POSIX, VirtualAlloc on Windows ----
static void *mem_reserve(size_t len) {
#ifdef MATOPT_WIN
    return VirtualAlloc(NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}
static void mem_release(void *p, size_t len) {
#ifdef MATOPT_WIN
    (void)len; VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, len);
#endif
}
static void mem_hint_huge(void *p, size_t len) {
#if defined(MADV_HUGEPAGE)
    madvise(p, len, MADV_HUGEPAGE); // Linux advisory only; macOS/Win: no-op
#else
    (void)p; (void)len;
#endif
}

// ---- SIMD detection: x86 only; ARM/others => scalar fallback ----
#if !defined(MATOPT_NO_SIMD) && \
    (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86))
#define HAVE_X86_SIMD 1
#include <immintrin.h>
#endif
#if defined(__GNUC__) || defined(__clang__)
#define MATOPT_AVX2TGT __attribute__((target("avx2,fma")))
#else
#define MATOPT_AVX2TGT // MSVC: needs /arch:AVX2 on the command line
#endif
#ifdef _MSC_VER
#include <intrin.h> // __cpuid
#endif

// optim_1: ijk naive. 2*N^3 flops, worst cache: B column jumps N*8 bytes.
void optim_1_naive(const double *A, const double *B, double *C, int N) {
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            double s = 0.0;
            for (int k = 0; k < N; k++)
                s += A[i * N + k] * B[k * N + j];
            C[i * N + j] = s;
        }
}

// optim_2: ikj reorder + restrict. B read row-sequential, C reused in cache.
// Same flops, ~2-3x faster. Auto-vectorizable by gcc.
void optim_2_reorder(const double *restrict A, const double *restrict B,
                     double *restrict C, int N) {
    for (int i = 0; i < N * N; i++) C[i] = 0.0;
    for (int i = 0; i < N; i++)
        for (int k = 0; k < N; k++) {
            double a = A[i * N + k];
            for (int j = 0; j < N; j++)
                C[i * N + j] += a * B[k * N + j];
        }
}

// optim_3: ikj + fma + 4x unroll (ILP). Fastest single-thread: sequential
// B/C rows let gcc emit packed AVX FMA.
void optim_3_blocked(const double *restrict A, const double *restrict B,
                     double *restrict C, int N) {
    for (int i = 0; i < N * N; i++) C[i] = 0.0;
    for (int i = 0; i < N; i++)
        for (int k = 0; k < N; k++) {
            double a = A[i * N + k];
            const double *b = &B[k * N];
            double *c = &C[i * N];
            int j = 0;
            for (; j + 3 < N; j += 4) {
                c[j]     = fma(a, b[j], c[j]);
                c[j + 1] = fma(a, b[j + 1], c[j + 1]);
                c[j + 2] = fma(a, b[j + 2], c[j + 2]);
                c[j + 3] = fma(a, b[j + 3], c[j + 3]);
            }
            for (; j < N; j++) c[j] = fma(a, b[j], c[j]);
        }
}

// optim_4: optim_3 + 4x j-unroll. Exposes ILP, fewer loop branches,
// lets gcc emit packed AVX FMA. Aligned access assumed.
void optim_4_unrolled(const double *restrict A, const double *restrict B,
                      double *restrict C, int N) {
    const int BS = 32;
    for (int i = 0; i < N * N; i++) C[i] = 0.0;
    for (int ii = 0; ii < N; ii += BS)
        for (int kk = 0; kk < N; kk += BS)
            for (int jj = 0; jj < N; jj += BS)
                for (int i = ii; i < ii + BS && i < N; i++)
                    for (int k = kk; k < kk + BS && k < N; k++) {
                        double a = A[i * N + k];
                        const double *b = &B[k * N + jj];
                        double *c = &C[i * N + jj];
                        int jend = jj + BS < N ? jj + BS : N;
                        int j = jj;
                        for (; j + 3 < jend; j += 4) {
                            c[j - jj]     = fma(a, b[j - jj], c[j - jj]);
                            c[j - jj + 1] = fma(a, b[j - jj + 1], c[j - jj + 1]);
                            c[j - jj + 2] = fma(a, b[j - jj + 2], c[j - jj + 2]);
                            c[j - jj + 3] = fma(a, b[j - jj + 3], c[j - jj + 3]);
                        }
                        for (; j < jend; j++)
                            c[j - jj] = fma(a, b[j - jj], c[j - jj]);
                    }
}

// --- parallel workers share optim_2 / optim_3 cores, row-split ---
typedef struct { const double *A, *B; double *C; int N, r0, r1; int blocked; int core, ncpu; } Job;
static void *worker(void *p) {
    Job *j = p;
    if (j->core >= 0) // optim_6: pin each thread to its own core, stop migration
        mt_pin(j->core, j->ncpu); // Linux only; no-op on macOS/Windows
    if (!j->blocked) {
        for (int i = j->r0; i < j->r1; i++)
            for (int k = 0; k < j->N; k++) {
                double a = j->A[i * j->N + k];
                for (int m = 0; m < j->N; m++)
                    j->C[i * j->N + m] = fma(a, j->B[k * j->N + m], j->C[i * j->N + m]);
            }
    } else {
        const int BS = 32;
        for (int ii = j->r0; ii < j->r1; ii += BS)
            for (int kk = 0; kk < j->N; kk += BS)
                for (int jj = 0; jj < j->N; jj += BS) {
                    int iend = ii + BS < j->r1 ? ii + BS : j->r1;
                    for (int i = ii; i < iend; i++)
                        for (int k = kk; k < kk + BS && k < j->N; k++) {
                            double a = j->A[i * j->N + k];
                            for (int m = jj; m < jj + BS && m < j->N; m++)
                                j->C[i * j->N + m] = fma(a, j->B[k * j->N + m], j->C[i * j->N + m]);
                        }
                }
    }
    return NULL;
}
static void run_jobs(const double *A, const double *B, double *C, int N, int blocked) {
    int ncpu = cpu_count();
    if (ncpu > N) ncpu = N;
    mt_thread_t *th = malloc(sizeof(*th) * (size_t)ncpu);
    Job *jobs = malloc(sizeof(*jobs) * (size_t)ncpu);
    for (int i = 0; i < N * N; i++) C[i] = 0.0;
    for (int t = 0; t < ncpu; t++)
        jobs[t] = (Job){A, B, C, N, t * N / ncpu, (t + 1) * N / ncpu, blocked, -1, ncpu};
    for (int t = 0; t < ncpu; t++) mt_create(&th[t], worker, &jobs[t]);
    for (int t = 0; t < ncpu; t++) mt_join(th[t]);
    free(th); free(jobs);
}

// optim_6: parallel + per-thread pinning + first-touch NUMA (mmap alloc in
// bench is touched by the thread that will use it — see bench). Each thread
// locked to its core: no migration, hot L1/L2 stay hot.
void optim_6_parallel_blocked(const double *restrict A, const double *restrict B,
                              double *restrict C, int N) {
    int ncpu = cpu_count();
    if (ncpu > N) ncpu = N;
    mt_thread_t *th = malloc(sizeof(*th) * (size_t)ncpu);
    Job *jobs = malloc(sizeof(*jobs) * (size_t)ncpu);
    for (int i = 0; i < N * N; i++) C[i] = 0.0;
    for (int t = 0; t < ncpu; t++)
        jobs[t] = (Job){A, B, C, N, t * N / ncpu, (t + 1) * N / ncpu, 0, t, ncpu};
    for (int t = 0; t < ncpu; t++) mt_create(&th[t], worker, &jobs[t]);
    for (int t = 0; t < ncpu; t++) mt_join(th[t]);
    free(th); free(jobs);
}

// optim_5: multithreaded ikj. Scales ~ncpu for large N.
void optim_5_parallel(const double *restrict A, const double *restrict B,
                      double *restrict C, int N) {
    run_jobs(A, B, C, N, 0);
}

// ================= novelty rungs =================
// (stdint.h/stdio.h already included at top; fcntl unused => dropped)

int cpu_has_avx2_fma(void) {
#if defined(HAVE_X86_SIMD)
#if defined(_MSC_VER)
    int i[4]; __cpuid(i, 1);
    int avx = (i[2] >> 28) & 1, fma = (i[2] >> 12) & 1;
    __cpuidex(i, 7, 0); int avx2 = (i[1] >> 5) & 1;
    return avx && fma && avx2;
#elif defined(__GNUC__) || defined(__clang__)
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#else
    return 0;
#endif
#else
    return 0; // ARM/Apple Silicon/RISC-V: scalar fallback
#endif
}

// Pack B into contiguous 8-column panels: Bp[p*N*8 + k*8 + j].
// Tail columns (>N) zero-padded so the vector kernel never reads OOB.
static double *pack_b8(const double *B, int N) {
    if (N <= 0 || N > 4096) return NULL;
    size_t P = (size_t)(N + 7) / 8;
    size_t len = P * (size_t)N * 8;
    if (len / 8 != P * (size_t)N) return NULL; // overflow
    double *Bp = malloc(len * sizeof(double));
    if (!Bp) return NULL;
    for (int p = 0; p < (int)P; p++)
        for (int k = 0; k < N; k++)
            for (int j = 0; j < 8; j++) {
                int c = p * 8 + j;
                Bp[(size_t)p * N * 8 + (size_t)k * 8 + j] =
                    (c < N) ? B[(size_t)k * N + c] : 0.0;
            }
    return Bp;
}

// 4x8 AVX2/FMA micro-kernel over packed B. C accumulators live in YMM
// across the whole k-loop: C traffic drops ~N× vs ikj. Scalar tails for
// fringe rows/cols => always in-bounds. loadu/storeu => no alignment faults.
#ifdef HAVE_X86_SIMD
MATOPT_AVX2TGT
static void packed_avx2_range(const double *A, const double *Bp, double *C,
                              int N, int r0, int r1) {
    int P = (N + 7) / 8;
    for (int i = r0; i < r1; i += 4) {
        int R = r1 - i >= 4 ? 4 : r1 - i;
        if (R < 4) { // fringe rows: scalar over packed panels
            for (int r = 0; r < R; r++) {
                const double *ar = A + (size_t)(i + r) * N;
                double *cr = C + (size_t)(i + r) * N;
                for (int p = 0; p < P; p++) {
                    int V = N - p * 8 >= 8 ? 8 : N - p * 8;
                    const double *b = Bp + (size_t)p * N * 8;
                    for (int j = 0; j < V; j++) {
                        double s = 0.0;
                        for (int k = 0; k < N; k++)
                            s = fma(ar[k], b[(size_t)k * 8 + j], s);
                        cr[p * 8 + j] = s;
                    }
                }
            }
            continue;
        }
        const double *a0 = A + (size_t)i * N, *a1 = a0 + N,
                     *a2 = a1 + N, *a3 = a2 + N;
        double *c0 = C + (size_t)i * N, *c1 = c0 + N,
               *c2 = c1 + N, *c3 = c2 + N;
        for (int p = 0; p < P; p++) {
            int V = N - p * 8 >= 8 ? 8 : N - p * 8;
            __m256d x00 = _mm256_setzero_pd(), x01 = _mm256_setzero_pd();
            __m256d x10 = _mm256_setzero_pd(), x11 = _mm256_setzero_pd();
            __m256d x20 = _mm256_setzero_pd(), x21 = _mm256_setzero_pd();
            __m256d x30 = _mm256_setzero_pd(), x31 = _mm256_setzero_pd();
            const double *b = Bp + (size_t)p * N * 8;
            for (int k = 0; k < N; k++) {
                __m256d b0 = _mm256_loadu_pd(b + (size_t)k * 8);
                __m256d b1 = _mm256_loadu_pd(b + (size_t)k * 8 + 4);
                __m256d t0 = _mm256_broadcast_sd(a0 + k);
                __m256d t1 = _mm256_broadcast_sd(a1 + k);
                __m256d t2 = _mm256_broadcast_sd(a2 + k);
                __m256d t3 = _mm256_broadcast_sd(a3 + k);
                x00 = _mm256_fmadd_pd(t0, b0, x00);
                x01 = _mm256_fmadd_pd(t0, b1, x01);
                x10 = _mm256_fmadd_pd(t1, b0, x10);
                x11 = _mm256_fmadd_pd(t1, b1, x11);
                x20 = _mm256_fmadd_pd(t2, b0, x20);
                x21 = _mm256_fmadd_pd(t2, b1, x21);
                x30 = _mm256_fmadd_pd(t3, b0, x30);
                x31 = _mm256_fmadd_pd(t3, b1, x31);
            }
            if (V == 8) {
                _mm256_storeu_pd(c0 + p * 8, x00);
                _mm256_storeu_pd(c0 + p * 8 + 4, x01);
                _mm256_storeu_pd(c1 + p * 8, x10);
                _mm256_storeu_pd(c1 + p * 8 + 4, x11);
                _mm256_storeu_pd(c2 + p * 8, x20);
                _mm256_storeu_pd(c2 + p * 8 + 4, x21);
                _mm256_storeu_pd(c3 + p * 8, x30);
                _mm256_storeu_pd(c3 + p * 8 + 4, x31);
            } else { // fringe cols: spill via stack tmp, copy valid lanes
                double t[8];
                _mm256_storeu_pd(t, x00); _mm256_storeu_pd(t + 4, x01);
                for (int j = 0; j < V; j++) c0[p * 8 + j] = t[j];
                _mm256_storeu_pd(t, x10); _mm256_storeu_pd(t + 4, x11);
                for (int j = 0; j < V; j++) c1[p * 8 + j] = t[j];
                _mm256_storeu_pd(t, x20); _mm256_storeu_pd(t + 4, x21);
                for (int j = 0; j < V; j++) c2[p * 8 + j] = t[j];
                _mm256_storeu_pd(t, x30); _mm256_storeu_pd(t + 4, x31);
                for (int j = 0; j < V; j++) c3[p * 8 + j] = t[j];
            }
        }
    }
}

#else // no x86 SIMD (ARM / Apple Silicon / RISC-V): scalar over packed panels
static void packed_avx2_range(const double *A, const double *Bp, double *C,
                              int N, int r0, int r1) {
    int P = (N + 7) / 8;
    for (int i = r0; i < r1; i++) {
        const double *ar = A + (size_t)i * N;
        double *cr = C + (size_t)i * N;
        for (int p = 0; p < P; p++) {
            int V = N - p * 8 >= 8 ? 8 : N - p * 8;
            const double *b = Bp + (size_t)p * N * 8;
            for (int j = 0; j < V; j++) {
                double s = 0.0;
                for (int k = 0; k < N; k++) s = fma(ar[k], b[(size_t)k * 8 + j], s);
                cr[p * 8 + j] = s;
            }
        }
    }
}
#endif

void optim_7_packed(const double *A, const double *B, double *C, int N) {
    if (!cpu_has_avx2_fma()) { optim_3_blocked(A, B, C, N); return; } // safe fallback
    double *Bp = pack_b8(B, N);
    if (!Bp) { optim_3_blocked(A, B, C, N); return; } // fail-closed
    for (int i = 0; i < N * N; i++) C[i] = 0.0; // overwritten below; keeps tails defined
    packed_avx2_range(A, Bp, C, N, 0, N);
    free(Bp);
}

// ---- peak rungs: thread-scaled packed kernel ----
typedef struct {
    const double *A, *Bp; double *C; int N, r0, r1, core, ncpu;
} PJob;

static void *pworker(void *v) {
    PJob *j = v;
    if (j->core >= 0) mt_pin(j->core, j->ncpu);
    for (int i = j->r0; i < j->r1; i++) // first-touch + zero own rows
        for (int m = 0; m < j->N; m++) j->C[(size_t)i * j->N + m] = 0.0;
    packed_avx2_range(j->A, j->Bp, j->C, j->N, j->r0, j->r1);
    return NULL;
}

// nthreads<=0 => all online cores. Static row split, pinned, shared Bp.
static void parallel_packed_n(const double *A, const double *Bp, double *C,
                              int N, int nthreads) {
    if (nthreads < 1) nthreads = cpu_count();
    if (nthreads > N) nthreads = N;
    mt_thread_t *th = malloc(sizeof(*th) * (size_t)nthreads);
    PJob *js = malloc(sizeof(*js) * (size_t)nthreads);
    if (!th || !js) { free(th); free(js); return; }
    for (int t = 0; t < nthreads; t++)
        js[t] = (PJob){A, Bp, C, N, t * N / nthreads, (t + 1) * N / nthreads,
                       t, nthreads};
    for (int t = 0; t < nthreads; t++) mt_create(&th[t], pworker, &js[t]);
    for (int t = 0; t < nthreads; t++) mt_join(th[t]);
    free(th); free(js);
}

// optim_11: parallel packed-AVX2. B packed once (shared, read-only),
// rows split across pinned threads. Falls back safely without AVX2/FMA.
void optim_11_parallel_packed(const double *A, const double *B, double *C, int N) {
    if (!cpu_has_avx2_fma()) { optim_5_parallel(A, B, C, N); return; }
    double *Bp = pack_b8(B, N);
    if (!Bp) { optim_5_parallel(A, B, C, N); return; }
    parallel_packed_n(A, Bp, C, N, 0);
    free(Bp);
}

// optim_12: optim_11 + thread-count autotune (1..ncpu, bounded 2 probes each
// on a 256-row sample). Topology-aware: picks what THIS box likes.
static int best_nthreads(const double *A, const double *Bp, double *C, int N) {
    int max = cpu_count();
    if (max > N) max = N;
    int Ns = N < 256 ? N : 256;
    // Probe powers of two up to ncpu: covers 128-core boxes in ~8 probes.
    int best = 1; double bt = 1e30;
    for (int t = 1; t <= max; t *= 2) {
        double t0 = now_sec();
        parallel_packed_n(A, Bp, C, Ns, t);
        parallel_packed_n(A, Bp, C, Ns, t);
        double dt = (now_sec() - t0) / 2;
        if (dt < bt) { bt = dt; best = t; }
    }
    // ...plus the full count itself (often better than the last power of 2)
    if (max > 1 && (max & (max - 1)) != 0) {
        double t0 = now_sec();
        parallel_packed_n(A, Bp, C, Ns, max);
        parallel_packed_n(A, Bp, C, Ns, max);
        double dt = (now_sec() - t0) / 2;
        if (dt < bt) { bt = dt; best = max; }
    }
    return best;
}

void optim_12_peak(const double *A, const double *B, double *C, int N) {
    if (!cpu_has_avx2_fma()) { optim_5_parallel(A, B, C, N); return; }
    double *Bp = pack_b8(B, N);
    if (!Bp) { optim_5_parallel(A, B, C, N); return; }
    // Tuning costs ~10ms: only worth it when compute dominates (N>=512).
    int nt = 0;
    if (N >= 512) nt = best_nthreads(A, Bp, C, N);
    parallel_packed_n(A, Bp, C, N, nt);
    free(Bp);
}

// --- sys utils: portable anon memory, monotonic clock, affinity ---
void *alloc_mmap(size_t bytes) {
    if (bytes == 0) return NULL;
    void *p = mem_reserve(bytes);
    if (!p) return NULL;
    mem_hint_huge(p, bytes); // Linux: THP advisory; elsewhere no-op
    return p;
}
void free_mmap(void *p, size_t bytes) { if (p) mem_release(p, bytes); }
double now_sec(void) {
#ifdef MATOPT_WIN
    static LARGE_INTEGER f = {0};
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // Linux + macOS 10.12+
    return ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}
void pin_cpu0(void) {
#if defined(__linux__)
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(0, &s);
    sched_setaffinity(0, sizeof(s), &s);
#else
    // no process-pinning API on macOS/Windows here; threads simply unpinned
#endif
}

// ================= optim_8: guaranteed-THP alloc =================
// 2M-align (promotion requires alignment) + MADV_HUGEPAGE (advisory only:
// no pool reservation, no mlock, fails open to 4K pages). Overflow-checked.
// Non-Linux: same API, alignment kept, huge-page hint skipped.
void *alloc_thp(size_t bytes, void **base_out, size_t *len_out) {
    const size_t HP = (size_t)2 << 20;
    if (bytes == 0 || bytes > (size_t)1024 << 30) return NULL;
    size_t len = bytes + HP;
    if (len < bytes) return NULL;
    void *base = mem_reserve(len);
    if (!base) return NULL;
    mem_hint_huge(base, len); // advisory; kernel may ignore
    uintptr_t a = ((uintptr_t)base + HP - 1) & ~(HP - 1);
    if (base_out) *base_out = base;
    if (len_out) *len_out = len;
    return (void *)a;
}
void free_thp(void *base, size_t len) {
    if (base && len) mem_release(base, len);
}

typedef struct { char *p; size_t n; } Touch;
static void *toucher(void *v) {
    Touch *t = v;
    for (size_t i = 0; i < t->n; i += 4096) t->p[i] = 0; // fault page locally
    return NULL;
}
// Each thread faults its own chunk => pages land near their user (first-touch).
void first_touch_parallel(void *p, size_t bytes) {
    int ncpu = cpu_count();
    mt_thread_t *th = malloc(sizeof(*th) * (size_t)ncpu);
    Touch *ts = malloc(sizeof(*ts) * (size_t)ncpu);
    if (!th || !ts) { free(th); free(ts); return; }
    for (int t = 0; t < ncpu; t++) {
        size_t c0 = bytes * (size_t)t / (size_t)ncpu;
        size_t c1 = bytes * (size_t)(t + 1) / (size_t)ncpu;
        ts[t] = (Touch){(char *)p + c0, c1 - c0};
        mt_create(&th[t], toucher, &ts[t]);
    }
    for (int t = 0; t < ncpu; t++) mt_join(th[t]);
    free(th); free(ts);
}

// ================= optim_9: wall-clock autotuner =================
// perf counters need paranoid<=2 (ours is 4), so tune on time only.
// Bounded: fixed small probe, reps=2, candidates fixed. No env/exec input.
cand_t autotune(int N) {
    cand_t cands[] = {
        {"optim_2 ikj", optim_2_reorder},
        {"optim_3 ikj+fma+unroll", optim_3_blocked},
        {"optim_7 packed-avx2", optim_7_packed},
    };
    int nc = 3;
    int Np = N < 256 ? N : 256;
    size_t b = (size_t)Np * Np * sizeof(double);
    double *A = malloc(b), *B = malloc(b), *C = malloc(b);
    if (!A || !B || !C) {
        free(A); free(B); free(C);
        return (cand_t){"optim_3 fallback", optim_3_blocked};
    }
    for (size_t i = 0; i < (size_t)Np * Np; i++) {
        A[i] = (double)(i % 97) / 97.0; B[i] = (double)(i % 53) / 53.0;
    }
    cand_t best = cands[0];
    double bt = 1e30;
    for (int c = 0; c < nc; c++) {
        double t0 = now_sec();
        cands[c].fn(A, B, C, Np);
        cands[c].fn(A, B, C, Np);
        double t = (now_sec() - t0) / 2;
        printf("  tune: %-22s %.4fs\n", cands[c].name, t);
        if (t < bt) { bt = t; best = cands[c]; }
    }
    free(A); free(B); free(C);
    return best;
}

// ================= optim_10: capped out-of-core =================
// POSIX: C computed inside a size-capped mkstemp-backed MAP_SHARED mapping.
// Safety: mkstemp (0600, no symlink race), hard 256MB cap, ftruncate-checked,
// msync, immediate unlink. No data survives the call.
// Windows: same API/caps, RAM-backed (no tmpfs-equivalent); file path N/A.
int ooc_matmul(const double *A, const double *B, int N, double *C) {
    if (N <= 0 || N > 2048) return -1;
    size_t bytes = (size_t)N * N * sizeof(double);
    if (bytes / sizeof(double) != (size_t)N * N) return -1; // overflow
    if (bytes > (size_t)256 << 20) return -1;               // 256MB cap
#ifdef MATOPT_WIN
    double *M = (double *)malloc(bytes); // RAM fallback on Windows
    if (!M) return -1;
    optim_7_packed(A, B, M, N);
    for (size_t i = 0; i < (size_t)N * N; i++) C[i] = M[i];
    free(M);
    return 0;
#else
    char path[] = "/tmp/oocCXXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    unlink(path); // anonymous-ish: dir entry gone, fd keeps inode
    if (ftruncate(fd, (off_t)bytes) != 0) { close(fd); return -1; }
    double *M = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (M == MAP_FAILED) { close(fd); return -1; }
    close(fd); // mapping holds the (unlinked) inode
#if defined(MADV_SEQUENTIAL)
    madvise(M, bytes, MADV_SEQUENTIAL);
#endif
    optim_7_packed(A, B, M, N);
    msync(M, bytes, MS_SYNC);
    for (size_t i = 0; i < (size_t)N * N; i++) C[i] = M[i];
    munmap(M, bytes);
    return 0;
#endif
}
