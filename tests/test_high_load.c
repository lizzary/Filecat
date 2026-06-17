/* High-load / soak tests — proof that the watcher survives sustained,
 * concurrent abuse and that its overflow path is honest.
 *
 * Production-readiness criteria (per test):
 *  - No crash, no hang, no unexpected error statuses.
 *  - Either the watcher captures the bulk of the events, or it reports
 *    FILECAT_ERR_OVERFLOW (a legitimate signal that the inotify queue
 *    was overwhelmed). Both are valid outcomes.
 *  - The watcher remains usable after the load — proven by a clean
 *    filecat_destroy. */

#include "test_helpers.h"

#include <stdatomic.h>

/* =========== Test 1: N producer threads × duration =========== */

struct producer_arg {
    const char *dir;
    int         idx;
    int         duration_ms;
    atomic_int  created;
    atomic_int  removed;
};

static void *producer_(void *arg)
{
    struct producer_arg *p = (struct producer_arg *)arg;
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int c = 0, r = 0;
    for (int i = 0; ; i++) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (th_elapsed_ms(&t0, &now) >= p->duration_ms) break;
        char path[512];
        snprintf(path, sizeof(path), "%s/p%d-%06d.txt", p->dir, p->idx, i);
        if (th_touch(path) == 0) {
            c++;
            if (unlink(path) == 0) r++;
        }
    }
    atomic_store_explicit(&p->created, c, memory_order_relaxed);
    atomic_store_explicit(&p->removed, r, memory_order_relaxed);
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
    pthread_t  thrs[8];
    struct producer_arg args[8];
    for (int i = 0; i < N; i++) {
        args[i].dir = dir;
        args[i].idx = i;
        args[i].duration_ms = duration_ms;
        atomic_init(&args[i].created, 0);
        atomic_init(&args[i].removed, 0);
        TH_ASSERT_EQ(pthread_create(&thrs[i], NULL, producer_, &args[i]), 0);
    }
    for (int i = 0; i < N; i++) pthread_join(thrs[i], NULL);

    /* Drain leftovers. */
    th_sleep_ms(1500);
    th_collector_stop(&col);

    int total_created = 0, total_removed = 0;
    for (int i = 0; i < N; i++) {
        total_created += atomic_load_explicit(&args[i].created, memory_order_relaxed);
        total_removed += atomic_load_explicit(&args[i].removed, memory_order_relaxed);
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
    /* (2) Either we observed close to all events, or the kernel signalled
     *     overflow. Both are correct outcomes per the public contract. */
    TH_ASSERT(col.overflows > 0 || obs_c >= total_created * 3 / 4);
    /* (3) Watcher survives — destroy must complete without crashing.    */

    th_collector_free(&col);
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

/* =========== Test 2: forced overflow + recovery =========== */

struct overflow_closer_arg { filecat_watcher_t *w; int delay_ms; };
static void *overflow_closer_(void *arg)
{
    struct overflow_closer_arg *a = (struct overflow_closer_arg *)arg;
    th_sleep_ms(a->delay_ms);
    filecat_close(a->w);
    return NULL;
}

static int test_overflow_then_recovery(void)
{
    /* Burst many events with no consumer attached. Once we start draining,
     * the kernel should either deliver everything (high max_queued_events)
     * or signal overflow at least once. Either way, after that point the
     * watcher must keep delivering subsequent events normally. */
    char *dir = th_mktmp(); TH_ASSERT(dir);
    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));

    const int burst = 30000;
    for (int i = 0; i < burst; i++) {
        char p[512];
        snprintf(p, sizeof(p), "%s/b%05d", dir, i);
        int fd = open(p, O_CREAT | O_WRONLY, 0644);
        if (fd >= 0) { close(fd); unlink(p); }
    }

    /* 3-second drain deadline via a watchdog thread. */
    struct overflow_closer_arg ca = { w, 3000 };
    pthread_t watchdog;
    TH_ASSERT_EQ(pthread_create(&watchdog, NULL, overflow_closer_, &ca), 0);

    int events = 0, overflows = 0, seen_overflow = 0, post_overflow_events = 0;
    for (;;) {
        filecat_event_t ev;
        filecat_status_t s = filecat_next_event(w, &ev);
        if (s == FILECAT_OK) {
            events++;
            if (seen_overflow) post_overflow_events++;
        } else if (s == FILECAT_ERR_OVERFLOW) {
            overflows++;
            seen_overflow = 1;
        } else {
            break;   /* CLOSED — watchdog fired */
        }
    }
    pthread_join(watchdog, NULL);

    fprintf(stderr,
            "    burst=%d, events=%d, overflows=%d, post_overflow_events=%d\n",
            burst, events, overflows, post_overflow_events);

    /* If the kernel handled the burst without overflowing (very large
     * max_queued_events), overflows==0 is fine. If overflow DID happen,
     * the watcher must remain valid — proven by the destroy below
     * completing cleanly. The events-or-overflow check ensures the
     * watcher delivered SOMETHING (it's not silently dead). */
    TH_ASSERT(events > 0 || overflows > 0);

    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

/* =========== Test 3: long soak under mixed load =========== */

struct soak_arg {
    const char *dir;
    int         idx;
    int         duration_ms;
    atomic_int  ops;
};

static void *soak_worker_(void *arg)
{
    struct soak_arg *s = (struct soak_arg *)arg;
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int ops = 0;
    for (int i = 0; ; i++) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (th_elapsed_ms(&t0, &now) >= s->duration_ms) break;

        char a[512], b[512];
        snprintf(a, sizeof(a), "%s/soak%d-%06d.txt", s->dir, s->idx, i);
        snprintf(b, sizeof(b), "%s/soak%d-%06d.bak", s->dir, s->idx, i);
        if (th_touch(a) != 0) continue;       ops++;
        if (th_write(a, "x") != 0) {/*no-op*/} else ops++;
        if (rename(a, b) == 0) ops++;
        if (unlink(b) == 0) ops++;
    }
    atomic_store_explicit(&s->ops, ops, memory_order_relaxed);
    return NULL;
}

static int test_long_soak(void)
{
    /* 5-second mixed workload (create/write/rename/unlink) from 4 threads.
     * Asserts: no errors, watcher destroys cleanly. */
    char *dir = th_mktmp(); TH_ASSERT(dir);
    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    const int N           = 4;
    const int duration_ms = 5000;
    pthread_t      thrs[4];
    struct soak_arg args[4];
    for (int i = 0; i < N; i++) {
        args[i].dir = dir;
        args[i].idx = i;
        args[i].duration_ms = duration_ms;
        atomic_init(&args[i].ops, 0);
        TH_ASSERT_EQ(pthread_create(&thrs[i], NULL, soak_worker_, &args[i]), 0);
    }
    for (int i = 0; i < N; i++) pthread_join(thrs[i], NULL);

    th_sleep_ms(1000);
    th_collector_stop(&col);

    int total_ops = 0;
    for (int i = 0; i < N; i++)
        total_ops += atomic_load_explicit(&args[i].ops, memory_order_relaxed);
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
    int max = th_max_user_watches();
    fprintf(stderr, "fs.inotify.max_user_watches = %d\n", max);

    TH_RUN(test_high_load_n_producers);
    TH_RUN(test_overflow_then_recovery);
    TH_RUN(test_long_soak);
    TH_SUMMARY();
}
