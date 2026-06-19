/* bench_rss — resident memory as a function of recursive watch size.
 *
 * For N in {0, 10, 100, 1000, 10000}: create N subdirectories under a
 * fresh tmpdir, snapshot RSS, open a recursive watcher, snapshot RSS
 * again, report the delta.
 *
 * Expected shapes (this is the headline):
 *   Linux:        delta scales linearly with N (one inotify watch per dir).
 *   macOS:        delta is ~flat (single FSEvents stream covers the tree).
 *   Windows:      delta is ~flat (single ReadDirectoryChangesW handle).
 *
 * On Linux, /proc/sys/fs/inotify/max_user_watches caps how many watches
 * the kernel will hand out; the backend is best-effort, so an N above the
 * cap won't crash — it'll just underreport coverage. We log the cap. */

#include "bench_helpers.h"
#include "test_helpers.h"

#include "filecat/filecat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int create_subdirs(const char *root, int n)
{
    for (int i = 0; i < n; i++) {
        char p[1024];
        if (snprintf(p, sizeof p, "%s" TH_SEP "sub%06d", root, i) >= (int)sizeof p)
            return -1;
        if (th_mkdir(p) != 0) return -1;
    }
    return 0;
}

static void measure(int n_subdirs)
{
    char *dir = th_mktmp();
    if (!dir) { fprintf(stderr, "  N=%d  mktmp failed\n", n_subdirs); return; }

    if (n_subdirs > 0 && create_subdirs(dir, n_subdirs) != 0) {
        fprintf(stderr, "  N=%d  mkdir failed\n", n_subdirs);
        th_rmtree(dir); free(dir);
        return;
    }

    size_t rss_before = bh_rss_kb();
    filecat_watcher_t *w = NULL;
    filecat_status_t   s = filecat_open(dir, 1, &w);
    if (s != FILECAT_OK) {
        fprintf(stderr, "  N=%d  open: %s\n", n_subdirs, filecat_strerror(s));
        th_rmtree(dir); free(dir);
        return;
    }
    size_t rss_after = bh_rss_kb();

    long delta = (long)rss_after - (long)rss_before;
    printf("  N=%-7d  RSS before=%6zu KB   after=%6zu KB   delta=%+6ld KB\n",
           n_subdirs, rss_before, rss_after, delta);

    filecat_destroy(w);
    th_rmtree(dir); free(dir);
}

int main(void)
{
    printf("bench_rss\n");
#if defined(__linux__)
    int max = th_max_user_watches();
    printf("  fs.inotify.max_user_watches = %d (per-user kernel cap)\n", max);
#endif

    int sizes[] = { 0, 10, 100, 1000, 10000 };
    size_t n_sizes = sizeof sizes / sizeof sizes[0];
    for (size_t i = 0; i < n_sizes; i++) measure(sizes[i]);
    return 0;
}
