/* Bench-only helpers, kept out of test_helpers.h so the test suite stays
 * lean. Provides:
 *   bh_now_ns       - monotonic clock with nanosecond resolution
 *   bh_rss_kb       - resident set size of the current process
 *   bh_pct          - percentile from a sorted uint64_t array
 *   bh_cmp_u64      - qsort comparator for uint64_t
 *
 * Pair with test_helpers.h for th_mktmp / th_rmtree / th_touch / th_unlink /
 * th_mkdir / th_thread_* etc. */

#ifndef FILECAT_BENCH_HELPERS_H
#define FILECAT_BENCH_HELPERS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <psapi.h>
#elif defined(__APPLE__)
#  include <mach/mach.h>
#  include <time.h>
#else
#  include <time.h>
#  include <unistd.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define BH_UNUSED __attribute__((unused))
#else
#  define BH_UNUSED
#endif

/* ---- monotonic nanosecond clock ----
 * QueryPerformanceCounter on Windows; CLOCK_MONOTONIC elsewhere.
 * Resolution is sub-microsecond on every modern platform. */
static uint64_t bh_now_ns(void) BH_UNUSED;
static uint64_t bh_now_ns(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    static int           freq_inited = 0;
    if (!freq_inited) { QueryPerformanceFrequency(&freq); freq_inited = 1; }
    LARGE_INTEGER ctr;
    QueryPerformanceCounter(&ctr);
    /* Split high/low to avoid losing precision in the multiply. */
    uint64_t q = (uint64_t)(ctr.QuadPart / freq.QuadPart);
    uint64_t r = (uint64_t)(ctr.QuadPart % freq.QuadPart);
    return q * 1000000000ULL + (r * 1000000000ULL) / (uint64_t)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

/* ---- resident set size, in KB ----
 * Returns 0 on failure (so callers can keep printing without aborting).
 * Includes shared pages, which is what every "RSS" tool on each OS shows. */
static size_t bh_rss_kb(void) BH_UNUSED;
static size_t bh_rss_kb(void)
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
    return (size_t)(pmc.WorkingSetSize / 1024);
#elif defined(__APPLE__)
    struct mach_task_basic_info info;
    mach_msg_type_number_t      cnt = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &cnt) != KERN_SUCCESS) return 0;
    return (size_t)(info.resident_size / 1024);
#else /* Linux */
    /* statm field 2 is RSS in pages. /proc/self/status's VmRSS would also
     * work but is slower to parse. */
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    long size_pages = 0, rss_pages = 0;
    if (fscanf(f, "%ld %ld", &size_pages, &rss_pages) != 2) {
        fclose(f); return 0;
    }
    fclose(f);
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0) return 0;
    return (size_t)((rss_pages * pg) / 1024);
#endif
}

/* ---- percentile + sort helpers ---- */

static int bh_cmp_u64(const void *a, const void *b) BH_UNUSED;
static int bh_cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Linear-interpolation percentile is overkill for our sample sizes;
 * "nearest rank" is fine. p in [0,1]. Caller passes a *sorted* array. */
static uint64_t bh_pct(const uint64_t *sorted, size_t n, double p) BH_UNUSED;
static uint64_t bh_pct(const uint64_t *sorted, size_t n, double p)
{
    if (n == 0) return 0;
    if (p <= 0) return sorted[0];
    if (p >= 1) return sorted[n - 1];
    size_t idx = (size_t)((double)(n - 1) * p);
    return sorted[idx];
}

#endif /* FILECAT_BENCH_HELPERS_H */
