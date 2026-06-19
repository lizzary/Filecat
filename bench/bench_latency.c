/* bench_latency — end-to-end touch→event latency, distribution.
 *
 * Architecture: a dedicated consumer thread drains filecat_next_event
 * and stamps each event's observation time into a shared array. The
 * main thread performs touches one at a time, then polls the array
 * for a matching path with a per-sample timeout.
 *
 * Why split it: a single-threaded drain risks a hang — Windows RDCW
 * is synchronous and will park forever waiting for a NEW event whenever
 * the touch we just issued didn't make it into the same batch (Defender
 * scan interleaving, kernel scheduling, residual unread events from the
 * previous iteration that don't match our name). The dedicated consumer
 * turns "no event in N ms" into a recoverable per-sample timeout instead
 * of an entire-bench freeze.
 *
 * Cost model: t_observe is captured in the consumer thread right after
 * filecat_next_event returns. The main→consumer scheduling delay shows
 * up as a constant additive noise floor on every sample, dominated by
 * the kernel's RDCW→user wake path which we are explicitly measuring. */

#include "bench_helpers.h"
#include "test_helpers.h"

#include "filecat/filecat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <sched.h>
#endif

#ifndef BENCH_LATENCY_SAMPLES
#  define BENCH_LATENCY_SAMPLES 5000
#endif
#ifndef BENCH_LATENCY_TIMEOUT_MS
#  define BENCH_LATENCY_TIMEOUT_MS 500
#endif

/* A single touch can produce >1 raw event on some platforms (Windows
 * emits CREATED + MODIFIED for fopen("wb")). Size the obs ring with a
 * generous headroom so the consumer never has to drop. */
#define BENCH_LATENCY_MAX_OBS (BENCH_LATENCY_SAMPLES * 4)

struct observation {
    char     path[64];
    uint64_t t_observe;
};

static struct observation g_obs[BENCH_LATENCY_MAX_OBS];
static int                g_n_obs = 0;
static th_mutex_t         g_mu;

static void *consumer_(void *arg)
{
    filecat_watcher_t *w = (filecat_watcher_t *)arg;
    for (;;) {
        filecat_event_t  ev;
        filecat_status_t s = filecat_next_event(w, &ev);
        if (s == FILECAT_ERR_CLOSED) return NULL;
        if (s != FILECAT_OK)         continue;

        /* Timestamp BEFORE the lock so the recorded value reflects the
         * earliest moment user-space saw the event, not the time we
         * waited for the mutex. */
        uint64_t t = bh_now_ns();

        th_mutex_lock(&g_mu);
        if (g_n_obs < BENCH_LATENCY_MAX_OBS) {
            size_t n = strlen(ev.path);
            if (n >= sizeof g_obs[g_n_obs].path)
                n = sizeof g_obs[g_n_obs].path - 1;
            memcpy(g_obs[g_n_obs].path, ev.path, n);
            g_obs[g_n_obs].path[n]   = '\0';
            g_obs[g_n_obs].t_observe = t;
            g_n_obs++;
        }
        th_mutex_unlock(&g_mu);
    }
}

static void short_yield(void)
{
#ifdef _WIN32
    Sleep(0);   /* surrender the rest of the time slice */
#else
    sched_yield();
#endif
}

int main(void)
{
    char *dir = th_mktmp();
    if (!dir) { fprintf(stderr, "mktmp failed\n"); return 1; }

    filecat_watcher_t *w = NULL;
    filecat_status_t   s = filecat_open(dir, 0, &w);
    if (s != FILECAT_OK) {
        fprintf(stderr, "open: %s\n", filecat_strerror(s));
        th_rmtree(dir); free(dir); return 1;
    }

    if (th_mutex_init(&g_mu) != 0) {
        fprintf(stderr, "mutex init failed\n");
        filecat_destroy(w); th_rmtree(dir); free(dir); return 1;
    }

    th_thread_t cons_t;
    if (th_thread_create(&cons_t, consumer_, w) != 0) {
        fprintf(stderr, "consumer spawn failed\n");
        th_mutex_destroy(&g_mu);
        filecat_destroy(w); th_rmtree(dir); free(dir); return 1;
    }
    /* Brief settle so the consumer has actually entered next_event
     * before we start producing — RDCW only captures events delivered
     * while a call is pending. */
    th_sleep_ms(100);

    uint64_t *samples = (uint64_t *)malloc(sizeof(uint64_t) * BENCH_LATENCY_SAMPLES);
    if (!samples) {
        fprintf(stderr, "alloc failed\n");
        filecat_close(w); th_thread_join(cons_t);
        th_mutex_destroy(&g_mu);
        filecat_destroy(w); th_rmtree(dir); free(dir); return 1;
    }

    int valid      = 0;
    int timeouts   = 0;
    int scan_start = 0;   /* observations[<scan_start] already consumed */

    for (int i = 0; i < BENCH_LATENCY_SAMPLES; i++) {
        char name[64], full[1024];
        snprintf(name, sizeof name, "lat-%06d", i);
        snprintf(full, sizeof full, "%s" TH_SEP "%s", dir, name);

        uint64_t t1 = bh_now_ns();
        if (th_touch(full) != 0) continue;

        uint64_t deadline = t1 + (uint64_t)BENCH_LATENCY_TIMEOUT_MS * 1000000ULL;
        uint64_t t2       = 0;
        for (int spin = 0; bh_now_ns() < deadline; spin++) {
            th_mutex_lock(&g_mu);
            int local_n = g_n_obs;
            for (int j = scan_start; j < local_n; j++) {
                if (strcmp(g_obs[j].path, name) == 0) {
                    t2         = g_obs[j].t_observe;
                    scan_start = j + 1;
                    break;
                }
            }
            th_mutex_unlock(&g_mu);
            if (t2) break;
            /* Every ~64 spins, yield so the consumer gets cycles to
             * append. Bare busy-wait would starve it on a single-core box. */
            if ((spin & 0x3F) == 0) short_yield();
        }
        if (t2 > t1) samples[valid++] = t2 - t1;
        else         timeouts++;
    }

    /* Stop consumer cleanly. */
    filecat_close(w);
    th_thread_join(cons_t);

    if (valid == 0) {
        fprintf(stderr, "no valid samples — every sample timed out\n");
        free(samples); th_mutex_destroy(&g_mu);
        filecat_destroy(w); th_rmtree(dir); free(dir); return 1;
    }

    qsort(samples, (size_t)valid, sizeof(uint64_t), bh_cmp_u64);

    printf("bench_latency\n");
    printf("  samples  = %d (valid %d, timeouts %d)\n",
           BENCH_LATENCY_SAMPLES, valid, timeouts);
    printf("  min      = %.2f us\n", (double)samples[0]                          / 1000.0);
    printf("  p50      = %.2f us\n", (double)bh_pct(samples, valid, 0.50)        / 1000.0);
    printf("  p90      = %.2f us\n", (double)bh_pct(samples, valid, 0.90)        / 1000.0);
    printf("  p99      = %.2f us\n", (double)bh_pct(samples, valid, 0.99)        / 1000.0);
    printf("  p999     = %.2f us\n", (double)bh_pct(samples, valid, 0.999)       / 1000.0);
    printf("  max      = %.2f us\n", (double)samples[valid - 1]                  / 1000.0);

    free(samples);
    th_mutex_destroy(&g_mu);
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}
