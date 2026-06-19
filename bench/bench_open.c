/* bench_open — filecat_open cold-start time as a function of subtree size.
 *
 * Counterpart to bench_rss: same N values, different metric. Linux walks
 * the tree and adds one inotify watch per directory (O(N)); macOS hands a
 * single path to FSEvents (O(1)); Windows opens one directory handle
 * (O(1)). Expect a striking divergence on the largest N.
 *
 * Each N is measured 3 times and the median is printed, which smooths
 * out OS scheduler jitter without inflating runtime. */

#include "bench_helpers.h"
#include "test_helpers.h"

#include "filecat/filecat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef BENCH_OPEN_TRIALS
#  define BENCH_OPEN_TRIALS 3
#endif

static int create_subdirs(const char *root, int n)
{
    for (int i = 0; i < n; i++) {
        char p[1024];
        if (snprintf(p, sizeof p, "%s" TH_SEP "sub%06d", root, i) >= (int)sizeof p)
            return -1;
        if (th_mkdir(p) != 0) return -1;
    }
    return 0;
}

/* Median of BENCH_OPEN_TRIALS measurements for one N. */
static double measure_ms(int n_subdirs)
{
    uint64_t samples[BENCH_OPEN_TRIALS];
    int      got = 0;

    for (int t = 0; t < BENCH_OPEN_TRIALS; t++) {
        char *dir = th_mktmp();
        if (!dir) continue;
        if (n_subdirs > 0 && create_subdirs(dir, n_subdirs) != 0) {
            th_rmtree(dir); free(dir); continue;
        }

        filecat_watcher_t *w = NULL;
        uint64_t t1 = bh_now_ns();
        filecat_status_t s = filecat_open(dir, 1, &w);
        uint64_t t2 = bh_now_ns();

        if (s == FILECAT_OK) {
            samples[got++] = t2 - t1;
            filecat_destroy(w);
        } else {
            fprintf(stderr, "  N=%d  open: %s\n", n_subdirs, filecat_strerror(s));
        }
        th_rmtree(dir); free(dir);
    }

    if (got == 0) return -1.0;
    qsort(samples, (size_t)got, sizeof(uint64_t), bh_cmp_u64);
    return (double)samples[got / 2] / 1e6;   /* ns -> ms */
}

int main(void)
{
    printf("bench_open\n");
    printf("  (median of %d trials per N; recursive)\n", BENCH_OPEN_TRIALS);

    int sizes[] = { 0, 10, 100, 1000, 10000 };
    size_t n_sizes = sizeof sizes / sizeof sizes[0];
    for (size_t i = 0; i < n_sizes; i++) {
        double ms = measure_ms(sizes[i]);
        if (ms < 0) printf("  N=%-7d  (failed)\n", sizes[i]);
        else        printf("  N=%-7d  open = %8.3f ms\n", sizes[i], ms);
    }
    return 0;
}
