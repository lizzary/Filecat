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

struct th_two_touch_arg { const char *p1, *p2; int initial_delay_ms; };
static void *th_two_touch_(void *arg)
{
    struct th_two_touch_arg *a = (struct th_two_touch_arg *)arg;
    th_sleep_ms(a->initial_delay_ms);
    th_touch(a->p1);
    th_sleep_ms(50);
    th_touch(a->p2);
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

/* ============================================================== */
/* Strict API-contract tests                                       */
/* ============================================================== */

/* The typical shutdown sequence: close signals stop, then destroy drops
 * the owner ref. After close, next_event must report CLOSED; destroy
 * must then absorb the already-closed handle without double-free. */
static int test_close_then_destroy(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);
    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));
    filecat_close(w);

    filecat_event_t ev;
    TH_ASSERT_STATUS(filecat_next_event(w, &ev), FILECAT_ERR_CLOSED);

    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

/* destroy without a prior close: the close-handle fallback inside
 * watcher_close_handle must still release the directory handle so the
 * subsequent rmtree on Windows isn't blocked. */
static int test_destroy_without_close(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);
    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));
    filecat_destroy(w);

    th_rmtree(dir);
    /* If destroy leaked the handle, on Windows the directory would still
     * exist (RemoveDirectory would have failed silently inside rmtree). */
#ifdef _WIN32
    DWORD a = GetFileAttributesA(dir);
    TH_ASSERT(a == INVALID_FILE_ATTRIBUTES);
#else
    struct stat st;
    TH_ASSERT(stat(dir, &st) != 0);
#endif
    free(dir);
    return 0;
}

/* CLOSED is sticky: once close has been signalled, every subsequent
 * next_event must keep returning CLOSED without ever blocking. Validates
 * the `closing` latch fast-path at the top of filecat_next_event. */
static int test_next_event_after_close_returns_closed_repeatedly(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);
    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));
    filecat_close(w);

    filecat_event_t ev;
    for (int i = 0; i < 32; i++) {
        filecat_status_t s = filecat_next_event(w, &ev);
        if (s != FILECAT_ERR_CLOSED)
            TH_FAIL("iteration %d: got %s, want closed",
                    i, filecat_strerror(s));
    }

    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

/* filecat.h promises event.path is "relative to the watch root in the
 * OS's native separator form" — so no leading separator, no absolute
 * prefix, no drive letter. */
static int test_event_path_is_relative(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);
    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    char path[1024];
    snprintf(path, sizeof(path), "%s" TH_SEP "leaf.txt", dir);
    TH_ASSERT_EQ(th_touch(path), 0);
    th_sleep_ms(TH_SETTLE_MS);
    th_collector_stop(&col);

    int saw_leaf = 0;
    th_mutex_lock(&col.mu);
    for (int i = 0; i < col.n; i++) {
        const char *p = col.events[i].path;
        TH_ASSERT(p != NULL);
        TH_ASSERT(p[0] != TH_SEP_CHAR);
        TH_ASSERT(strstr(p, dir) == NULL);
#ifdef _WIN32
        /* A relative path never begins with "X:". */
        TH_ASSERT(!(p[0] && p[1] == ':'));
#endif
        if (strcmp(p, "leaf.txt") == 0) saw_leaf = 1;
    }
    th_mutex_unlock(&col.mu);
    TH_ASSERT(saw_leaf);

    th_collector_free(&col);
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

/* Header contract: event.path is "valid only until the next
 * filecat_next_event / filecat_close call". This test drives two events
 * synchronously and duplicates the FIRST event's path *before* issuing
 * the second call — the library must not have corrupted that buffer
 * between the two calls. */
static int test_event_path_lifetime(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);
    char p1[1024], p2[1024];
    snprintf(p1, sizeof(p1), "%s" TH_SEP "first.txt",  dir);
    snprintf(p2, sizeof(p2), "%s" TH_SEP "second.txt", dir);

    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));

    /* Producer fires both touches after a delay long enough for the
     * consumer (this thread) to park inside next_event — on Windows
     * RDCW has to be armed before the touch arrives. */
    struct th_two_touch_arg pa = { p1, p2, 150 };
    th_thread_t prod;
    TH_ASSERT_EQ(th_thread_create(&prod, th_two_touch_, &pa), 0);

    filecat_event_t ev1;
    TH_ASSERT_OK(filecat_next_event(w, &ev1));
    TH_ASSERT(ev1.path != NULL);
    size_t len1 = strlen(ev1.path);
    TH_ASSERT(len1 > 0 && len1 < 1024);
    char *dup1 = (char *)malloc(len1 + 1);
    TH_ASSERT(dup1);
    memcpy(dup1, ev1.path, len1 + 1);

    /* Second call may invalidate ev1.path, but dup1 must survive. */
    filecat_event_t ev2;
    TH_ASSERT_OK(filecat_next_event(w, &ev2));
    TH_ASSERT(ev2.path != NULL);

    th_thread_join(prod);

    /* dup1 still readable and well-formed after the second call. */
    TH_ASSERT(strcmp(dup1, "first.txt")  == 0 ||
              strcmp(dup1, "second.txt") == 0);

    free(dup1);
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

/* Opening with a trailing path separator must be tolerated: on Windows
 * GetFullPathNameW normalizes it away; on POSIX a trailing slash on a
 * directory is always valid. */
static int test_open_with_trailing_separator(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);
    char sub[1024], subSlash[1024], file[1024];
    snprintf(sub,      sizeof(sub),      "%s" TH_SEP "sub", dir);
    TH_ASSERT_EQ(th_mkdir(sub), 0);
    snprintf(subSlash, sizeof(subSlash), "%s" TH_SEP "sub" TH_SEP, dir);
    snprintf(file,     sizeof(file),     "%s" TH_SEP "x.txt", sub);

    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(subSlash, 0, &w));
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    TH_ASSERT_EQ(th_touch(file), 0);
    th_sleep_ms(TH_SETTLE_MS);
    th_collector_stop(&col);

    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_CREATED, "x.txt"));

    th_collector_free(&col);
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

/* mkdir/rmdir on a subdirectory must produce CREATED / REMOVED at the
 * parent. Validates that the directory-name filter is in the watch mask
 * (Windows: FILE_NOTIFY_CHANGE_DIR_NAME; Linux: IN_CREATE/IN_DELETE on
 * subdirectory entries). */
static int test_directory_create_remove_events(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);
    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    char sub[1024];
    snprintf(sub, sizeof(sub), "%s" TH_SEP "newdir", dir);
    TH_ASSERT_EQ(th_mkdir(sub), 0);
    th_sleep_ms(TH_SETTLE_MS);
    TH_ASSERT_EQ(th_rmdir(sub), 0);
    th_sleep_ms(TH_SETTLE_MS);
    th_collector_stop(&col);

    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_CREATED, "newdir"));
    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_REMOVED, "newdir"));
    TH_ASSERT_EQ(col.errors, 0);

    th_collector_free(&col);
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

/* The watch mask includes attribute changes — toggling read-only must
 * surface as MODIFIED. Validates FILE_NOTIFY_CHANGE_ATTRIBUTES (Windows)
 * and IN_ATTRIB -> MODIFIED mapping (Linux). */
static int test_attribute_change_event(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);
    char path[1024];
    snprintf(path, sizeof(path), "%s" TH_SEP "attr.txt", dir);
    TH_ASSERT_EQ(th_touch(path), 0);

    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    TH_ASSERT_EQ(th_set_readonly(path), 0);
    th_sleep_ms(TH_SETTLE_MS);
    th_collector_stop(&col);

    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_MODIFIED, "attr.txt"));
    TH_ASSERT_EQ(col.errors, 0);

    /* Clear before rmtree so unlink can succeed. */
    th_clear_readonly(path);
    th_collector_free(&col);
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

/* Spaces in the watched directory path must work end-to-end. Easy to
 * miss when CreateFileW / GetFullPathNameW are involved — earlier
 * Win32 wrappers used to require quoting. */
static int test_path_with_spaces(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);
    char sub[1024], file[1024];
    snprintf(sub,  sizeof(sub),  "%s" TH_SEP "with space dir", dir);
    TH_ASSERT_EQ(th_mkdir(sub), 0);
    snprintf(file, sizeof(file), "%s" TH_SEP "leaf.txt", sub);

    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(sub, 0, &w));
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    TH_ASSERT_EQ(th_touch(file), 0);
    th_sleep_ms(TH_SETTLE_MS);
    th_collector_stop(&col);

    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_CREATED, "leaf.txt"));

    th_collector_free(&col);
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

/* UTF-8 contract: the header says event.path is UTF-8. Create a file
 * with a non-ASCII name and assert the emitted path bytes match exactly
 * — exercises the UTF-16 -> UTF-8 round trip in the Windows backend
 * (store_event_path) and the raw byte passthrough on POSIX. */
static int test_unicode_filename_event(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);
    /* "测试.txt" in UTF-8 bytes. */
    const char *u8name = "\xe6\xb5\x8b\xe8\xaf\x95.txt";
    char path[1024];
    snprintf(path, sizeof(path), "%s" TH_SEP "%s", dir, u8name);

    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    TH_ASSERT_EQ(th_touch_u8(path), 0);
    th_sleep_ms(TH_SETTLE_MS);
    th_collector_stop(&col);

    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_CREATED, u8name));
    TH_ASSERT_EQ(col.errors, 0);

    th_collector_free(&col);
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

/* Re-opening the same directory after destroy must succeed repeatedly.
 * If destroy leaked the handle, on Windows this would still appear to
 * work (we open with SHARE_READ|WRITE|DELETE) but the handle count would
 * climb; on POSIX the inotify_fd would leak. Either way, 16 iterations
 * is enough to flush out a use-after-free in the shutdown path. */
static int test_open_destroy_repeated_same_dir(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);
    for (int i = 0; i < 16; i++) {
        filecat_watcher_t *w;
        TH_ASSERT_OK(filecat_open(dir, 0, &w));
        filecat_destroy(w);
    }
    th_rmtree(dir); free(dir);
    return 0;
}

/* ============================================================== */
/* Correlation-id contract (event.event_correlation_id +           */
/* filecat_event_pairable)                                         */
/* ============================================================== */

/* Pure read of the field — verify the inline helper without any
 * filesystem activity. Catches a regression where the helper drifts
 * away from the documented "non-zero == pairable" rule. */
static int test_pairable_helper_semantics(void)
{
    filecat_event_t zero    = { FILECAT_EVENT_CREATED,  "x", 0 };
    filecat_event_t one     = { FILECAT_EVENT_CREATED,  "x", 1 };
    filecat_event_t max_val = { FILECAT_EVENT_MODIFIED, "x", UINT64_MAX };

    TH_ASSERT(filecat_event_pairable(&zero)    == 0);
    TH_ASSERT(filecat_event_pairable(&one)     != 0);
    TH_ASSERT(filecat_event_pairable(&max_val) != 0);
    return 0;
}

/* A single rename(2) must surface two events whose event_correlation_id
 * is identical and non-zero, regardless of platform.
 *   Linux:   the two halves share the inotify rename cookie.
 *   Windows: the two halves share the NTFS FileId.
 *   macOS:   the two halves share the FSEvents-reported inode (extended
 *            data); the library exposes both halves as RENAMED_OLD, but
 *            the id is identical, which is what downstream pairs on. */
static int test_rename_pair_shares_correlation_id(void)
{
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

    TH_ASSERT(th_collector_contains_any_rename(&col, "before.txt"));
    TH_ASSERT(th_collector_contains_any_rename(&col, "after.txt"));

    uint64_t id_old = th_collector_correlation_id_any_rename(&col, "before.txt");
    uint64_t id_new = th_collector_correlation_id_any_rename(&col, "after.txt");
    TH_ASSERT(id_old != 0);
    TH_ASSERT(id_new != 0);
    TH_ASSERT_EQ(id_old, id_new);
    TH_ASSERT_EQ(col.errors, 0);

    th_collector_free(&col);
    filecat_destroy(w);
    th_rmtree(dir); free(dir);
    return 0;
}

/* Per-platform correlation_id contract on a plain (non-rename) CREATE:
 *   Linux:           cookie is 0 — inotify never tags non-rename events.
 *   Windows / macOS: every event for a real file has a non-zero id
 *                    (NTFS FileId / FSEvents inode), so a CREATE alone
 *                    carries one even though there is nothing to pair
 *                    with yet (a later delete-then-create that reuses
 *                    the same FileId / inode would be the partner). */
static int test_correlation_id_per_platform(void)
{
    char *dir = th_mktmp(); TH_ASSERT(dir);
    char path[1024];
    snprintf(path, sizeof(path), "%s" TH_SEP "plain.txt", dir);

    filecat_watcher_t *w;
    TH_ASSERT_OK(filecat_open(dir, 0, &w));
    th_collector_t col;
    TH_ASSERT_EQ(th_collector_start(&col, w), 0);

    TH_ASSERT_EQ(th_touch(path), 0);
    th_sleep_ms(TH_SETTLE_MS);
    th_collector_stop(&col);

    TH_ASSERT(th_collector_contains(&col, FILECAT_EVENT_CREATED, "plain.txt"));
    uint64_t id = th_collector_correlation_id(&col,
                                              FILECAT_EVENT_CREATED,
                                              "plain.txt");
#if defined(__linux__)
    TH_ASSERT_EQ(id, 0);
#else
    TH_ASSERT(id != 0);
#endif
    TH_ASSERT_EQ(col.errors, 0);

    th_collector_free(&col);
    filecat_destroy(w);
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
    /* strict API-contract tests */
    TH_RUN(test_close_then_destroy);
    TH_RUN(test_destroy_without_close);
    TH_RUN(test_next_event_after_close_returns_closed_repeatedly);
    TH_RUN(test_event_path_is_relative);
    TH_RUN(test_event_path_lifetime);
    TH_RUN(test_open_with_trailing_separator);
    TH_RUN(test_directory_create_remove_events);
    TH_RUN(test_attribute_change_event);
    TH_RUN(test_path_with_spaces);
    TH_RUN(test_unicode_filename_event);
    TH_RUN(test_open_destroy_repeated_same_dir);
    /* correlation_id contract */
    TH_RUN(test_pairable_helper_semantics);
    TH_RUN(test_rename_pair_shares_correlation_id);
    TH_RUN(test_correlation_id_per_platform);
    TH_SUMMARY();
}
