# Porting Filecat to macOS

This document is for the engineer who will implement the macOS backend at
[src/platform/macos/](../src/platform/macos/). It describes:

1. The public contract every backend must honor.
2. The shape of the Windows reference backend
   ([filecat_win.c](../src/platform/windows/filecat_win.c)) so you can mirror its
   lifecycle/threading model.
3. The recommended macOS implementation (FSEvents-based) with the
   platform-specific gotchas spelled out.
4. A step-by-step workflow and a self-check list.

You should treat this as a spec, not a tutorial — the public header
[filecat.h](../include/filecat/filecat.h) and [threading.md](threading.md)
are the source of truth; this doc explains how to satisfy them on macOS.

---

## 1. What every backend must implement

The library exposes exactly five public symbols
([filecat.h](../include/filecat/filecat.h)):

| Symbol              | Role                                                      |
|---------------------|-----------------------------------------------------------|
| `filecat_open`      | Create a watcher on a directory.                          |
| `filecat_next_event`| Block until the next event; return it through `out`.      |
| `filecat_close`     | Idempotent, thread-safe stop signal.                      |
| `filecat_destroy`   | Release the watcher (with CAS-once semantics).            |
| `filecat_strerror`  | Pure function — already implemented per-platform.         |

Each backend defines its own `struct filecat_watcher` (the type is opaque to
users). On Windows the struct holds a `HANDLE`; on macOS it will hold an
`FSEventStreamRef` and a pthread/queue pair (details in §3).

### 1.1 The contract, restated

These are non-negotiable invariants. Re-read them while writing each function:

- **Path encoding.** Input `path` is UTF-8. The path returned through
  `event.path` is UTF-8 **relative to the watch root**, using the OS's native
  separator (`/` on macOS). It is owned by the watcher and may be invalidated
  on the next `filecat_next_event` / `filecat_close` call — store it in a
  watcher-owned scratch buffer that is grown via `realloc`.

- **Recursive.** `recursive == 0` means watch only the target directory
  (one level, no descendants). `recursive != 0` means watch the entire
  subtree. FSEvents is inherently recursive — for the non-recursive case you
  must filter (see §3.4).

- **Event types.** Map every native event onto one of:

  ```
  FILECAT_EVENT_CREATED       = 1
  FILECAT_EVENT_REMOVED       = 2
  FILECAT_EVENT_MODIFIED      = 3
  FILECAT_EVENT_RENAMED_OLD   = 4  /* emitted BEFORE the _NEW event */
  FILECAT_EVENT_RENAMED_NEW   = 5
  ```

  Treat anything you cannot classify as `FILECAT_EVENT_MODIFIED` (the
  Windows backend already does this — see `map_action` in
  [filecat_win.c:54](../src/platform/windows/filecat_win.c#L54)).

- **Status codes.** Map every native error onto a `filecat_status_t`. The
  Windows backend's `map_win_error` ([filecat_win.c:33](../src/platform/windows/filecat_win.c#L33))
  is the model: a small switch that collapses many native codes onto the
  short enum in [filecat.h:8](../include/filecat/filecat.h#L8).
  `FILECAT_ERR_OVERFLOW` is a soft error — the watcher remains valid and the
  caller may keep iterating.

- **Threading.** `filecat_next_event` is single-consumer (do not call it
  concurrently with itself on the same watcher). `filecat_close` and
  `filecat_destroy` MAY race with a blocked `filecat_next_event` and MUST
  unblock it cleanly. See §1.2.

- **Lifetime.** `filecat_destroy` may be called only once per watcher
  pointer, but a racing concurrent call from another thread must not
  double-free. After the first `filecat_destroy` on a pointer has returned,
  the caller must not touch that pointer. The Windows backend uses a
  `destroyed` set-once latch + a refcount; you should mirror this exactly
  (§3.5).

### 1.2 Thread-safety table

Mirror this table — your implementation should pass the same matrix.

| Operation                  | Concurrent with itself | Concurrent with `close`/`destroy` |
|----------------------------|------------------------|-----------------------------------|
| `filecat_next_event`       | **No** (single consumer) | Yes (returns `FILECAT_ERR_CLOSED`) |
| `filecat_close`            | Yes (idempotent)       | Yes                               |
| `filecat_destroy`          | Yes (CAS-once)         | Yes                               |

---

## 2. How the Windows backend works (reference model)

The Windows implementation lives entirely in
[filecat_win.c](../src/platform/windows/filecat_win.c) (~350 LOC). Five
ideas, each transferable to macOS:

### 2.1 One kernel buffer per watcher

`ReadDirectoryChangesW` writes packed `FILE_NOTIFY_INFORMATION` records into
a fixed 64 KB buffer (`FILECAT_BUFFER_SIZE`,
[filecat_win.c:9](../src/platform/windows/filecat_win.c#L9)). One blocking
syscall produces N records; `filecat_next_event` walks them via
`NextEntryOffset` and only re-enters the kernel when the buffer is drained
(`w->current == NULL`).

**macOS analogue:** FSEvents will hand you batches of events through a
callback running on a CFRunLoop thread. You will need a thread-safe queue
between that callback and `filecat_next_event` — the buffer becomes a queue
node list (§3.3).

### 2.2 UTF-8 scratch buffer

`store_event_path` ([filecat_win.c:125](../src/platform/windows/filecat_win.c#L125))
converts the wide name into `w->utf8_path`, growing it with `realloc` as
needed. The returned `event.path` aliases this buffer, which is why the
public header says it is invalidated on the next call.

**macOS analogue:** FSEvents already hands you UTF-8 absolute paths. You
still want a single watcher-owned scratch buffer for the **relative** path
you compute (strip the watch root, copy into the scratch buffer, expose).

### 2.3 Path normalization on input

`utf8_to_wide_path` ([filecat_win.c:85](../src/platform/windows/filecat_win.c#L85))
resolves relative inputs and prepends `\\?\` so MAX_PATH stops mattering.

**macOS analogue:** call `realpath(path, NULL)` to canonicalize and follow
symlinks; remember the canonical absolute path so you can strip it from
event paths later. macOS has no MAX_PATH ceremony, but `realpath` failing
should map to `FILECAT_ERR_NOT_FOUND`.

### 2.4 Idempotent close

`watcher_close_handle` ([filecat_win.c:193](../src/platform/windows/filecat_win.c#L193))
uses `InterlockedExchange(&w->closing, 1) == 0` to gate the real
`CloseHandle` to exactly one caller. Closing the handle is what unblocks
the in-flight `ReadDirectoryChangesW` (with `ERROR_OPERATION_ABORTED`,
mapped to `FILECAT_ERR_CLOSED`).

**macOS analogue:** the equivalent unblock primitive is signaling a
`pthread_cond_t` after marking a `closed` flag. See §3.5.

### 2.5 Refcount + two latches

The lifetime block in [filecat_win.c:154-199](../src/platform/windows/filecat_win.c#L154-L199)
is the most important part to copy:

- `refcount` starts at 1 (the owner ref). Every call to
  `filecat_next_event` / `filecat_close` retains on entry, releases on
  exit. `filecat_destroy` drops the owner ref. The last release runs
  `watcher_free`.
- `closing` is a set-once latch — flipped by either `close` or `destroy`,
  ensuring `CloseHandle` runs exactly once.
- `destroyed` is a separate set-once latch — flipped by `destroy` only,
  ensuring the owner ref is dropped exactly once even if `destroy` is
  called concurrently from two threads.

Why two latches: a user may call `close` and `destroy` separately (one to
unblock the consumer, the other to free). The two latches let `destroy`
work correctly whether or not `close` ran first.

**macOS analogue:** use C11 `<stdatomic.h>` — `atomic_int` for the counter,
`atomic_flag` or `atomic_int` with `atomic_compare_exchange_strong` for the
latches. Functionally identical.

---

## 3. The macOS backend

### 3.1 API choice: FSEvents over kqueue

| API              | Pros                                          | Cons                                                    |
|------------------|-----------------------------------------------|---------------------------------------------------------|
| **FSEvents**     | Recursive by default; native UTF-8 paths; designed for directory trees. | CFRunLoop-driven (callback-on-thread); coalesces events; no native rename pairing. |
| `kqueue` + `EVFILT_VNODE` | poll-style, fits a blocking next-event loop. | One fd per file/dir; **no recursion** — you would have to walk the tree and re-open on every directory add. |

Use **FSEvents**. The library's contract requires `recursive=1` to mean
"watch the whole subtree", and FSEvents is the only macOS API that delivers
that without you re-implementing a tree walker.

Required since macOS 10.7: `kFSEventStreamCreateFlagFileEvents` so you get
per-file events instead of per-directory coalesced notifications. Without
it FSEvents only tells you "something in this directory changed", which is
not granular enough for our event types.

### 3.2 File layout

Create `src/platform/macos/filecat_mac.c`. One translation unit, mirroring
[filecat_win.c](../src/platform/windows/filecat_win.c). Link CoreServices:

```cmake
# CMakeLists.txt — replace the FATAL_ERROR in the APPLE branch
elseif(APPLE)
    set(FILECAT_PLATFORM_SOURCES src/platform/macos/filecat_mac.c)
    # After add_library(filecat ...):
    # target_link_libraries(filecat PRIVATE "-framework CoreServices")
```

(Add the `target_link_libraries` call after the existing `add_library`
line. Keep the link private — consumers don't need CoreServices on their
own link line because Filecat is static.)

### 3.3 Suggested struct layout

```c
struct filecat_watcher {
    /* config */
    char            *root;          /* canonical absolute path from realpath() */
    size_t           root_len;      /* strlen(root); used to strip prefix */
    int              recursive;     /* 0 -> filter out events not in root */

    /* FSEvents */
    FSEventStreamRef stream;
    CFRunLoopRef     run_loop;      /* the loop the stream is scheduled on */
    pthread_t        run_loop_thr;  /* thread that runs CFRunLoopRun() */

    /* event queue: producer = FS callback, consumer = next_event */
    pthread_mutex_t  mu;
    pthread_cond_t   cv;
    /* simple singly-linked list of pending events; head/tail pointers */
    struct fc_node  *head;
    struct fc_node  *tail;

    /* scratch buffer for the event.path the consumer is currently holding */
    char            *utf8_path;
    size_t           utf8_capacity;

    /* lifecycle, copied from Windows model */
    atomic_int       refcount;      /* 1 owner + 1 per in-flight call */
    atomic_int       closing;       /* set-once latch */
    atomic_int       destroyed;     /* set-once latch */
};

struct fc_node {
    filecat_event_type_t type;
    char                *rel_path;  /* malloc'd, owned by the node */
    struct fc_node      *next;
};
```

### 3.4 The threading model

FSEvents pushes events into a callback that runs on a CFRunLoop. The
public API exposes a blocking `filecat_next_event`. The bridge:

```
 FSEvents callback (run loop thread)        next_event (consumer thread)
 ┌────────────────────────────────┐         ┌────────────────────────────┐
 │ for each (path, flags) in batch│         │ pthread_mutex_lock(&mu)    │
 │   classify -> type             │         │ while (head == NULL        │
 │   make rel_path                │ ─────►  │        && !closing)        │
 │   pthread_mutex_lock(&mu)      │         │   pthread_cond_wait(&cv,&mu│
 │   append node to tail          │         │ if closing -> CLOSED       │
 │   pthread_cond_signal(&cv)     │         │ pop head; copy to utf8_path│
 │   pthread_mutex_unlock(&mu)    │         │ pthread_mutex_unlock(&mu)  │
 └────────────────────────────────┘         └────────────────────────────┘
```

`filecat_open` spawns `run_loop_thr`. Inside it:

```c
w->run_loop = CFRunLoopGetCurrent();
FSEventStreamScheduleWithRunLoop(w->stream, w->run_loop, kCFRunLoopDefaultMode);
FSEventStreamStart(w->stream);
CFRunLoopRun();   /* blocks until CFRunLoopStop() */
/* on exit: invalidate + release the stream here */
```

Synchronization: wait for `run_loop` to be assigned before returning from
`filecat_open` (e.g. with a one-shot condvar) — `filecat_close` calls
`CFRunLoopStop(run_loop)` and that pointer must be valid by the time the
caller can ever call `close`.

**Recursive=0 filter.** FSEvents gives you absolute paths under `root`.
For non-recursive watches, accept an event only when its parent directory
is exactly `root` — i.e. there is no further `/` after the `root_len`-byte
prefix. Drop everything else inside the callback before enqueueing.

**Path strip.** After confirming the absolute path starts with `root` (use
the canonical, `realpath`'d form), the relative path is `abs + root_len`,
skipping a leading `/` if present. Empty relative path (the root itself)
is valid — emit it as `""`.

### 3.5 Mapping FSEvents flags to event types

`FSEventStreamEventFlags` is a bitmask. One physical event may have
several flags set (e.g. `Created | Modified`). Classify with this priority:

| Flag                                | Mapped type                         |
|-------------------------------------|-------------------------------------|
| `kFSEventStreamEventFlagItemRenamed`| See note below                      |
| `kFSEventStreamEventFlagItemRemoved`| `FILECAT_EVENT_REMOVED`             |
| `kFSEventStreamEventFlagItemCreated`| `FILECAT_EVENT_CREATED`             |
| `kFSEventStreamEventFlagItemModified` <br/> `…ItemInodeMetaMod` <br/> `…ItemXattrMod` <br/> `…ItemFinderInfoMod` <br/> `…ItemChangeOwner` | `FILECAT_EVENT_MODIFIED` |

**Renames.** FSEvents does NOT pair renames the way Windows does. It just
sets `ItemRenamed` on both the old and new path (which may arrive in the
same batch or different batches). The contract requires that
`FILECAT_EVENT_RENAMED_OLD` precedes `FILECAT_EVENT_RENAMED_NEW`. A simple
and correct strategy:

- If `ItemRenamed` is set and the path **no longer exists** on disk (use
  `lstat`), emit `FILECAT_EVENT_RENAMED_OLD`.
- If `ItemRenamed` is set and the path **does exist**, emit
  `FILECAT_EVENT_RENAMED_NEW`.

This satisfies the ordering rule within a batch (you process renames in
batch order) and across batches (the OLD always arrives first because the
file disappeared before the new one appeared). It is also the strategy
fsnotify and watchman use. Document this in a comment — the heuristic is
not obvious.

**Overflow.** If the callback receives an event with
`kFSEventStreamEventFlagMustScanSubDirs` or
`kFSEventStreamEventFlagUserDropped` / `KernelDropped`, push a sentinel
node that makes `filecat_next_event` return `FILECAT_ERR_OVERFLOW` once,
then drain normally. The Windows backend returns OVERFLOW directly from
the read call; you'll do it through the queue.

### 3.6 Lifecycle (verbatim mirror of Windows)

```c
static void watcher_close(filecat_watcher_t *w)
{
    int expected = 0;
    if (atomic_compare_exchange_strong(&w->closing, &expected, 1)) {
        /* exactly one caller gets here */
        FSEventStreamStop(w->stream);
        FSEventStreamInvalidate(w->stream);  /* unschedules from run loop */
        CFRunLoopStop(w->run_loop);          /* unblocks the run-loop thread */
        pthread_mutex_lock(&w->mu);
        pthread_cond_broadcast(&w->cv);      /* wake blocked next_event */
        pthread_mutex_unlock(&w->mu);
        pthread_join(w->run_loop_thr, NULL); /* run-loop thread releases the stream */
    }
}

void filecat_close(filecat_watcher_t *w)
{
    if (!w) return;
    watcher_retain(w);
    watcher_close(w);
    watcher_release(w);
}

void filecat_destroy(filecat_watcher_t *w)
{
    if (!w) return;
    int expected = 0;
    if (!atomic_compare_exchange_strong(&w->destroyed, &expected, 1)) return;
    watcher_close(w);
    watcher_release(w);   /* drop owner ref */
}
```

`watcher_free` (run when the last release decrements to 0) tears down the
mutex/condvar, frees the queue, frees `root` / `utf8_path`, then `free(w)`.

### 3.7 Errno mapping

Mirror `map_win_error`. Approximate table:

| Source                           | Status                       |
|----------------------------------|------------------------------|
| `realpath` -> `ENOENT`           | `FILECAT_ERR_NOT_FOUND`      |
| `realpath` -> `EACCES` / other   | `FILECAT_ERR_SYSTEM`         |
| `stat` says not a directory      | `FILECAT_ERR_INVALID_ARG`    |
| `FSEventStreamCreate` returns NULL | `FILECAT_ERR_SYSTEM`       |
| any `malloc`/`pthread_*` failure | `FILECAT_ERR_NO_MEMORY` / `FILECAT_ERR_SYSTEM` |
| internal `closing` flag set      | `FILECAT_ERR_CLOSED`         |
| FSEvents dropped/scan flags      | `FILECAT_ERR_OVERFLOW`       |

---

## 4. Workflow

A suggested order of work. Each step is testable on its own.

1. **CMake wiring.** Replace the `FATAL_ERROR` in the APPLE branch of
   [CMakeLists.txt](../CMakeLists.txt) (line 10–11) with the snippet in §3.2
   and add the `target_link_libraries(filecat PRIVATE "-framework CoreServices")`
   call. Confirm `cmake -B build && cmake --build build` produces an empty
   `libfilecat.a` with the (still-empty) `.c` file linking.

2. **Skeleton + strerror.** Create `filecat_mac.c` with stub
   implementations that all return `FILECAT_ERR_SYSTEM`, plus the real
   `filecat_strerror` (copy verbatim from
   [filecat_win.c:339](../src/platform/windows/filecat_win.c#L339)).

3. **Open & destroy, no events.** Implement `filecat_open` (validate the
   path with `realpath` + `stat`, allocate the struct, init mutex/cond,
   create the FSEventStream with an empty callback, spawn the run-loop
   thread, wait for `run_loop` to be set). Implement `filecat_close` and
   `filecat_destroy` per §3.6. Verify with: open a watcher, call destroy,
   no crash, no leaks under leaks/Instruments.

4. **Single-threaded event loop.** Wire the FSEvents callback to translate
   flags (§3.5) and enqueue nodes; have `filecat_next_event` pop them.
   Build the existing `examples/watch.c` against the new backend and
   verify event types match expectations: create, write, rename, delete
   files in the watched directory.

5. **Recursive=0 filter.** Add the parent-directory check (§3.4). Test by
   creating files in both the root and a subdirectory; only root-level
   events should appear.

6. **Cancellation race.** Run `examples/watch.c` modified to call
   `filecat_close` from a second pthread after `sleep(1)`. The pending
   `filecat_next_event` must return `FILECAT_ERR_CLOSED` promptly. Run it
   under TSan if possible.

7. **Overflow / scan flags.** Generate many events quickly (e.g.
   `for i in $(seq 1 100000); do touch /tmp/watch/$i; done`) and confirm
   the watcher either drains them all or returns `FILECAT_ERR_OVERFLOW`
   without crashing and continues delivering subsequent events.

8. **README + status table update.** Flip the macOS row of the status
   table in [README.md](../README.md) to "Implemented".

---

## 5. Self-check before sending the PR

- [ ] `filecat_next_event` never holds the mutex across the
      `pthread_cond_wait` exit AND a copy of `out->path` — release the
      mutex once the node is popped; copy into the scratch buffer with no
      lock held.
- [ ] The run-loop thread releases (`FSEventStreamRelease`) the stream
      after `CFRunLoopRun()` returns, not from `filecat_close`. Mixing
      Core Foundation release across threads in flight is the usual
      crash.
- [ ] `realpath()`'s result is `free`d (it `malloc`s when the second arg
      is NULL).
- [ ] No event is enqueued after `closing` is set. Check the flag inside
      the callback under the mutex, or drop events at the consumer.
- [ ] The reference count goes back to zero in all of: clean close+destroy,
      destroy-only, close-then-destroy from two threads, destroy called
      while `next_event` is blocked.
- [ ] Build with `-Wall -Wextra -Wpedantic -fsanitize=address,undefined`
      and run the example. CI knobs already match (see
      [CMakeLists.txt:27](../CMakeLists.txt#L27)).

---

## References

- Public header: [include/filecat/filecat.h](../include/filecat/filecat.h)
- Threading rationale: [docs/threading.md](threading.md)
- Reference impl: [src/platform/windows/filecat_win.c](../src/platform/windows/filecat_win.c)
- Apple docs: *File System Events Programming Guide*; `man 3 FSEvents`;
  headers in `CoreServices/FSEvents.h`.
