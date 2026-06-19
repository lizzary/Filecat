/* bench_throughput — sustained event-consumption rate.
 *
 * N producer threads spam touch+unlink against the watched dir for a fixed
 * duration; one consumer thread drains filecat_next_event in parallel. We
 * report:
 *   - producer ops/sec (fs operations actually executed)
 *   - consumer events/sec (events successfully drained from the watcher)
 *   - overflow count (kernel buffer pressure indicator)
 *
 * Producer ops upper-bounds what the watcher could observe; events/sec
 * vs ops/sec reveals coalescing or queue saturation, not bugs. */

#include "bench_helpers.h"
#include "test_helpers.h"

#include "filecat/filecat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef BENCH_DURATION_MS
#  define BENCH_DURATION_MS 10000
#endif
#ifndef BENCH_PRODUCERS
#  define BENCH_PRODUCERS  4
#endif

struct prod_arg {
    const char  *dir;
    int          idx;
    volatile int *stop;
    long long    ops;
};

static void *producer_(void *arg)
{
    struct prod_arg *a   = (struct prod_arg *)arg;
    long long        ops = 0;
    for (long long i = 0; !*a->stop; i++) {
        char p[1024];
        snprintf(p, sizeof p, "%s" TH_SEP "p%d-%lld", a->dir, a->idx, i);
        if (th_touch(p) == 0) {
            ops++;
            if (th_unlink(p) == 0) ops++;
        }
    }
    a->ops = ops;
    return NULL;
}

struct cons_arg {
    filecat_watcher_t *w;
    long long          events;
    long long          overflows;
};

static void *consumer_(void *arg)
{
    struct cons_arg  *a = (struct cons_arg *)arg;
    filecat_event_t   ev;
    for (;;) {
        filecat_status_t s = filecat_next_event(a->w, &ev);
        if (s == FILECAT_ERR_CLOSED)   return NULL;
        if (s == FILECAT_ERR_OVERFLOW) { a->overflows++; continue; }
        if (s != FILECAT_OK)           return NULL;
        a->events++;
    }
}

int main(void)
{
    char *dir = th_mktmp();
    if (!dir) { fprintf(stderr, "mktmp failed\n"); return 1; }

    filecat_watcher_t *w = NULL;
    filecat_status_t   s = filecat_open(dir, 0, &w);
    if (s != FILECAT_OK) {
        fprintf(stderr, "open: %s\n", filecat_strerror(s));
        th_rmtree(dir); free(dir);
        return 1;
    }

    struct cons_arg cons = { w, 0, 0 };
    th_thread_t     cons_t;
    if (th_thread_create(&cons_t, consumer_, &cons) != 0) {
        fprintf(stderr, "consumer spawn failed\n");
        filecat_destroy(w); th_rmtree(dir); free(dir);
        return 1;
    }
    /* Let the consumer enter filecat_next_event before producers start —
     * critical on Windows, where RDCW only catches events delivered while
     * a call is pending. */
    th_sleep_ms(50);

    volatile int    stop = 0;
    th_thread_t     prods[BENCH_PRODUCERS];
    struct prod_arg args [BENCH_PRODUCERS];
    for (int i = 0; i < BENCH_PRODUCERS; i++) {
        args[i].dir  = dir;
        args[i].idx  = i;
        args[i].stop = &stop;
        args[i].ops  = 0;
        if (th_thread_create(&prods[i], producer_, &args[i]) != 0) {
            fprintf(stderr, "producer %d spawn failed\n", i);
            stop = 1;
            for (int j = 0; j < i; j++) th_thread_join(prods[j]);
            filecat_close(w); th_thread_join(cons_t);
            filecat_destroy(w); th_rmtree(dir); free(dir);
            return 1;
        }
    }

    uint64_t t0 = bh_now_ns();
    th_sleep_ms(BENCH_DURATION_MS);
    stop = 1;
    for (int i = 0; i < BENCH_PRODUCERS; i++) th_thread_join(prods[i]);
    uint64_t t1 = bh_now_ns();

    /* Let the consumer drain residual events before we close. */
    th_sleep_ms(500);
    filecat_close(w);
    th_thread_join(cons_t);

    long long total_ops = 0;
    for (int i = 0; i < BENCH_PRODUCERS; i++) total_ops += args[i].ops;

    double sec = (double)(t1 - t0) / 1e9;
    printf("bench_throughput\n");
    printf("  producers      = %d\n",         BENCH_PRODUCERS);
    printf("  duration       = %.3f s\n",     sec);
    printf("  producer ops   = %lld (%.0f ops/sec)\n",
           total_ops, (double)total_ops / sec);
    printf("  events drained = %lld (%.0f events/sec)\n",
           cons.events, (double)cons.events / sec);
    printf("  overflows      = %lld\n",       cons.overflows);

    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}
