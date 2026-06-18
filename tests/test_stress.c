/* Stress tests — moderate but sustained load.
 *
 * Exercises paths that the correctness suite hits only once each: thousands
 * of events, deep recursion, directory renames within the tree, and
 * concurrent close/destroy. They time-out at 60s per test (see CMake).
 *
 * Backend-specific caveats absorbed via test_helpers.h:
 *   - macOS FSEvents may coalesce rapid create+delete on the same path
 *     into a single event with both flags set; we therefore assert
 *     "events were observed" totals, not per-action equality.
 *   - rename ordering is gated on TH_RENAME_TRACKED (off on macOS).
 *   - the inotify per-user watch-count check is Linux-only. */

#include "test_helpers.h"

/* =============================================================== */

static int test_1k_files_in_root(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);
    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    const int N = 1000;
    for (int i = 0; i < N; i++) {
        char p[1024];
        snprintf(p, sizeof(p), "%s" TH_SEP "f%04d.txt", dir, i);
        if (th_touch(p) != 0) TH_FAIL("touch failed at i=%d", i);
    }

    /* Generous drain time: 1000 CREATE events on different paths shouldn't
     * coalesce on any backend. */
    th_sleep_ms(TH_SETTLE_MS * 10);
    th_collector_stop(&col);

    int created = th_collector_count_type(&col, FILECAT_EVENT_CREATED);
    TH_ASSERT_EQ(col.errors, 0);
    TH_ASSERT(col.overflows == 0);
    TH_ASSERT(created >= N);

    th_collector_free(&col);
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

static int test_deep_tree_recursive(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);

    /* Build a 30-level chain BEFORE opening the watcher so the open does
     * the recursive walk all at once. */
    const int depth = 30;
    char path[8192];
    strncpy(path, dir, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    for (int i = 0; i < depth; i++) {
        size_t len = strlen(path);
        snprintf(path + len, sizeof(path) - len, TH_SEP "d%d", i);
        if (th_mkdir(path) != 0)
            TH_FAIL("mkdir failed at depth %d", i);
    }

    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 1, &w));
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    /* Touch a file at the deepest level. */
    {
        size_t len = strlen(path);
        snprintf(path + len, sizeof(path) - len, TH_SEP "leaf.txt");
        TH_ASSERT_EQ(th_touch(path), 0);
    }

    th_sleep_ms(TH_SETTLE_MS * 2);
    th_collector_stop(&col);

    /* Expected relative path = "d0<SEP>d1<SEP>...<SEP>d29<SEP>leaf.txt". */
    char expected[8192];
    size_t off = 0;
    for (int i = 0; i < depth; i++) {
        off += (size_t)snprintf(expected + off, sizeof(expected) - off,
                                "d%d" TH_SEP, i);
    }
    snprintf(expected + off, sizeof(expected) - off, "leaf.txt");
    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_CREATED, expected));
    TH_ASSERT_EQ(col.errors, 0);

    th_collector_free(&col);
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

static int test_directory_rename_within_tree(void)
{
    /* mv a -> b within the watched tree. Verify that:
     *   (1) both backends surface SOME rename event for "a" and "b",
     *       and (where supported) OLD precedes NEW,
     *   (2) a file created in "b/" afterwards reports the NEW path —
     *       not the stale "a/..." — which exercises the rename-driven
     *       wd remap on Linux and FSEvents' inherent canonical-path
     *       behavior elsewhere. */
    char *dir = th_mktmp(); TH_ASSERT(dir);
    char subA[1024], subB[1024], inA[1024], inB[1024];
    snprintf(subA, sizeof(subA), "%s" TH_SEP "a", dir);
    snprintf(subB, sizeof(subB), "%s" TH_SEP "b", dir);
    snprintf(inA,  sizeof(inA),  "%s" TH_SEP "inside.txt", subA);
    TH_ASSERT_EQ(th_mkdir(subA), 0);
    TH_ASSERT_EQ(th_touch(inA),  0);

    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 1, &w));
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    TH_ASSERT_EQ(th_rename(subA, subB), 0);
    th_sleep_ms(TH_SETTLE_MS);

    snprintf(inB, sizeof(inB), "%s" TH_SEP "new.txt", subB);
    TH_ASSERT_EQ(th_touch(inB), 0);

    th_sleep_ms(TH_SETTLE_MS);
    th_collector_stop(&col);

    TH_ASSERT(th_collector_contains_any_rename(&col, "a"));
    TH_ASSERT(th_collector_contains_any_rename(&col, "b"));

    /* Critical: the new file must arrive under "b" with the native
     * separator, not "a". */
    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_CREATED,
                                    "b" TH_SEP "new.txt"));
    TH_ASSERT(!th_collector_contains(&col, FILECAT_EVENT_CREATED,
                                     "a" TH_SEP "new.txt"));
    TH_ASSERT_EQ(col.errors, 0);

#if TH_RENAME_TRACKED
    th_mutex_lock(&col.mu);
    int i_old = th_collector_find_locked(&col, FILECAT_EVENT_RENAMED_OLD, "a");
    int i_new = th_collector_find_locked(&col, FILECAT_EVENT_RENAMED_NEW, "b");
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

static int test_rapid_create_delete_cycles(void)
{
    /* Each iteration touches and immediately removes a unique path.
     * The exact event counts are backend-specific:
     *   - Linux/Windows: 1 CREATED + 1 REMOVED per cycle,
     *   - macOS: FSEvents may coalesce into a single event with both
     *            flags set; map_flags picks REMOVED, so CREATED can be
     *            under-counted.
     * We assert "the watcher observed roughly N events" instead. */
    char *dir = th_mktmp(); TH_ASSERT(dir);
    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    const int N = 500;
    for (int i = 0; i < N; i++) {
        char p[1024];
        snprintf(p, sizeof(p), "%s" TH_SEP "c%04d.txt", dir, i);
        if (th_touch(p)  != 0) TH_FAIL("touch failed at i=%d",  i);
        if (th_unlink(p) != 0) TH_FAIL("unlink failed at i=%d", i);
    }

    th_sleep_ms(TH_SETTLE_MS * 10);
    th_collector_stop(&col);

    TH_ASSERT_EQ(col.errors, 0);
    int c = th_collector_count_type(&col, FILECAT_EVENT_CREATED);
    int r = th_collector_count_type(&col, FILECAT_EVENT_REMOVED);
    /* Combined visibility: we expect at least roughly N events overall.
     * Slack absorbs both coalescing and any overflow recovery. */
    TH_ASSERT(c + r >= N);

    th_collector_free(&col);
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

/* ----- concurrent close idempotency ----- */

struct ccl_arg { filecat_watcher_t *w; int delay_ms; };
static void *ccl_closer_(void *arg)
{
    struct ccl_arg *a = (struct ccl_arg *)arg;
    th_sleep_ms(a->delay_ms);
    filecat_close(a->w);
    return NULL;
}

static int test_concurrent_close_idempotent(void)
{
    /* 16 threads race to filecat_close. Watcher must remain valid for a
     * single subsequent destroy, with no double-free / no hang. */
    char *dir = th_mktmp(); TH_ASSERT(dir);
    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));

    const int N = 16;
    th_thread_t thrs[16];
    struct ccl_arg args[16];
    for (int i = 0; i < N; i++) {
        args[i].w = w;
        args[i].delay_ms = 50 + (i % 4) * 10;   /* jittered start */
        TH_ASSERT_EQ(th_thread_create(&thrs[i], ccl_closer_, &args[i]), 0);
    }
    for (int i = 0; i < N; i++) th_thread_join(thrs[i]);

    /* All subsequent calls must report CLOSED. */
    filecat_event_t ev;
    TH_ASSERT_STATUS(filecat_next_event(w, &ev), FILECAT_ERR_CLOSED);

    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

/* ----- many sibling subdirectories ----- */

static int test_many_subdirs_recursive(void)
{
    /* 200 sibling subdirs, recursive watch. Then touch one file in each
     * and verify all events are reported with correct relative paths. */
    char *dir = th_mktmp(); TH_ASSERT(dir);
    const int N = 200;

#if defined(__linux__)
    /* On Linux each subdir consumes one inotify watch; skip the test if
     * the per-user budget is too low. */
    int max = th_max_user_watches();
    if (max > 0 && max < N + 64) {
        fprintf(stderr, "(skip — max_user_watches=%d) ", max);
        th_rmtree(dir); free(dir);
        return 0;
    }
#endif

    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 1, &w));
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    for (int i = 0; i < N; i++) {
        char sub[1024];
        snprintf(sub, sizeof(sub), "%s" TH_SEP "s%03d", dir, i);
        if (th_mkdir(sub) != 0) TH_FAIL("mkdir at i=%d", i);
    }
    /* Let backends install per-subdir watches before we write inside. */
    th_sleep_ms(TH_SETTLE_MS * 3);

    for (int i = 0; i < N; i++) {
        char f[1024];
        snprintf(f, sizeof(f), "%s" TH_SEP "s%03d" TH_SEP "file.txt",
                 dir, i);
        if (th_touch(f) != 0) TH_FAIL("touch at i=%d", i);
    }
    th_sleep_ms(TH_SETTLE_MS * 5);
    th_collector_stop(&col);

    /* Spot-check first, middle, last. */
    char e[64];
    snprintf(e, sizeof(e), "s%03d" TH_SEP "file.txt", 0);
    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_CREATED, e));
    snprintf(e, sizeof(e), "s%03d" TH_SEP "file.txt", N / 2);
    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_CREATED, e));
    snprintf(e, sizeof(e), "s%03d" TH_SEP "file.txt", N - 1);
    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_CREATED, e));

    TH_ASSERT_EQ(col.errors, 0);

    th_collector_free(&col);
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

int main(void)
{
    TH_RUN(test_1k_files_in_root);
    TH_RUN(test_deep_tree_recursive);
    TH_RUN(test_directory_rename_within_tree);
    TH_RUN(test_rapid_create_delete_cycles);
    TH_RUN(test_concurrent_close_idempotent);
    TH_RUN(test_many_subdirs_recursive);
    TH_SUMMARY();
}
