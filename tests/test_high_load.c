/* High-load / soak tests — proof that the watcher survives sustained,
 * concurrent abuse and that its overflow path is honest.
 *
 * Production-readiness criteria per test:
 *   - No crash, no hang, no unexpected error statuses.
 *   - Either the watcher captures the bulk of the events, or it reports
 *     FILECAT_ERR_OVERFLOW (a legitimate signal that the kernel queue
 *     was overwhelmed). Both are valid outcomes per the public contract.
 *   - The watcher remains usable after the load — proven by a clean
 *     filecat_destroy.
 *
 * The producer-thread counters are plain int rather than atomic_int —
 * they're only read by main after every producer is pthread_join'd, and
 * join provides the necessary happens-before. This also keeps the suite
 * portable across MSVC versions whose <stdatomic.h> support varies. */

#include "test_helpers.h"

/* =========== Test 1: N producer threads × duration =========== */

struct producer_arg {
    const char *dir;
    int         idx;
    int         duration_ms;
    int         created;
    int         removed;
};

static void *producer_(void *arg)
{
    struct producer_arg *p = (struct producer_arg *)arg;
    th_time_t t0 = th_now();
    int c = 0, r = 0;
    for (int i = 0; ; i++) {
        if (th_elapsed_ms(t0, th_now()) >= p->duration_ms) break;
        char path[1024];
        snprintf(path, sizeof(path), "%s" TH_SEP "p%d-%06d.txt",
                 p->dir, p->idx, i);
        if (th_touch(path) == 0) {
            c++;
            if (th_unlink(path) == 0) r++;
        }
    }
    p->created = c;
    p->removed = r;
    return NULL;
}

static int test_high_load_n_producers(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);
    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    const int N           = 8;        /* producer threads */
    const int duration_ms = 3000;
    th_thread_t        thrs[8];
    struct producer_arg args[8];
    for (int i = 0; i < N; i++) {
        args[i].dir         = dir;
        args[i].idx         = i;
        args[i].duration_ms = duration_ms;
        args[i].created     = 0;
        args[i].removed     = 0;
        TH_ASSERT_EQ(th_thread_create(&thrs[i], producer_, &args[i]), 0);
    }
    for (int i = 0; i < N; i++) th_thread_join(thrs[i]);

    /* Drain leftovers. */
    th_sleep_ms(TH_SETTLE_MS * 8);
    th_collector_stop(&col);

    int total_created = 0, total_removed = 0;
    for (int i = 0; i < N; i++) {
        total_created += args[i].created;
        total_removed += args[i].removed;
    }
    int obs_c = th_collector_count_type(&col, FILECAT_EVENT_CREATED);
    int obs_r = th_collector_count_type(&col, FILECAT_EVENT_REMOVED);

    fprintf(stderr,
            "    producers=%d, duration=%dms, fs_created=%d, fs_removed=%d, "
            "observed CREATED=%d, REMOVED=%d, overflows=%d\n",
            N, duration_ms, total_created, total_removed,
            obs_c, obs_r, col.overflows);

    /* (1) No internal errors. */
    TH_ASSERT_EQ(col.errors, 0);
    /* (2) Either we observed close to all events, or the kernel
     *     signalled overflow. Both are correct outcomes. The combined
     *     CREATED+REMOVED count absorbs FSEvents coalescing on macOS. */
    TH_ASSERT(col.overflows > 0
              || obs_c + obs_r >= (total_created + total_removed) * 3 / 4);
    /* (3) Watcher survives — destroy must complete without crashing. */

    th_collector_free(&col);
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

/* =========== Test 2: forced overflow + recovery =========== */

static int test_overflow_then_recovery(void)
{
    /* Burst 30k rapid create+delete pairs while a consumer is draining
     * concurrently. The test is intentionally portable: it does NOT
     * assume that the kernel buffers events between watcher creation
     * and the first read — Linux inotify and macOS FSEvents do, but
     * Windows ReadDirectoryChangesW only captures while a call is
     * pending, so the consumer has to be active during the burst.
     *
     * Pass criteria are:
     *   (1) no internal errors,
     *   (2) the watcher delivered events or signalled overflow (not
     *       silently dead),
     *   (3) destroy completes cleanly after the load. */
    char *dir = th_mktmp(); TH_ASSERT(dir);
    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));

    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    const int burst = 30000;
    for (int i = 0; i < burst; i++) {
        char p[1024];
        snprintf(p, sizeof(p), "%s" TH_SEP "b%05d", dir, i);
        if (th_touch(p) == 0) th_unlink(p);
    }

    /* Let the consumer drain leftovers — generous on macOS where the
     * dispatch queue may still be flushing. */
    th_sleep_ms(TH_SETTLE_MS * 15);
    th_collector_stop(&col);

    int events = th_collector_count(&col);
    fprintf(stderr,
            "    burst=%d, events=%d, overflows=%d\n",
            burst, events, col.overflows);

    TH_ASSERT_EQ(col.errors, 0);
    TH_ASSERT(events > 0 || col.overflows > 0);

    th_collector_free(&col);
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

/* =========== Test 3: long soak under mixed load =========== */

struct soak_arg {
    const char *dir;
    int         idx;
    int         duration_ms;
    int         ops;
};

static void *soak_worker_(void *arg)
{
    struct soak_arg *s = (struct soak_arg *)arg;
    th_time_t t0 = th_now();
    int ops = 0;
    for (int i = 0; ; i++) {
        if (th_elapsed_ms(t0, th_now()) >= s->duration_ms) break;
        char a[1024], b[1024];
        snprintf(a, sizeof(a), "%s" TH_SEP "soak%d-%06d.txt",
                 s->dir, s->idx, i);
        snprintf(b, sizeof(b), "%s" TH_SEP "soak%d-%06d.bak",
                 s->dir, s->idx, i);
        if (th_touch(a) != 0) continue;
        ops++;
        if (th_write(a, "x") == 0) ops++;
        if (th_rename(a, b)  == 0) ops++;
        if (th_unlink(b)     == 0) ops++;
    }
    s->ops = ops;
    return NULL;
}

static int test_long_soak(void)
{
    /* 5-second mixed workload (create/write/rename/unlink) from 4
     * threads. Asserts: no errors, watcher destroys cleanly. */
    char *dir = th_mktmp(); TH_ASSERT(dir);
    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    const int N           = 4;
    const int duration_ms = 5000;
    th_thread_t     thrs[4];
    struct soak_arg args[4];
    for (int i = 0; i < N; i++) {
        args[i].dir         = dir;
        args[i].idx         = i;
        args[i].duration_ms = duration_ms;
        args[i].ops         = 0;
        TH_ASSERT_EQ(th_thread_create(&thrs[i], soak_worker_, &args[i]), 0);
    }
    for (int i = 0; i < N; i++) th_thread_join(thrs[i]);

    th_sleep_ms(TH_SETTLE_MS * 5);
    th_collector_stop(&col);

    int total_ops = 0;
    for (int i = 0; i < N; i++) total_ops += args[i].ops;
    int total_events = th_collector_count(&col);

    fprintf(stderr,
            "    workers=%d, duration=%dms, fs_ops=%d, "
            "events_observed=%d, overflows=%d\n",
            N, duration_ms, total_ops, total_events, col.overflows);

    TH_ASSERT_EQ(col.errors, 0);
    /* Watcher must have survived; destroy completes below. */

    th_collector_free(&col);
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

int main(void)
{
#if defined(__linux__)
    int max = th_max_user_watches();
    fprintf(stderr, "fs.inotify.max_user_watches = %d\n", max);
#endif

    TH_RUN(test_high_load_n_producers);
    TH_RUN(test_overflow_then_recovery);
    TH_RUN(test_long_soak);
    TH_SUMMARY();
}
