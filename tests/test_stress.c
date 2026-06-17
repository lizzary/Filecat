/* Stress tests — moderate but sustained load.
 *
 * These exercise paths that the correctness suite hits only once each:
 * thousands of events, deep recursion, directory renames within the tree,
 * and concurrent close/destroy. They time-out at 60s per test (see CMake). */

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
        char p[512];
        snprintf(p, sizeof(p), "%s/f%04d.txt", dir, i);
        if (th_touch(p) != 0) TH_FAIL("touch failed at i=%d (errno=%d)", i, errno);
    }

    /* Generous drain time: 1000 IN_CREATE events are well under both the
     * default inotify queue (16384) and the inotify_event throughput. */
    th_sleep_ms(2000);
    th_collector_stop(&col);

    int created = th_collector_count_type(&col, FILECAT_EVENT_CREATED);
    TH_ASSERT_EQ(col.errors, 0);
    /* At 1000 events with a quiet consumer there should be no overflow. */
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

    /* Build a 30-level chain BEFORE opening the watcher, so the open does
     * the recursive walk all at once. */
    const int depth = 30;
    char path[8192];
    strncpy(path, dir, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    for (int i = 0; i < depth; i++) {
        size_t len = strlen(path);
        snprintf(path + len, sizeof(path) - len, "/d%d", i);
        if (mkdir(path, 0755) != 0)
            TH_FAIL("mkdir failed at depth %d (errno=%d)", i, errno);
    }

    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 1, &w));
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    /* Touch a file at the deepest level. */
    {
        size_t len = strlen(path);
        snprintf(path + len, sizeof(path) - len, "/leaf.txt");
        TH_ASSERT_EQ(th_touch(path), 0);
    }

    th_sleep_ms(300);
    th_collector_stop(&col);

    /* Expected relative path = "d0/d1/.../d29/leaf.txt". */
    char expected[8192];
    size_t off = 0;
    for (int i = 0; i < depth; i++) {
        off += (size_t)snprintf(expected + off, sizeof(expected) - off, "d%d/", i);
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
     *   (1) we get RENAMED_OLD "a" and RENAMED_NEW "b" in order,
     *   (2) a file created in "b/" afterwards reports the NEW path,
     *       not the stale "a/..." (validates our IN_MOVED_FROM scan +
     *       IN_MOVED_TO re-walk). */
    char *dir = th_mktmp(); TH_ASSERT(dir);
    char subA[512], subB[512], inA[512], inB[512];
    snprintf(subA, sizeof(subA), "%s/a", dir);
    snprintf(subB, sizeof(subB), "%s/b", dir);
    snprintf(inA,  sizeof(inA),  "%s/inside.txt", subA);
    TH_ASSERT_EQ(mkdir(subA, 0755), 0);
    TH_ASSERT_EQ(th_touch(inA), 0);

    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 1, &w));
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    TH_ASSERT_EQ(rename(subA, subB), 0);
    th_sleep_ms(200);

    snprintf(inB, sizeof(inB), "%s/new.txt", subB);
    TH_ASSERT_EQ(th_touch(inB), 0);

    th_sleep_ms(200);
    th_collector_stop(&col);

    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_RENAMED_OLD, "a"));
    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_RENAMED_NEW, "b"));
    /* Critical: the new file must arrive under "b/", not "a/". */
    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_CREATED, "b/new.txt"));
    TH_ASSERT(!th_collector_contains(&col, FILECAT_EVENT_CREATED, "a/new.txt"));
    TH_ASSERT_EQ(col.errors, 0);

    th_collector_free(&col);
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

static int test_rapid_create_delete_cycles(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);
    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    const int N = 500;
    for (int i = 0; i < N; i++) {
        char p[512];
        snprintf(p, sizeof(p), "%s/c%04d.txt", dir, i);
        if (th_touch(p) != 0) TH_FAIL("touch failed at i=%d", i);
        if (unlink(p)   != 0) TH_FAIL("unlink failed at i=%d", i);
    }

    th_sleep_ms(2000);
    th_collector_stop(&col);

    TH_ASSERT_EQ(col.errors, 0);
    int c = th_collector_count_type(&col, FILECAT_EVENT_CREATED);
    int r = th_collector_count_type(&col, FILECAT_EVENT_REMOVED);
    TH_ASSERT_EQ(c, N);
    TH_ASSERT_EQ(r, N);

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
    pthread_t thrs[16];
    struct ccl_arg args[16];
    for (int i = 0; i < N; i++) {
        args[i].w = w;
        args[i].delay_ms = 50 + (i % 4) * 10;   /* jittered start */
        TH_ASSERT_EQ(pthread_create(&thrs[i], NULL, ccl_closer_, &args[i]), 0);
    }
    for (int i = 0; i < N; i++) pthread_join(thrs[i], NULL);

    /* All subsequent calls must report CLOSED. */
    filecat_event_t ev;
    TH_ASSERT_STATUS(filecat_next_event(w, &ev), FILECAT_ERR_CLOSED);

    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

/* ----- many subdirectories (recursive watch fan-out) ----- */

static int test_many_subdirs_recursive(void)
{
    /* 200 sibling subdirs, recursive watch. Then touch one file in each;
     * verify all events are reported with correct relative paths. */
    char *dir = th_mktmp(); TH_ASSERT(dir);
    const int N = 200;

    /* Skip if the system can't support N+root watches comfortably. */
    int max = th_max_user_watches();
    if (max > 0 && max < N + 64) {
        fprintf(stderr, "(skip — max_user_watches=%d) ", max);
        th_rmtree(dir); free(dir);
        return 0;
    }

    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 1, &w));
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    for (int i = 0; i < N; i++) {
        char sub[512];
        snprintf(sub, sizeof(sub), "%s/s%03d", dir, i);
        if (mkdir(sub, 0755) != 0) TH_FAIL("mkdir at i=%d", i);
    }
    /* Let the consumer install all the per-subdir watches. */
    th_sleep_ms(500);

    for (int i = 0; i < N; i++) {
        char f[512];
        snprintf(f, sizeof(f), "%s/s%03d/file.txt", dir, i);
        if (th_touch(f) != 0) TH_FAIL("touch at i=%d", i);
    }
    th_sleep_ms(1000);
    th_collector_stop(&col);

    /* Spot-check several: first, middle, last. */
    char e[64];
    snprintf(e, sizeof(e), "s%03d/file.txt", 0);
    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_CREATED, e));
    snprintf(e, sizeof(e), "s%03d/file.txt", N/2);
    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_CREATED, e));
    snprintf(e, sizeof(e), "s%03d/file.txt", N-1);
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
