/* filecat-watch: tiny demo for the Filecat library.
 *
 *   filecat-watch <path> [recursive=0|1]
 *
 * Prints one line per filesystem event. Single-threaded, blocking; Ctrl+C
 * terminates the process (the OS reclaims the handle on exit). */

#include "filecat/filecat.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

static const char *type_name(filecat_event_type_t t)
{
    switch (t) {
        case FILECAT_EVENT_CREATED:     return "CREATED";
        case FILECAT_EVENT_REMOVED:     return "REMOVED";
        case FILECAT_EVENT_MODIFIED:    return "MODIFIED";
        case FILECAT_EVENT_RENAMED_OLD: return "RENAMED_OLD";
        case FILECAT_EVENT_RENAMED_NEW: return "RENAMED_NEW";
        default:                        return "?";
    }
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    /* Library emits UTF-8; tell the console to decode it as UTF-8 instead of
     * the system codepage (otherwise non-ASCII paths print as garbage). */
    SetConsoleOutputCP(CP_UTF8);
#endif

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s <path> [recursive=0|1]\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    int recursive = (argc == 3) ? atoi(argv[2]) : 0;

    filecat_watcher_t *w = NULL;
    filecat_status_t s = filecat_open(path, recursive, &w);
    if (s != FILECAT_OK) {
        fprintf(stderr, "filecat_open(%s, recursive=%d): %s\n",
                path, recursive, filecat_strerror(s));
        return 1;
    }

    fprintf(stderr, "watching %s (recursive=%d). Ctrl+C to stop.\n",
            path, recursive);

    filecat_event_t ev;
    for (;;) {
        s = filecat_next_event(w, &ev);
        if (s == FILECAT_OK) {
            printf("%-12s %s\n", type_name(ev.type), ev.path);
            fflush(stdout);
        } else if (s == FILECAT_ERR_OVERFLOW) {
            fprintf(stderr, "** overflow: events were dropped\n");
        } else {
            fprintf(stderr, "** %s\n", filecat_strerror(s));
            break;
        }
    }

    filecat_destroy(w);
    return 0;
}
