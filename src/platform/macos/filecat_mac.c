#include "filecat/filecat.h"

#include <CoreServices/CoreServices.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/* ---- internal event queue node ----------------------------------------- */

struct fc_node {
    filecat_event_type_t type;
    char                *rel_path;   /* malloc'd, relative to root */
    struct fc_node      *next;
};

/* ---- watcher structure -------------------------------------------------- */

struct filecat_watcher {
    /* config */
    char    *root;          /* canonical absolute path from realpath() */
    size_t   root_len;      /* strlen(root) */
    int      recursive;     /* 0 -> filter out events not directly in root */

    /* FSEvents */
    FSEventStreamRef stream;
    CFRunLoopRef     run_loop;      /* the loop the stream is scheduled on */
    pthread_t        run_loop_thr;  /* thread that runs CFRunLoopRun() */

    /* event queue: producer = FS callback, consumer = next_event */
    pthread_mutex_t  mu;
    pthread_cond_t   cv;
    struct fc_node  *head;
    struct fc_node  *tail;

    /* scratch buffer for the event.path the consumer is currently holding */
    char    *utf8_path;
    size_t   utf8_capacity;

    /* lifecycle, copied from Windows/Linux model */
    atomic_int refcount;    /* 1 owner + 1 per in-flight call */
    atomic_int closing;     /* set-once latch */
    atomic_int destroyed;   /* set-once latch */
};

/* ---- error mapping ----------------------------------------------------- */

static filecat_status_t map_errno(int e)
{
    switch (e) {
        case ENOENT:
        case ENOTDIR:
            return FILECAT_ERR_NOT_FOUND;
        case ENOMEM:
            return FILECAT_ERR_NO_MEMORY;
        case EINVAL:
            return FILECAT_ERR_INVALID_ARG;
        default:
            return FILECAT_ERR_SYSTEM;
    }
}

/* ---- refcount + close latches -------------------------------------------
 *
 * Mirror the Windows/Linux model exactly.
 */

static void watcher_free(filecat_watcher_t *w)
{
    if (w->stream) {
        /* Should have been invalidated/released already, but be safe. */
        FSEventStreamInvalidate(w->stream);
        FSEventStreamRelease(w->stream);
    }
    /* Free the queue */
    struct fc_node *n = w->head;
    while (n) {
        struct fc_node *next = n->next;
        free(n->rel_path);
        free(n);
        n = next;
    }
    free(w->utf8_path);
    free(w->root);
    pthread_mutex_destroy(&w->mu);
    pthread_cond_destroy(&w->cv);
    free(w);
}

static void watcher_retain(filecat_watcher_t *w)
{
    atomic_fetch_add_explicit(&w->refcount, 1, memory_order_relaxed);
}

static void watcher_release(filecat_watcher_t *w)
{
    if (atomic_fetch_sub_explicit(&w->refcount, 1, memory_order_acq_rel) == 1)
        watcher_free(w);
}

/* Close the FSEventStream and stop the run loop exactly once.
 * Idempotent and thread-safe. */
static void watcher_close_internal(filecat_watcher_t *w)
{
    int expected = 0;
    if (!atomic_compare_exchange_strong(&w->closing, &expected, 1))
        return;

    if (w->stream) {
        FSEventStreamStop(w->stream);
        FSEventStreamInvalidate(w->stream);
    }
    if (w->run_loop)
        CFRunLoopStop(w->run_loop);

    /* Wake any consumer blocked in next_event. */
    pthread_mutex_lock(&w->mu);
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mu);

    /* Wait for the run-loop thread to exit. It releases the stream. */
    if (w->run_loop_thr)
        pthread_join(w->run_loop_thr, NULL);
    w->run_loop_thr = 0;
}

/* ---- FSEvents callback ------------------------------------------------- */

static void fsevents_callback(ConstFSEventStreamRef streamRef,
                              void *clientCallBackInfo,
                              size_t numEvents,
                              void *eventPaths,
                              const FSEventStreamEventFlags eventFlags[],
                              const FSEventStreamEventId eventIds[])
{
    filecat_watcher_t *w = (filecat_watcher_t *)clientCallBackInfo;
    char **paths = (char **)eventPaths;

    /* If the watcher is closing, drop all events. */
    if (atomic_load_explicit(&w->closing, memory_order_acquire))
        return;

    for (size_t i = 0; i < numEvents; i++) {
        const char *abs = paths[i];
        uint32_t flags = eventFlags[i];

        /* ----- overflow handling ----- */
        if (flags & (kFSEventStreamEventFlagMustScanSubDirs |
                     kFSEventStreamEventFlagUserDropped |
                     kFSEventStreamEventFlagKernelDropped)) {
            struct fc_node *node = (struct fc_node *)malloc(sizeof(*node));
            if (!node) continue;  /* drop the overflow event; better than crashing */
            node->type = 0;       /* special marker: overflow */
            node->rel_path = NULL;
            node->next = NULL;

            pthread_mutex_lock(&w->mu);
            if (w->tail) {
                w->tail->next = node;
                w->tail = node;
            } else {
                w->head = w->tail = node;
            }
            pthread_cond_signal(&w->cv);
            pthread_mutex_unlock(&w->mu);
            continue;
        }

        /* ----- filter recursive ----- */
        /* Ensure event is inside root */
        if (strncmp(abs, w->root, w->root_len) != 0)
            continue;
        const char *rel = abs + w->root_len;
        if (*rel == '/') rel++;   /* skip the separator */

        /* Non-recursive: only allow events where the parent is exactly root,
         * i.e. rel contains no '/'. */
        if (!w->recursive) {
            if (strchr(rel, '/') != NULL)
                continue;
        }

        /* ----- classify event ----- */
        filecat_event_type_t type;
        if (flags & kFSEventStreamEventFlagItemRenamed) {
            /* Determine old vs new by checking existence of the item.
             * If the item exists, it's the new name; otherwise old. */
            struct stat st;
            if (lstat(abs, &st) == 0)
                type = FILECAT_EVENT_RENAMED_NEW;
            else
                type = FILECAT_EVENT_RENAMED_OLD;
        } else if (flags & kFSEventStreamEventFlagItemRemoved) {
            type = FILECAT_EVENT_REMOVED;
        } else if (flags & kFSEventStreamEventFlagItemCreated) {
            type = FILECAT_EVENT_CREATED;
        } else if (flags & (kFSEventStreamEventFlagItemModified |
                            kFSEventStreamEventFlagItemInodeMetaMod |
                            kFSEventStreamEventFlagItemXattrMod |
                            kFSEventStreamEventFlagItemFinderInfoMod |
                            kFSEventStreamEventFlagItemChangeOwner)) {
            type = FILECAT_EVENT_MODIFIED;
        } else {
            type = FILECAT_EVENT_MODIFIED;   /* fallback */
        }

        /* ----- enqueue node ----- */
        char *rel_copy = strdup(rel);
        if (!rel_copy) continue;  /* out of memory, drop this event */

        struct fc_node *node = (struct fc_node *)malloc(sizeof(*node));
        if (!node) {
            free(rel_copy);
            continue;
        }
        node->type = type;
        node->rel_path = rel_copy;
        node->next = NULL;

        pthread_mutex_lock(&w->mu);
        if (w->tail) {
            w->tail->next = node;
            w->tail = node;
        } else {
            w->head = w->tail = node;
        }
        pthread_cond_signal(&w->cv);
        pthread_mutex_unlock(&w->mu);
    }
}

/* ---- run-loop thread --------------------------------------------------- */

static void *run_loop_thread(void *arg)
{
    filecat_watcher_t *w = (filecat_watcher_t *)arg;

    w->run_loop = CFRunLoopGetCurrent();

    /* Signal that run_loop is assigned. */
    pthread_mutex_lock(&w->mu);
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mu);

    FSEventStreamScheduleWithRunLoop(w->stream, w->run_loop, kCFRunLoopDefaultMode);
    FSEventStreamStart(w->stream);

    CFRunLoopRun();

    /* Run loop stopped by CFRunLoopStop(). Now release the stream.
     * It has already been invalidated (by watcher_close_internal). */
    if (w->stream) {
        FSEventStreamInvalidate(w->stream);  /* safe even if already invalidated */
        FSEventStreamRelease(w->stream);
        w->stream = NULL;
    }
    w->run_loop = NULL;
    return NULL;
}

/* ---- store event path (scratch buffer) -------------------------------- */

static filecat_status_t store_event_path(filecat_watcher_t *w, const char *rel)
{
    size_t len = strlen(rel);
    if (len + 1 > w->utf8_capacity) {
        char *p = (char *)realloc(w->utf8_path, len + 1);
        if (!p) return FILECAT_ERR_NO_MEMORY;
        w->utf8_path = p;
        w->utf8_capacity = len + 1;
    }
    memcpy(w->utf8_path, rel, len + 1);
    return FILECAT_OK;
}

/* ---- public API -------------------------------------------------------- */

filecat_status_t filecat_open(const char *path, int recursive, filecat_watcher_t **out)
{
    if (!path || !out) return FILECAT_ERR_INVALID_ARG;
    *out = NULL;

    char *root = realpath(path, NULL);
    if (!root) return map_errno(errno);

    struct stat st;
    if (stat(root, &st) != 0) {
        int e = errno;
        free(root);
        return map_errno(e);
    }
    if (!S_ISDIR(st.st_mode)) {
        free(root);
        return FILECAT_ERR_INVALID_ARG;
    }

    filecat_watcher_t *w = (filecat_watcher_t *)calloc(1, sizeof(*w));
    if (!w) {
        free(root);
        return FILECAT_ERR_NO_MEMORY;
    }

    w->root = root;
    w->root_len = strlen(root);
    w->recursive = recursive ? 1 : 0;
    atomic_init(&w->refcount, 1);
    atomic_init(&w->closing, 0);
    atomic_init(&w->destroyed, 0);

    if (pthread_mutex_init(&w->mu, NULL) != 0 ||
        pthread_cond_init(&w->cv, NULL) != 0) {
        watcher_free(w);
        return FILECAT_ERR_SYSTEM;
    }

    /* Create FSEventStream */
    CFStringRef pathRef = CFStringCreateWithCString(NULL, root, kCFStringEncodingUTF8);
    if (!pathRef) {
        watcher_free(w);
        return FILECAT_ERR_NO_MEMORY;
    }
    CFArrayRef paths = CFArrayCreate(NULL, (const void **)&pathRef, 1, &kCFTypeArrayCallBacks);
    if (!paths) {
        CFRelease(pathRef);
        watcher_free(w);
        return FILECAT_ERR_NO_MEMORY;
    }

    FSEventStreamContext ctx = {0};
    ctx.info = w;
    ctx.retain = NULL;
    ctx.release = NULL;
    ctx.copyDescription = NULL;

    FSEventStreamRef stream = FSEventStreamCreate(
        NULL,
        fsevents_callback,
        &ctx,
        paths,
        kFSEventStreamEventIdSinceNow,
        0.0,   /* latency: 0 to get events as soon as possible */
        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer
    );
    CFRelease(paths);
    CFRelease(pathRef);
    if (!stream) {
        watcher_free(w);
        return FILECAT_ERR_SYSTEM;
    }
    w->stream = stream;

    /* Spawn run-loop thread */
    pthread_t thr;
    if (pthread_create(&thr, NULL, run_loop_thread, w) != 0) {
        watcher_free(w);
        return FILECAT_ERR_SYSTEM;
    }
    w->run_loop_thr = thr;

    /* Wait for run_loop to be assigned */
    pthread_mutex_lock(&w->mu);
    while (w->run_loop == NULL) {
        pthread_cond_wait(&w->cv, &w->mu);
    }
    pthread_mutex_unlock(&w->mu);

    *out = w;
    return FILECAT_OK;
}

filecat_status_t filecat_next_event(filecat_watcher_t *w, filecat_event_t *out)
{
    if (!w || !out) return FILECAT_ERR_INVALID_ARG;

    watcher_retain(w);

    filecat_status_t status = FILECAT_OK;

    pthread_mutex_lock(&w->mu);
    while (w->head == NULL) {
        if (atomic_load_explicit(&w->closing, memory_order_acquire)) {
            pthread_mutex_unlock(&w->mu);
            watcher_release(w);
            return FILECAT_ERR_CLOSED;
        }
        pthread_cond_wait(&w->cv, &w->mu);
        /* Re-check closing after spurious wake */
        if (atomic_load_explicit(&w->closing, memory_order_acquire)) {
            pthread_mutex_unlock(&w->mu);
            watcher_release(w);
            return FILECAT_ERR_CLOSED;
        }
    }

    /* Pop front node */
    struct fc_node *node = w->head;
    w->head = node->next;
    if (!w->head) w->tail = NULL;
    pthread_mutex_unlock(&w->mu);

    /* Handle overflow marker */
    if (node->type == 0 && node->rel_path == NULL) {
        free(node);
        watcher_release(w);
        return FILECAT_ERR_OVERFLOW;
    }

    /* Store path in scratch buffer */
    status = store_event_path(w, node->rel_path);
    if (status != FILECAT_OK) {
        free(node->rel_path);
        free(node);
        watcher_release(w);
        return status;
    }

    out->type = node->type;
    out->path = w->utf8_path;

    free(node->rel_path);
    free(node);
    watcher_release(w);
    return FILECAT_OK;
}

void filecat_close(filecat_watcher_t *w)
{
    if (!w) return;
    watcher_retain(w);
    watcher_close_internal(w);
    watcher_release(w);
}

void filecat_destroy(filecat_watcher_t *w)
{
    if (!w) return;
    int expected = 0;
    if (!atomic_compare_exchange_strong(&w->destroyed, &expected, 1))
        return;
    watcher_close_internal(w);
    watcher_release(w);   /* drop owner ref */
}

const char *filecat_strerror(filecat_status_t status)
{
    switch (status) {
        case FILECAT_OK:               return "ok";
        case FILECAT_ERR_INVALID_ARG:  return "invalid argument";
        case FILECAT_ERR_NOT_FOUND:    return "path not found";
        case FILECAT_ERR_NO_MEMORY:    return "out of memory";
        case FILECAT_ERR_OVERFLOW:     return "kernel buffer overflow: events were dropped";
        case FILECAT_ERR_SYSTEM:       return "system error";
        case FILECAT_ERR_CLOSED:       return "watcher closed";
        default:                       return "unknown error";
    }
}