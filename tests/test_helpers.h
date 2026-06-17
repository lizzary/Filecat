/* Shared infrastructure for the Linux test suite.
 *
 * Header-only: each test_*.c is a self-contained executable that includes
 * this file. All helpers are marked static; unused-function warnings are
 * suppressed with __attribute__((unused)). */

#ifndef FILECAT_TEST_HELPERS_H
#define FILECAT_TEST_HELPERS_H

#define _GNU_SOURCE 1

#include "filecat/filecat.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__GNUC__) || defined(__clang__)
#  define TH_UNUSED __attribute__((unused))
#else
#  define TH_UNUSED
#endif

/* ===================== assertions ===================== */

#define TH_FAIL(...) do {                                       \
    fprintf(stderr, "\n  FAIL %s:%d: ", __FILE__, __LINE__);    \
    fprintf(stderr, __VA_ARGS__);                               \
    fprintf(stderr, "\n");                                      \
    return -1;                                                  \
} while (0)

#define TH_ASSERT(cond) do {                                    \
    if (!(cond)) TH_FAIL("assertion failed: %s", #cond);        \
} while (0)

#define TH_ASSERT_EQ(a, b) do {                                 \
    long long _a = (long long)(a), _b = (long long)(b);         \
    if (_a != _b)                                               \
        TH_FAIL("%s != %s (got %lld, want %lld)",               \
                #a, #b, _a, _b);                                \
} while (0)

#define TH_ASSERT_STATUS(expr, want) do {                       \
    filecat_status_t _got = (expr);                             \
    filecat_status_t _want = (want);                            \
    if (_got != _want)                                          \
        TH_FAIL("%s -> %s, want %s", #expr,                     \
                filecat_strerror(_got), filecat_strerror(_want));\
} while (0)

#define TH_ASSERT_OK(expr) TH_ASSERT_STATUS((expr), FILECAT_OK)

static int th_passes = 0;
static int th_fails  = 0;

#define TH_RUN(name) do {                                            \
    fprintf(stderr, "[RUN ] %-50s ", #name);                         \
    fflush(stderr);                                                  \
    struct timespec _t0, _t1;                                        \
    clock_gettime(CLOCK_MONOTONIC, &_t0);                            \
    int _r = name();                                                 \
    clock_gettime(CLOCK_MONOTONIC, &_t1);                            \
    long _ms = (_t1.tv_sec - _t0.tv_sec)*1000                        \
             + (_t1.tv_nsec - _t0.tv_nsec)/1000000;                  \
    if (_r != 0) { fprintf(stderr, "FAIL  (%ld ms)\n", _ms); th_fails++; } \
    else         { fprintf(stderr, "ok    (%ld ms)\n", _ms); th_passes++; }\
} while (0)

#define TH_SUMMARY() do {                                            \
    fprintf(stderr, "------\n%d passed, %d failed\n",                \
            th_passes, th_fails);                                    \
    return th_fails ? 1 : 0;                                         \
} while (0)

/* ===================== fs / time helpers ===================== */

static char *th_mktmp(void) TH_UNUSED;
static char *th_mktmp(void)
{
    char *p = strdup("/tmp/filecat-test-XXXXXX");
    if (!p) return NULL;
    if (!mkdtemp(p)) { free(p); return NULL; }
    return p;
}

static void th_rmtree(const char *path) TH_UNUSED;
static void th_rmtree(const char *path)
{
    DIR *d = opendir(path);
    if (!d) { rmdir(path); return; }
    struct dirent *de;
    char buf[4096];
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        snprintf(buf, sizeof(buf), "%s/%s", path, de->d_name);
        struct stat st;
        if (lstat(buf, &st) == 0 && S_ISDIR(st.st_mode)) th_rmtree(buf);
        else unlink(buf);
    }
    closedir(d);
    rmdir(path);
}

static int th_touch(const char *path) TH_UNUSED;
static int th_touch(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    return close(fd);
}

static int th_write(const char *path, const char *content) TH_UNUSED;
static int th_write(const char *path, const char *content)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    size_t n = strlen(content);
    if (write(fd, content, n) != (ssize_t)n) { close(fd); return -1; }
    return close(fd);
}

static void th_sleep_ms(int ms) TH_UNUSED;
static void th_sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static long th_elapsed_ms(const struct timespec *t0,
                          const struct timespec *t1) TH_UNUSED;
static long th_elapsed_ms(const struct timespec *t0,
                          const struct timespec *t1)
{
    return (t1->tv_sec - t0->tv_sec) * 1000
         + (t1->tv_nsec - t0->tv_nsec) / 1000000;
}

static int th_max_user_watches(void) TH_UNUSED;
static int th_max_user_watches(void)
{
    FILE *f = fopen("/proc/sys/fs/inotify/max_user_watches", "r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    return v;
}

/* ===================== event collector =====================
 *
 * Background thread that drains filecat_next_event into a growable array.
 * Pattern:
 *   th_collector_start(&col, w);
 *   // perform fs actions
 *   th_sleep_ms(N);     // let kernel + consumer catch up
 *   th_collector_stop(&col);   // calls filecat_close + joins consumer
 *   // inspect col.events / contains / count_type
 *   th_collector_free(&col);
 */

typedef struct {
    filecat_event_type_t type;
    char                *path;   /* owned */
} th_event_t;

typedef struct {
    filecat_watcher_t *w;
    th_event_t        *events;
    int                n;
    int                cap;
    int                overflows;   /* count of FILECAT_ERR_OVERFLOW returns */
    int                errors;      /* count of unexpected non-OK statuses   */
    pthread_mutex_t    mu;
    pthread_t          consumer;
    int                started;
} th_collector_t;

static void *th_collector_loop_(void *arg) TH_UNUSED;
static void *th_collector_loop_(void *arg)
{
    th_collector_t *c = (th_collector_t *)arg;
    for (;;) {
        filecat_event_t ev;
        filecat_status_t s = filecat_next_event(c->w, &ev);
        if (s == FILECAT_ERR_CLOSED) return NULL;
        if (s == FILECAT_ERR_OVERFLOW) {
            pthread_mutex_lock(&c->mu);
            c->overflows++;
            pthread_mutex_unlock(&c->mu);
            continue;
        }
        if (s != FILECAT_OK) {
            pthread_mutex_lock(&c->mu);
            c->errors++;
            pthread_mutex_unlock(&c->mu);
            return NULL;
        }
        char *path = strdup(ev.path);
        if (!path) continue;
        pthread_mutex_lock(&c->mu);
        if (c->n == c->cap) {
            int newcap = c->cap ? c->cap * 2 : 64;
            th_event_t *e = (th_event_t *)realloc(c->events,
                                                  (size_t)newcap * sizeof(*e));
            if (!e) { pthread_mutex_unlock(&c->mu); free(path); continue; }
            c->events = e;
            c->cap    = newcap;
        }
        c->events[c->n].type = ev.type;
        c->events[c->n].path = path;
        c->n++;
        pthread_mutex_unlock(&c->mu);
    }
}

static int th_collector_start(th_collector_t *c, filecat_watcher_t *w) TH_UNUSED;
static int th_collector_start(th_collector_t *c, filecat_watcher_t *w)
{
    memset(c, 0, sizeof(*c));
    c->w = w;
    if (pthread_mutex_init(&c->mu, NULL) != 0) return -1;
    if (pthread_create(&c->consumer, NULL, th_collector_loop_, c) != 0) {
        pthread_mutex_destroy(&c->mu);
        return -1;
    }
    c->started = 1;
    return 0;
}

static void th_collector_stop(th_collector_t *c) TH_UNUSED;
static void th_collector_stop(th_collector_t *c)
{
    if (!c->started) return;
    filecat_close(c->w);
    pthread_join(c->consumer, NULL);
    pthread_mutex_destroy(&c->mu);
    c->started = 0;
}

static void th_collector_free(th_collector_t *c) TH_UNUSED;
static void th_collector_free(th_collector_t *c)
{
    for (int i = 0; i < c->n; i++) free(c->events[i].path);
    free(c->events);
    memset(c, 0, sizeof(*c));
}

static int th_collector_contains(th_collector_t *c,
                                 filecat_event_type_t type,
                                 const char *path) TH_UNUSED;
static int th_collector_contains(th_collector_t *c,
                                 filecat_event_type_t type,
                                 const char *path)
{
    pthread_mutex_lock(&c->mu);
    int found = 0;
    for (int i = 0; i < c->n; i++) {
        if (c->events[i].type == type &&
            strcmp(c->events[i].path, path) == 0) { found = 1; break; }
    }
    pthread_mutex_unlock(&c->mu);
    return found;
}

static int th_collector_count(th_collector_t *c) TH_UNUSED;
static int th_collector_count(th_collector_t *c)
{
    pthread_mutex_lock(&c->mu);
    int n = c->n;
    pthread_mutex_unlock(&c->mu);
    return n;
}

static int th_collector_count_type(th_collector_t *c,
                                   filecat_event_type_t type) TH_UNUSED;
static int th_collector_count_type(th_collector_t *c,
                                   filecat_event_type_t type)
{
    pthread_mutex_lock(&c->mu);
    int n = 0;
    for (int i = 0; i < c->n; i++) if (c->events[i].type == type) n++;
    pthread_mutex_unlock(&c->mu);
    return n;
}

/* Find the first index of (type, path) in events, or -1. Caller must hold
 * c->mu while reading the result. */
static int th_collector_find_locked(th_collector_t *c,
                                    filecat_event_type_t type,
                                    const char *path) TH_UNUSED;
static int th_collector_find_locked(th_collector_t *c,
                                    filecat_event_type_t type,
                                    const char *path)
{
    for (int i = 0; i < c->n; i++) {
        if (c->events[i].type == type &&
            strcmp(c->events[i].path, path) == 0) return i;
    }
    return -1;
}

#endif /* FILECAT_TEST_HELPERS_H */
