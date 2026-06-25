/* Cross-platform infrastructure for the Filecat test suite.
 *
 * Header-only — each test_*.c is a self-contained executable that includes
 * this file. All helpers are marked static; unused-function warnings are
 * suppressed with TH_UNUSED.
 *
 * Platform abstractions live here so the individual test files stay
 * platform-agnostic:
 *
 *   th_thread_*    threads          (pthread on POSIX, _beginthreadex on Win)
 *   th_mutex_*     mutex            (pthread_mutex / CRITICAL_SECTION)
 *   th_now / th_elapsed_ms          monotonic timestamps
 *   th_sleep_ms                     blocking sleep
 *   th_mktmp, th_rmtree             temp directory create/destroy
 *   th_mkdir, th_touch, th_write,
 *   th_unlink, th_rename            file system ops (use stdio when possible)
 *   th_collector_*                  background event drainer + assertions
 *
 * Platform-specific behavior the suite has to tolerate:
 *
 *  - Path separator. Linux/macOS use '/', Windows '\\'. Tests compare
 *    against expected paths using the TH_SEP macro.
 *
 *  - Rename pairing by type. Linux inotify and Windows ReadDirectoryChangesW
 *    emit a strict RENAMED_OLD -> RENAMED_NEW pair. macOS FSEvents reports
 *    a single rename flag on both sides without pairing, which our backend
 *    exposes as two RENAMED_OLD events. Use th_collector_contains_any_rename
 *    for cross-platform assertions and gate ordering checks on
 *    TH_RENAME_TRACKED.
 *
 *  - Correlation id. event_correlation_id is the cross-platform pairing
 *    key (cookie on Linux, NTFS FileId on Windows, inode on macOS). It
 *    is non-zero on both halves of every rename on every platform; on
 *    Win/mac it is also non-zero on non-rename events. The collector
 *    records it per event; use th_collector_correlation_id /
 *    th_collector_correlation_id_any_rename to query it in assertions.
 *
 *  - Event coalescing. FSEvents may coalesce rapid create+delete on the
 *    same path into one event with both flags set; the priority in
 *    map_flags picks REMOVED. Stress tests therefore assert totals
 *    instead of exact CREATED/REMOVED counts.
 */

#ifndef FILECAT_TEST_HELPERS_H
#define FILECAT_TEST_HELPERS_H

/* ============================================================== */
/* Platform headers                                                */
/* ============================================================== */

#ifdef _WIN32
#  ifndef _CRT_SECURE_NO_WARNINGS
#    define _CRT_SECURE_NO_WARNINGS 1
#  endif
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <direct.h>
#  include <io.h>
#  include <process.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#else
#  ifndef _GNU_SOURCE
#    define _GNU_SOURCE 1
#  endif
#  include <dirent.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <pthread.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <time.h>
#  include <unistd.h>
#endif

#include "filecat/filecat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#  define TH_UNUSED __attribute__((unused))
#else
#  define TH_UNUSED
#endif

/* ============================================================== */
/* Platform-flavored constants                                     */
/* ============================================================== */

#ifdef _WIN32
#  define TH_SEP      "\\"
#  define TH_SEP_CHAR '\\'
#else
#  define TH_SEP      "/"
#  define TH_SEP_CHAR '/'
#endif

/* Linux inotify and Windows ReadDirectoryChangesW deliver a strict
 * RENAMED_OLD then RENAMED_NEW pair. macOS FSEvents does not — our
 * backend surfaces both sides as RENAMED_OLD. Gate strict-ordering
 * assertions on this macro. */
#if defined(__APPLE__)
#  define TH_RENAME_TRACKED 0
#else
#  define TH_RENAME_TRACKED 1
#endif

/* Default settle time for the consumer to drain an event burst. FSEvents
 * has more kernel-latency than inotify/RDCW, so bump on macOS. Tests use
 * multiples of this for larger bursts. */
#if defined(__APPLE__)
#  define TH_SETTLE_MS 500
#else
#  define TH_SETTLE_MS 200
#endif

/* ============================================================== */
/* Assertions and test runner                                      */
/* ============================================================== */

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
    filecat_status_t _got  = (expr);                            \
    filecat_status_t _want = (want);                            \
    if (_got != _want)                                          \
        TH_FAIL("%s -> %s, want %s", #expr,                     \
                filecat_strerror(_got),                         \
                filecat_strerror(_want));                       \
} while (0)

#define TH_ASSERT_OK(expr) TH_ASSERT_STATUS((expr), FILECAT_OK)

/* TH_UNUSED on the counters so bench programs can include this header
 * without unused-variable warnings (they don't use TH_RUN/TH_SUMMARY). */
static int th_passes TH_UNUSED = 0;
static int th_fails  TH_UNUSED = 0;

/* ============================================================== */
/* Time                                                            */
/* ============================================================== */

#ifdef _WIN32
typedef ULONGLONG th_time_t;
static th_time_t th_now(void) TH_UNUSED;
static th_time_t th_now(void) { return GetTickCount64(); }
static long      th_elapsed_ms(th_time_t t0, th_time_t t1) TH_UNUSED;
static long      th_elapsed_ms(th_time_t t0, th_time_t t1) {
    return (long)(t1 - t0);
}
static void      th_sleep_ms(int ms) TH_UNUSED;
static void      th_sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
typedef struct timespec th_time_t;
static th_time_t th_now(void) TH_UNUSED;
static th_time_t th_now(void) {
    th_time_t ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts;
}
static long      th_elapsed_ms(th_time_t t0, th_time_t t1) TH_UNUSED;
static long      th_elapsed_ms(th_time_t t0, th_time_t t1) {
    return (long)((t1.tv_sec  - t0.tv_sec ) * 1000
                + (t1.tv_nsec - t0.tv_nsec) / 1000000);
}
static void      th_sleep_ms(int ms) TH_UNUSED;
static void      th_sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

#define TH_RUN(name) do {                                            \
    fprintf(stderr, "[RUN ] %-50s ", #name);                         \
    fflush(stderr);                                                  \
    th_time_t _t0 = th_now();                                        \
    int _r = name();                                                 \
    th_time_t _t1 = th_now();                                        \
    long _ms = th_elapsed_ms(_t0, _t1);                              \
    if (_r != 0) { fprintf(stderr, "FAIL  (%ld ms)\n", _ms); th_fails++; } \
    else         { fprintf(stderr, "ok    (%ld ms)\n", _ms); th_passes++; }\
} while (0)

#define TH_SUMMARY() do {                                            \
    fprintf(stderr, "------\n%d passed, %d failed\n",                \
            th_passes, th_fails);                                    \
    return th_fails ? 1 : 0;                                         \
} while (0)

/* ============================================================== */
/* File system                                                     */
/* ============================================================== */

static int th_mkdir(const char *path) TH_UNUSED;
static int th_mkdir(const char *path)
{
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

static int th_unlink(const char *path) TH_UNUSED;
static int th_unlink(const char *path) { return remove(path); }

static int th_rename(const char *src, const char *dst) TH_UNUSED;
static int th_rename(const char *src, const char *dst)
{
#ifdef _WIN32
    /* MoveFileEx with REPLACE_EXISTING matches POSIX rename(2) which
     * silently overwrites the destination. Plain rename(3) on Windows
     * fails if dst exists, breaking tests that rotate names. */
    return MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
    return rename(src, dst);
#endif
}

static int th_touch(const char *path) TH_UNUSED;
static int th_touch(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    return fclose(f) == 0 ? 0 : -1;
}

static int th_write(const char *path, const char *content) TH_UNUSED;
static int th_write(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = strlen(content);
    if (n > 0 && fwrite(content, 1, n, f) != n) { fclose(f); return -1; }
    return fclose(f) == 0 ? 0 : -1;
}

static int th_rmdir(const char *path) TH_UNUSED;
static int th_rmdir(const char *path)
{
#ifdef _WIN32
    return _rmdir(path);
#else
    return rmdir(path);
#endif
}

/* Toggle a "modify the attributes" event without changing file contents.
 * On Windows this sets the read-only attribute, which triggers
 * FILE_NOTIFY_CHANGE_ATTRIBUTES; on POSIX it chmods. Pair with
 * th_clear_readonly so subsequent cleanup (th_rmtree / unlink) succeeds. */
static int th_set_readonly(const char *path) TH_UNUSED;
static int th_set_readonly(const char *path)
{
#ifdef _WIN32
    return SetFileAttributesA(path, FILE_ATTRIBUTE_READONLY) ? 0 : -1;
#else
    return chmod(path, 0444);
#endif
}

static int th_clear_readonly(const char *path) TH_UNUSED;
static int th_clear_readonly(const char *path)
{
#ifdef _WIN32
    return SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL) ? 0 : -1;
#else
    return chmod(path, 0644);
#endif
}

/* UTF-8 safe touch. On Windows, fopen() interprets its path argument as
 * the current ANSI codepage, which mangles non-ASCII bytes — we convert
 * to UTF-16 and use _wfopen instead. On POSIX paths are opaque byte
 * sequences, so plain fopen on UTF-8 just works. */
static int th_touch_u8(const char *utf8_path) TH_UNUSED;
static int th_touch_u8(const char *utf8_path)
{
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                   utf8_path, -1, NULL, 0);
    if (wlen <= 0) return -1;
    wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)wlen);
    if (!w) return -1;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            utf8_path, -1, w, wlen) <= 0) {
        free(w); return -1;
    }
    FILE *f = _wfopen(w, L"wb");
    free(w);
    if (!f) return -1;
    return fclose(f) == 0 ? 0 : -1;
#else
    return th_touch(utf8_path);
#endif
}

/* Build a unique temporary directory and return its absolute path. The
 * caller free()s the result and is responsible for th_rmtree before
 * release. */
static char *th_mktmp(void) TH_UNUSED;
static char *th_mktmp(void)
{
#ifdef _WIN32
    char base[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, base);
    if (n == 0 || n >= MAX_PATH) return NULL;

    /* Process-local counter avoids same-process collisions when many
     * tmpdirs are created in quick succession (e.g. parallel TH_RUNs). */
    static volatile LONG counter = 0;
    LONG cnt = InterlockedIncrement(&counter);

    char *out = (char *)malloc(MAX_PATH);
    if (!out) return NULL;
    for (int attempt = 0; attempt < 200; attempt++) {
        int written = snprintf(out, MAX_PATH,
                               "%sfilecat-test-%lu-%ld-%d",
                               base,
                               (unsigned long)GetCurrentProcessId(),
                               (long)cnt, attempt);
        if (written <= 0 || written >= MAX_PATH) { free(out); return NULL; }
        if (_mkdir(out) == 0) return out;
        if (errno != EEXIST) { free(out); return NULL; }
    }
    free(out);
    return NULL;
#else
    char *p = strdup("/tmp/filecat-test-XXXXXX");
    if (!p) return NULL;
    if (!mkdtemp(p)) { free(p); return NULL; }
    return p;
#endif
}

/* Best-effort recursive removal — used for test cleanup, ignores
 * intermediate failures (the tmpdir is going to be reaped eventually
 * anyway). */
static void th_rmtree(const char *path) TH_UNUSED;
static void th_rmtree(const char *path)
{
#ifdef _WIN32
    char pattern[MAX_PATH];
    if (snprintf(pattern, sizeof(pattern), "%s\\*", path) >= (int)sizeof(pattern))
        return;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0 ||
                strcmp(fd.cFileName, "..") == 0) continue;
            char child[MAX_PATH];
            if (snprintf(child, sizeof(child), "%s\\%s",
                         path, fd.cFileName) >= (int)sizeof(child)) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                th_rmtree(child);
            } else {
                /* Clear read-only so DeleteFile succeeds. */
                SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL);
                DeleteFileA(child);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(path);
#else
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
#endif
}

/* Linux-only knob: events watchers > max_user_watches abort. Returns -1
 * on platforms that don't expose this. */
#if defined(__linux__)
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
#endif

/* ============================================================== */
/* Threading                                                       */
/* ============================================================== */

#ifdef _WIN32

typedef HANDLE             th_thread_t;
typedef CRITICAL_SECTION   th_mutex_t;

typedef struct {
    void *(*fn)(void *);
    void  *arg;
} th_thread_arg_;

static unsigned __stdcall th_thread_thunk_(void *p) TH_UNUSED;
static unsigned __stdcall th_thread_thunk_(void *p)
{
    th_thread_arg_ *a   = (th_thread_arg_ *)p;
    void *(*fn)(void *) = a->fn;
    void *arg           = a->arg;
    free(a);
    fn(arg);
    return 0;
}

static int  th_thread_create(th_thread_t *t, void *(*fn)(void *), void *arg) TH_UNUSED;
static int  th_thread_create(th_thread_t *t, void *(*fn)(void *), void *arg)
{
    th_thread_arg_ *a = (th_thread_arg_ *)malloc(sizeof(*a));
    if (!a) return -1;
    a->fn = fn; a->arg = arg;
    *t = (HANDLE)_beginthreadex(NULL, 0, th_thread_thunk_, a, 0, NULL);
    if (!*t) { free(a); return -1; }
    return 0;
}

static int  th_thread_join(th_thread_t t) TH_UNUSED;
static int  th_thread_join(th_thread_t t)
{
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return 0;
}

static int  th_mutex_init   (th_mutex_t *m) TH_UNUSED;
static int  th_mutex_init   (th_mutex_t *m) { InitializeCriticalSection(m); return 0; }
static void th_mutex_destroy(th_mutex_t *m) TH_UNUSED;
static void th_mutex_destroy(th_mutex_t *m) { DeleteCriticalSection(m); }
static void th_mutex_lock   (th_mutex_t *m) TH_UNUSED;
static void th_mutex_lock   (th_mutex_t *m) { EnterCriticalSection(m); }
static void th_mutex_unlock (th_mutex_t *m) TH_UNUSED;
static void th_mutex_unlock (th_mutex_t *m) { LeaveCriticalSection(m); }

#else

typedef pthread_t       th_thread_t;
typedef pthread_mutex_t th_mutex_t;

static int  th_thread_create(th_thread_t *t, void *(*fn)(void *), void *arg) TH_UNUSED;
static int  th_thread_create(th_thread_t *t, void *(*fn)(void *), void *arg)
{
    return pthread_create(t, NULL, fn, arg);
}

static int  th_thread_join(th_thread_t t) TH_UNUSED;
static int  th_thread_join(th_thread_t t) { return pthread_join(t, NULL); }

static int  th_mutex_init   (th_mutex_t *m) TH_UNUSED;
static int  th_mutex_init   (th_mutex_t *m) { return pthread_mutex_init(m, NULL); }
static void th_mutex_destroy(th_mutex_t *m) TH_UNUSED;
static void th_mutex_destroy(th_mutex_t *m) { pthread_mutex_destroy(m); }
static void th_mutex_lock   (th_mutex_t *m) TH_UNUSED;
static void th_mutex_lock   (th_mutex_t *m) { pthread_mutex_lock(m); }
static void th_mutex_unlock (th_mutex_t *m) TH_UNUSED;
static void th_mutex_unlock (th_mutex_t *m) { pthread_mutex_unlock(m); }

#endif

/* ============================================================== */
/* Event collector                                                 */
/*                                                                 */
/* Background thread that drains filecat_next_event into a growable */
/* array. Pattern:                                                 */
/*                                                                 */
/*   th_collector_start(&col, w);                                  */
/*   ... perform fs actions ...                                    */
/*   th_sleep_ms(TH_SETTLE_MS);     // let the pipeline drain      */
/*   th_collector_stop(&col);       // close + join consumer       */
/*   ... assert via th_collector_contains / count_type ...         */
/*   th_collector_free(&col);                                      */
/* ============================================================== */

typedef struct {
    filecat_event_type_t type;
    uint64_t             correlation_id;   /* ev.event_correlation_id at capture */
    char                *path;             /* owned */
} th_event_t;

typedef struct {
    filecat_watcher_t *w;
    th_event_t        *events;
    int                n;
    int                cap;
    int                overflows;   /* count of FILECAT_ERR_OVERFLOW returns */
    int                errors;      /* count of unexpected non-OK statuses   */
    th_mutex_t         mu;
    th_thread_t        consumer;
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
            th_mutex_lock(&c->mu);
            c->overflows++;
            th_mutex_unlock(&c->mu);
            continue;
        }
        if (s != FILECAT_OK) {
            th_mutex_lock(&c->mu);
            c->errors++;
            th_mutex_unlock(&c->mu);
            return NULL;
        }
        size_t len  = strlen(ev.path);
        char  *path = (char *)malloc(len + 1);
        if (!path) continue;
        memcpy(path, ev.path, len + 1);

        th_mutex_lock(&c->mu);
        if (c->n == c->cap) {
            int newcap = c->cap ? c->cap * 2 : 64;
            th_event_t *e = (th_event_t *)realloc(c->events,
                                                  (size_t)newcap * sizeof(*e));
            if (!e) { th_mutex_unlock(&c->mu); free(path); continue; }
            c->events = e;
            c->cap    = newcap;
        }
        c->events[c->n].type           = ev.type;
        c->events[c->n].correlation_id = ev.event_correlation_id;
        c->events[c->n].path           = path;
        c->n++;
        th_mutex_unlock(&c->mu);
    }
}

static int  th_collector_start(th_collector_t *c, filecat_watcher_t *w) TH_UNUSED;
static int  th_collector_start(th_collector_t *c, filecat_watcher_t *w)
{
    memset(c, 0, sizeof(*c));
    c->w = w;
    if (th_mutex_init(&c->mu) != 0) return -1;
    if (th_thread_create(&c->consumer, th_collector_loop_, c) != 0) {
        th_mutex_destroy(&c->mu);
        return -1;
    }
    c->started = 1;
    /* Brief settle so the consumer thread has time to actually enter
     * filecat_next_event before the test mutates the file system. On
     * Windows, ReadDirectoryChangesW only captures events delivered
     * while a call is pending; anything that happens between CreateFile
     * and the consumer's first RDCW can be dropped. Linux/macOS buffer
     * pre-call events, but the delay is cheap and uniform. */
    th_sleep_ms(50);
    return 0;
}

static void th_collector_stop(th_collector_t *c) TH_UNUSED;
static void th_collector_stop(th_collector_t *c)
{
    if (!c->started) return;
    filecat_close(c->w);
    th_thread_join(c->consumer);
    /* Do NOT destroy the mutex here: tests routinely call
     * th_collector_contains / count_type AFTER stop, and on Windows
     * EnterCriticalSection on a deleted CRITICAL_SECTION is real UB
     * (Linux's pthread_mutex_t happens to tolerate the same misuse). The
     * mutex's lifetime now matches the collector's data lifetime — it's
     * destroyed inside th_collector_free. */
    c->started = 0;
}

static void th_collector_free(th_collector_t *c) TH_UNUSED;
static void th_collector_free(th_collector_t *c)
{
    /* The mutex was held only by the consumer thread, which has already
     * been joined inside th_collector_stop, so destroying here is safe. */
    th_mutex_destroy(&c->mu);
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
    th_mutex_lock(&c->mu);
    int found = 0;
    for (int i = 0; i < c->n; i++) {
        if (c->events[i].type == type &&
            strcmp(c->events[i].path, path) == 0) { found = 1; break; }
    }
    th_mutex_unlock(&c->mu);
    return found;
}

/* Cross-platform helper for assertions about rename events: on macOS
 * both sides of a rename arrive as RENAMED_OLD; on Linux/Windows we get
 * a strict OLD/NEW pair. */
static int th_collector_contains_any_rename(th_collector_t *c,
                                            const char *path) TH_UNUSED;
static int th_collector_contains_any_rename(th_collector_t *c,
                                            const char *path)
{
    return th_collector_contains(c, FILECAT_EVENT_RENAMED_OLD, path)
        || th_collector_contains(c, FILECAT_EVENT_RENAMED_NEW, path);
}

static int th_collector_count(th_collector_t *c) TH_UNUSED;
static int th_collector_count(th_collector_t *c)
{
    th_mutex_lock(&c->mu);
    int n = c->n;
    th_mutex_unlock(&c->mu);
    return n;
}

static int th_collector_count_type(th_collector_t *c,
                                   filecat_event_type_t type) TH_UNUSED;
static int th_collector_count_type(th_collector_t *c,
                                   filecat_event_type_t type)
{
    th_mutex_lock(&c->mu);
    int n = 0;
    for (int i = 0; i < c->n; i++) if (c->events[i].type == type) n++;
    th_mutex_unlock(&c->mu);
    return n;
}

/* Find the first index of (type, path) in events, or -1. The caller
 * must hold c->mu while reading the result. */
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

/* Return the correlation_id of the first event matching (type, path), or
 * 0 if no such event was recorded. Note: 0 is also a legitimate
 * correlation_id value ("backend didn't provide one"), so callers that
 * need to distinguish "missing event" from "event with id 0" should
 * th_collector_contains first. */
static uint64_t th_collector_correlation_id(th_collector_t *c,
                                            filecat_event_type_t type,
                                            const char *path) TH_UNUSED;
static uint64_t th_collector_correlation_id(th_collector_t *c,
                                            filecat_event_type_t type,
                                            const char *path)
{
    th_mutex_lock(&c->mu);
    uint64_t id = 0;
    for (int i = 0; i < c->n; i++) {
        if (c->events[i].type == type &&
            strcmp(c->events[i].path, path) == 0) {
            id = c->events[i].correlation_id;
            break;
        }
    }
    th_mutex_unlock(&c->mu);
    return id;
}

/* Cross-platform helper: returns the correlation_id of whichever rename
 * event arrived for `path` (RENAMED_OLD on every backend; RENAMED_NEW only
 * on Linux/Windows). 0 if no rename event matched. */
static uint64_t th_collector_correlation_id_any_rename(
    th_collector_t *c, const char *path) TH_UNUSED;
static uint64_t th_collector_correlation_id_any_rename(
    th_collector_t *c, const char *path)
{
    uint64_t id = th_collector_correlation_id(c, FILECAT_EVENT_RENAMED_OLD, path);
    if (id == 0)
        id = th_collector_correlation_id(c, FILECAT_EVENT_RENAMED_NEW, path);
    return id;
}

#endif /* FILECAT_TEST_HELPERS_H */
