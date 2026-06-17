/* Correctness tests for the Linux backend.
 *
 * Each test stands alone: open a fresh watcher in a fresh tmpdir, exercise
 * one bit of the contract, destroy, clean up. Per-event timing waits are
 * generous (200 ms typical) so the suite is reliable on loaded CI hosts. */

#include "test_helpers.h"

/* ===================== thread helpers used below ===================== */

struct th_delayed_close_arg { filecat_watcher_t *w; int delay_ms; };
static void *th_delayed_close_(void *arg)
{
    struct th_delayed_close_arg *a = (struct th_delayed_close_arg *)arg;
    th_sleep_ms(a->delay_ms);
    filecat_close(a->w);
    return NULL;
}

struct th_blocking_next_arg { filecat_watcher_t *w; filecat_status_t result; };
static void *th_blocking_next_(void *arg)
{
    struct th_blocking_next_arg *a = (struct th_blocking_next_arg *)arg;
    filecat_event_t ev;
    a->result = filecat_next_event(a->w, &ev);
    return NULL;
}

/* =============================================================== */

static int test_strerror_all_codes(void)
{
    TH_ASSERT(filecat_strerror(FILECAT_OK)              != NULL);
    TH_ASSERT(filecat_strerror(FILECAT_ERR_INVALID_ARG) != NULL);
    TH_ASSERT(filecat_strerror(FILECAT_ERR_NOT_FOUND)   != NULL);
    TH_ASSERT(filecat_strerror(FILECAT_ERR_NO_MEMORY)   != NULL);
    TH_ASSERT(filecat_strerror(FILECAT_ERR_OVERFLOW)    != NULL);
    TH_ASSERT(filecat_strerror(FILECAT_ERR_SYSTEM)      != NULL);
    TH_ASSERT(filecat_strerror(FILECAT_ERR_CLOSED)      != NULL);
    /* Out-of-range code must still return a non-NULL static string. */
    TH_ASSERT(filecat_strerror((filecat_status_t)999)   != NULL);
    return 0;
}

static int test_invalid_args(void)
{
    filecat_watcher_t *w = NULL;
    TH_ASSERT_STATUS(filecat_open(NULL, 0, &w), FILECAT_ERR_INVALID_ARG);
    TH_ASSERT_STATUS(filecat_open("/tmp", 0, NULL), FILECAT_ERR_INVALID_ARG);
    TH_ASSERT_STATUS(filecat_open("/this/path/should/not/exist/zzz", 0, &w),
                     FILECAT_ERR_NOT_FOUND);

    char *dir = th_mktmp(); TH_ASSERT(dir);
    char file[512];
    snprintf(file, sizeof(file), "%s/regular.txt", dir);
    TH_ASSERT_EQ(th_touch(file), 0);
    /* Path is a regular file, not a directory. */
    TH_ASSERT_STATUS(filecat_open(file, 0, &w), FILECAT_ERR_INVALID_ARG);

    /* Valid open, then exercise next_event arg-checks. */
    TH_ASSERT_OK(filecat_open(dir, 0, &w));
    filecat_event_t ev;
    TH_ASSERT_STATUS(filecat_next_event(NULL, &ev), FILECAT_ERR_INVALID_ARG);
    TH_ASSERT_STATUS(filecat_next_event(w,    NULL), FILECAT_ERR_INVALID_ARG);

    /* close/destroy on NULL is documented as safe. */
    filecat_close(NULL);
    filecat_destroy(NULL);

    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

static int test_basic_open_close_destroy(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);
    filecat_watcher_t *w = NULL;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));
    TH_ASSERT(w != NULL);

    /* close is idempotent. */
    filecat_close(w);
    filecat_close(w);
    filecat_close(w);

    /* After close, next_event must report CLOSED. */
    filecat_event_t ev;
    TH_ASSERT_STATUS(filecat_next_event(w, &ev), FILECAT_ERR_CLOSED);

    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

static int test_recursive_open_succeeds(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);
    /* Pre-populate a small tree. */
    char a[512], b[512];
    snprintf(a, sizeof(a), "%s/a", dir); TH_ASSERT_EQ(mkdir(a, 0755), 0);
    snprintf(b, sizeof(b), "%s/a/b", dir); TH_ASSERT_EQ(mkdir(b, 0755), 0);

    filecat_watcher_t *w = NULL;
    TH_ASSERT_OK(filecat_open(dir, 1, &w));
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

static int test_create_event(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);
    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));

    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    char path[512];
    snprintf(path, sizeof(path), "%s/created.txt", dir);
    TH_ASSERT_EQ(th_touch(path), 0);

    th_sleep_ms(200);
    th_collector_stop(&col);

    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_CREATED, "created.txt"));
    TH_ASSERT_EQ(col.errors, 0);

    th_collector_free(&col);
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

static int test_modify_event(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);
    char path[512];
    snprintf(path, sizeof(path), "%s/mod.txt", dir);
    TH_ASSERT_EQ(th_touch(path), 0);

    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    TH_ASSERT_EQ(th_write(path, "payload"), 0);

    th_sleep_ms(200);
    th_collector_stop(&col);

    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_MODIFIED, "mod.txt"));

    th_collector_free(&col);
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

static int test_remove_event(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);
    char path[512];
    snprintf(path, sizeof(path), "%s/rm.txt", dir);
    TH_ASSERT_EQ(th_touch(path), 0);

    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    TH_ASSERT_EQ(unlink(path), 0);

    th_sleep_ms(200);
    th_collector_stop(&col);

    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_REMOVED, "rm.txt"));

    th_collector_free(&col);
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

static int test_rename_event_pair_ordered(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);
    char src[512], dst[512];
    snprintf(src, sizeof(src), "%s/before.txt", dir);
    snprintf(dst, sizeof(dst), "%s/after.txt",  dir);
    TH_ASSERT_EQ(th_touch(src), 0);

    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    TH_ASSERT_EQ(rename(src, dst), 0);

    th_sleep_ms(200);
    th_collector_stop(&col);

    pthread_mutex_lock(&col.mu);
    int i_old = th_collector_find_locked(&col, FILECAT_EVENT_RENAMED_OLD, "before.txt");
    int i_new = th_collector_find_locked(&col, FILECAT_EVENT_RENAMED_NEW, "after.txt");
    pthread_mutex_unlock(&col.mu);
    TH_ASSERT(i_old >= 0);
    TH_ASSERT(i_new >= 0);
    /* Contract: RENAMED_OLD precedes RENAMED_NEW. */
    TH_ASSERT(i_old < i_new);

    th_collector_free(&col);
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

static int test_recursive_subdir_events(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);

    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 1, &w));
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    char sub[512];
    snprintf(sub, sizeof(sub), "%s/sub", dir);
    TH_ASSERT_EQ(mkdir(sub, 0755), 0);
    /* Give the consumer time to install the new watch before we write
     * inside the subdirectory — otherwise IN_CREATE for the file races
     * the inotify_add_watch and may legitimately be missed. */
    th_sleep_ms(150);

    char file[512];
    snprintf(file, sizeof(file), "%s/inside.txt", sub);
    TH_ASSERT_EQ(th_touch(file), 0);

    th_sleep_ms(200);
    th_collector_stop(&col);

    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_CREATED, "sub"));
    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_CREATED, "sub/inside.txt"));

    th_collector_free(&col);
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

static int test_non_recursive_drops_subdir_events(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);
    char sub[512];
    snprintf(sub, sizeof(sub), "%s/sub", dir);
    TH_ASSERT_EQ(mkdir(sub, 0755), 0);

    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));   /* not recursive */
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    char rfile[512], sfile[512];
    snprintf(rfile, sizeof(rfile), "%s/at-root.txt", dir);
    snprintf(sfile, sizeof(sfile), "%s/inside.txt",  sub);
    TH_ASSERT_EQ(th_touch(rfile), 0);
    TH_ASSERT_EQ(th_touch(sfile), 0);

    th_sleep_ms(200);
    th_collector_stop(&col);

    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_CREATED, "at-root.txt"));
    TH_ASSERT(!th_collector_contains(&col, FILECAT_EVENT_CREATED, "sub/inside.txt"));

    th_collector_free(&col);
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

static int test_close_from_another_thread_unblocks(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);
    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));

    struct th_delayed_close_arg ca = { w, 200 };
    pthread_t closer;
    TH_ASSERT_EQ(pthread_create(&closer, NULL, th_delayed_close_, &ca), 0);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    filecat_event_t ev;
    filecat_status_t s = filecat_next_event(w, &ev);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    pthread_join(closer, NULL);

    TH_ASSERT_STATUS(s, FILECAT_ERR_CLOSED);
    long ms = th_elapsed_ms(&t0, &t1);
    /* close was scheduled at 200 ms; expect unblock between 150–1000 ms. */
    TH_ASSERT(ms >= 150);
    TH_ASSERT(ms <  1000);

    /* Re-entry continues to return CLOSED. */
    TH_ASSERT_STATUS(filecat_next_event(w, &ev), FILECAT_ERR_CLOSED);

    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

static int test_destroy_from_another_thread_unblocks(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);
    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));

    struct th_blocking_next_arg na = { w, FILECAT_OK };
    pthread_t blocker;
    TH_ASSERT_EQ(pthread_create(&blocker, NULL, th_blocking_next_, &na), 0);

    /* Ensure the consumer is parked in poll() before destroying. */
    th_sleep_ms(200);
    filecat_destroy(w);
    pthread_join(blocker, NULL);

    TH_ASSERT_STATUS(na.result, FILECAT_ERR_CLOSED);
    /* w is invalid here — must not touch it again. */

    th_rmtree(dir); free(dir);
    return 0;
}

static int test_two_watchers_in_one_dir(void)
{
    /* Two independent watchers on the same directory must each receive
     * their own copy of every event (separate inotify instances). */
    char *dir = th_mktmp(); TH_ASSERT(dir);
    filecat_watcher_t *w1, *w2;
    TH_ASSERT_OK(filecat_open(dir, 0, &w1));
    TH_ASSERT_OK(filecat_open(dir, 0, &w2));

    th_collector_t c1, c2;
    TH_ASSERT_EQ(th_collector_start(&c1, w1), 0);
    TH_ASSERT_EQ(th_collector_start(&c2, w2), 0);

    char path[512];
    snprintf(path, sizeof(path), "%s/twin.txt", dir);
    TH_ASSERT_EQ(th_touch(path), 0);

    th_sleep_ms(200);
    th_collector_stop(&c1);
    th_collector_stop(&c2);

    TH_ASSERT(th_collector_contains(&c1, FILECAT_EVENT_CREATED, "twin.txt"));
    TH_ASSERT(th_collector_contains(&c2, FILECAT_EVENT_CREATED, "twin.txt"));

    th_collector_free(&c1);
    th_collector_free(&c2);
    filecat_destroy(w1);
    filecat_destroy(w2);
    th_rmtree(dir); free(dir);
    return 0;
}

int main(void)
{
    TH_RUN(test_strerror_all_codes);
    TH_RUN(test_invalid_args);
    TH_RUN(test_basic_open_close_destroy);
    TH_RUN(test_recursive_open_succeeds);
    TH_RUN(test_create_event);
    TH_RUN(test_modify_event);
    TH_RUN(test_remove_event);
    TH_RUN(test_rename_event_pair_ordered);
    TH_RUN(test_recursive_subdir_events);
    TH_RUN(test_non_recursive_drops_subdir_events);
    TH_RUN(test_close_from_another_thread_unblocks);
    TH_RUN(test_destroy_from_another_thread_unblocks);
    TH_RUN(test_two_watchers_in_one_dir);
    TH_SUMMARY();
}
