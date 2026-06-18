/* Correctness tests for the public Filecat API.
 *
 * Each test stands alone: open a fresh watcher in a fresh tmpdir, exercise
 * one bit of the contract, destroy, clean up. Per-event timing waits are
 * generous (200 ms typical, 500 ms on macOS via TH_SETTLE_MS) so the suite
 * is reliable on loaded CI hosts.
 *
 * Platform divergences are absorbed by test_helpers.h:
 *   - paths are compared using TH_SEP for the native separator,
 *   - rename assertions use th_collector_contains_any_rename and gate
 *     OLD-before-NEW ordering on TH_RENAME_TRACKED (off on macOS). */

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
    char *dir = th_mktmp(); TH_ASSERT(dir);
    filecat_watcher_t *w = NULL;

    TH_ASSERT_STATUS(filecat_open(NULL, 0, &w),  FILECAT_ERR_INVALID_ARG);
    TH_ASSERT_STATUS(filecat_open(dir,  0, NULL), FILECAT_ERR_INVALID_ARG);

    /* A path that cannot exist on any sane FS. */
    TH_ASSERT_STATUS(
        filecat_open("does/not/exist/zzz-filecat-9999", 0, &w),
        FILECAT_ERR_NOT_FOUND);

    /* Path is a regular file, not a directory. */
    char file[1024];
    snprintf(file, sizeof(file), "%s" TH_SEP "regular.txt", dir);
    TH_ASSERT_EQ(th_touch(file), 0);
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
    char a[1024], b[1024];
    snprintf(a, sizeof(a), "%s" TH_SEP "a",      dir); TH_ASSERT_EQ(th_mkdir(a), 0);
    snprintf(b, sizeof(b), "%s" TH_SEP "a" TH_SEP "b", dir); TH_ASSERT_EQ(th_mkdir(b), 0);

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

    char path[1024];
    snprintf(path, sizeof(path), "%s" TH_SEP "created.txt", dir);
    TH_ASSERT_EQ(th_touch(path), 0);

    th_sleep_ms(TH_SETTLE_MS);
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
    char path[1024];
    snprintf(path, sizeof(path), "%s" TH_SEP "mod.txt", dir);
    TH_ASSERT_EQ(th_touch(path), 0);

    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    TH_ASSERT_EQ(th_write(path, "payload"), 0);

    th_sleep_ms(TH_SETTLE_MS);
    th_collector_stop(&col);

    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_MODIFIED, "mod.txt"));
    TH_ASSERT_EQ(col.errors, 0);

    th_collector_free(&col);
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

static int test_remove_event(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);
    char path[1024];
    snprintf(path, sizeof(path), "%s" TH_SEP "rm.txt", dir);
    TH_ASSERT_EQ(th_touch(path), 0);

    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    TH_ASSERT_EQ(th_unlink(path), 0);

    th_sleep_ms(TH_SETTLE_MS);
    th_collector_stop(&col);

    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_REMOVED, "rm.txt"));
    TH_ASSERT_EQ(col.errors, 0);

    th_collector_free(&col);
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

static int test_rename_events(void)
{
    /* Cross-platform: rename "before.txt" -> "after.txt".
     *   Linux & Windows: strict RENAMED_OLD("before.txt") then
     *                    RENAMED_NEW("after.txt").
     *   macOS:           two RENAMED_OLD events, one per path
     *                    (FSEvents doesn't pair). */
    char *dir = th_mktmp(); TH_ASSERT(dir);
    char src[1024], dst[1024];
    snprintf(src, sizeof(src), "%s" TH_SEP "before.txt", dir);
    snprintf(dst, sizeof(dst), "%s" TH_SEP "after.txt",  dir);
    TH_ASSERT_EQ(th_touch(src), 0);

    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    TH_ASSERT_EQ(th_rename(src, dst), 0);

    th_sleep_ms(TH_SETTLE_MS);
    th_collector_stop(&col);

    /* Every backend reports some rename event for both paths. */
    TH_ASSERT(th_collector_contains_any_rename(&col, "before.txt"));
    TH_ASSERT(th_collector_contains_any_rename(&col, "after.txt"));
    TH_ASSERT_EQ(col.errors, 0);

#if TH_RENAME_TRACKED
    /* Linux/Windows: the contract additionally requires OLD precede NEW
     * with the specific event types. */
    th_mutex_lock(&col.mu);
    int i_old = th_collector_find_locked(&col, FILECAT_EVENT_RENAMED_OLD,
                                         "before.txt");
    int i_new = th_collector_find_locked(&col, FILECAT_EVENT_RENAMED_NEW,
                                         "after.txt");
    th_mutex_unlock(&col.mu);
    TH_ASSERT(i_old >= 0);
    TH_ASSERT(i_new >= 0);
    TH_ASSERT(i_old < i_new);
#endif

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

    char sub[1024];
    snprintf(sub, sizeof(sub), "%s" TH_SEP "sub", dir);
    TH_ASSERT_EQ(th_mkdir(sub), 0);

    /* Linux inotify needs the consumer to install a watch on the new
     * subdir before we write inside it (the IN_CREATE for the file
     * otherwise races inotify_add_watch). The wait is harmless on
     * Windows/macOS where recursion is built into the syscall. */
    th_sleep_ms(TH_SETTLE_MS);

    char file[1024];
    snprintf(file, sizeof(file), "%s" TH_SEP "inside.txt", sub);
    TH_ASSERT_EQ(th_touch(file), 0);

    th_sleep_ms(TH_SETTLE_MS);
    th_collector_stop(&col);

    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_CREATED, "sub"));
    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_CREATED,
                                    "sub" TH_SEP "inside.txt"));
    TH_ASSERT_EQ(col.errors, 0);

    th_collector_free(&col);
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

static int test_non_recursive_drops_subdir_events(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);
    char sub[1024];
    snprintf(sub, sizeof(sub), "%s" TH_SEP "sub", dir);
    TH_ASSERT_EQ(th_mkdir(sub), 0);

    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));   /* recursive=0 */
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    char rfile[1024], sfile[1024];
    snprintf(rfile, sizeof(rfile), "%s" TH_SEP "at-root.txt", dir);
    snprintf(sfile, sizeof(sfile), "%s" TH_SEP "inside.txt",  sub);
    TH_ASSERT_EQ(th_touch(rfile), 0);
    TH_ASSERT_EQ(th_touch(sfile), 0);

    th_sleep_ms(TH_SETTLE_MS);
    th_collector_stop(&col);

    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_CREATED,
                                    "at-root.txt"));
    TH_ASSERT(!th_collector_contains(&col, FILECAT_EVENT_CREATED,
                                     "sub" TH_SEP "inside.txt"));
    TH_ASSERT_EQ(col.errors, 0);

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

    struct th_delayed_close_arg ca = { w, TH_SETTLE_MS };
    th_thread_t closer;
    TH_ASSERT_EQ(th_thread_create(&closer, th_delayed_close_, &ca), 0);

    th_time_t t0 = th_now();
    filecat_event_t ev;
    filecat_status_t s = filecat_next_event(w, &ev);
    th_time_t t1 = th_now();

    th_thread_join(closer);

    TH_ASSERT_STATUS(s, FILECAT_ERR_CLOSED);
    long ms = th_elapsed_ms(t0, t1);
    /* close was scheduled at TH_SETTLE_MS; expect unblock in a wide
     * window — generous to absorb scheduler jitter on loaded hosts. */
    TH_ASSERT(ms >= TH_SETTLE_MS - 100);
    TH_ASSERT(ms <  TH_SETTLE_MS * 10);

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
    th_thread_t blocker;
    TH_ASSERT_EQ(th_thread_create(&blocker, th_blocking_next_, &na), 0);

    /* Ensure the consumer is parked inside filecat_next_event before
     * destroying. */
    th_sleep_ms(TH_SETTLE_MS);
    filecat_destroy(w);
    th_thread_join(blocker);

    TH_ASSERT_STATUS(na.result, FILECAT_ERR_CLOSED);
    /* w is invalid past this point — must not touch it again. */

    th_rmtree(dir); free(dir);
    return 0;
}

static int test_two_watchers_in_one_dir(void)
{
    /* Two independent watchers on the same directory must each receive
     * their own copy of every event. */
    char *dir = th_mktmp(); TH_ASSERT(dir);
    filecat_watcher_t *w1, *w2;
    TH_ASSERT_OK(filecat_open(dir, 0, &w1));
    TH_ASSERT_OK(filecat_open(dir, 0, &w2));

    th_collector_t c1, c2;
    TH_ASSERT_EQ(th_collector_start(&c1, w1), 0);
    TH_ASSERT_EQ(th_collector_start(&c2, w2), 0);

    char path[1024];
    snprintf(path, sizeof(path), "%s" TH_SEP "twin.txt", dir);
    TH_ASSERT_EQ(th_touch(path), 0);

    th_sleep_ms(TH_SETTLE_MS);
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
    TH_RUN(test_rename_events);
    TH_RUN(test_recursive_subdir_events);
    TH_RUN(test_non_recursive_drops_subdir_events);
    TH_RUN(test_close_from_another_thread_unblocks);
    TH_RUN(test_destroy_from_another_thread_unblocks);
    TH_RUN(test_two_watchers_in_one_dir);
    TH_SUMMARY();
}
