# Filecat — Design Notes

This document records the design decisions behind Filecat's public API and
its three platform backends. The intent is to make the trade-offs explicit
so callers (and binding authors) can predict behavior without reading the
implementation.

## 1. Goals and non-goals

**Goals.**

- Deliver filesystem change events from a recursively watched directory
  with a uniform C ABI across Linux, Windows, and macOS.
- Keep the public surface small enough to wrap from cgo / Rust FFI /
  Python `ctypes` without a hand-written shim per language.
- Push the recursion strategy down to each OS's native subsystem so the
  caller never pays for a userspace recursive walker.
- Survive bursts: handle queue-overflow conditions without corrupting
  internal state, even if individual events are lost.

**Non-goals.**

- *Reliable replay.* Filecat is a notification library, not a journal. On
  overflow, the caller is told that events were lost; it is the caller's
  responsibility to reconcile by rescanning if reconciliation is required.
- *Content diffing.* Events carry `(type, path)`, not before/after bytes.
- *Filtering DSL.* Glob/regex filtering belongs in the binding layer, not
  the C ABI.
- *Multi-watcher multiplexing in one thread.* The blocking single-threaded
  API (see §2) deliberately leaves this to the caller; cgo wrappers can do
  it cheaply with a goroutine per watcher.

## 2. API shape

### 2.1 Blocking, single-threaded `next_event`

```c
filecat_status_t filecat_next_event(filecat_watcher_t *w, filecat_event_t *ev);
```

`filecat_next_event` blocks the calling thread until an event is available,
the watcher is closed, or a fatal backend error occurs. There is **no
callback registration API**.

Why blocking-pull instead of push-callback:

- **FFI-friendliness.** A callback that fires from a Filecat-owned thread
  would force every binding to deal with thread-attach/detach (`cgocall`
  on the wrong thread, GIL acquisition in Python, `&mut` reentrancy in
  Rust). A blocking call invoked from a caller-owned thread sidesteps all
  of this.
- **Composes with Go.** The expected usage from cgo is one goroutine per
  watcher that forwards events onto a Go channel. The Go scheduler is then
  in charge of fan-out, back-pressure, and cancellation — none of which
  the C library has to know about.
- **No reentrancy traps.** A caller cannot accidentally call `filecat_close`
  from inside an event handler running on a backend thread.

The cost is that one watcher consumes one thread/goroutine. This is
acceptable in practice because the number of watchers is typically O(1)
per application, not O(N) per file.

### 2.2 Event path ownership

The string in `ev.path` is **owned by the watcher** and is guaranteed valid
only until the next call to `filecat_next_event` or `filecat_close` on the
same watcher. Callers that need long-lived storage must copy.

Why this contract:

- **No per-event allocation.** The watcher reuses an internal buffer
  (grown geometrically as needed). For workloads that drain at hundreds of
  thousands of events per second, removing one `malloc`/`free` pair per
  event is a measurable win.
- **Familiar pattern.** This matches `getline(3)`, `readdir(3)`, and
  `strerror(3)`: the function returns a pointer to memory it controls, and
  the next call invalidates it.
- **Cheap to bind.** A cgo wrapper copies the C string into a Go `string`
  immediately after each call, so the ownership rule never leaks into
  Go-land.

The trade-off is that callers cannot pipeline events (e.g. push raw `ev`
pointers onto a queue and process later). This is intentional: pipelining
is a higher-level concern that lives in the binding layer, which already
has to copy across the language boundary anyway.

### 2.3 Status / error model

`filecat_status_t` is a flat enum. The library distinguishes three
categories of return value from `filecat_next_event`:

| Category    | Example                | Caller action                                    |
| ----------- | ---------------------- | ------------------------------------------------ |
| Success     | `FILECAT_OK`           | Consume `ev`, call again.                        |
| Recoverable | `FILECAT_ERR_OVERFLOW`     | Lost events; rescan the tree, then call again.   |
| Fatal       | `FILECAT_ERR_CLOSED`, … | Stop. The watcher is no longer usable.           |

Recoverable statuses are first-class so that the caller can distinguish
"the OS queue overflowed; reconcile" from "this watcher is dead". On
Linux, `IN_Q_OVERFLOW` from the kernel is translated to `FILECAT_OVERFLOW`;
on Windows, an ERROR_NOTIFY_ENUM_DIR return from `GetOverlappedResult`
maps to the same status; on macOS, the `kFSEventStreamEventFlagMustScanSubDirs`
flag triggers it.

Adjust the exact constant names to match `include/filecat/filecat.h`.

## 3. Backend strategy

| Platform | Subsystem                | Subtree handle | Memory shape | Cold start  |
| -------- | ------------------------ | -------------- | ------------ | ----------- |
| Linux    | `inotify`                | one watch per dir | O(N) dirs    | O(N) dirs   |
| Windows  | `ReadDirectoryChangesW`  | one HANDLE        | O(1)         | O(1)        |
| macOS    | `FSEvents`               | one stream        | O(1)         | O(1)        |

The library does not pretend this asymmetry away. It does, however, make
sure the *interface* is uniform: callers see `filecat_open(path, recursive=1)`
on all three platforms and get a `(type, path)` event stream.

### 3.1 Linux — `inotify`

`inotify_init1(IN_NONBLOCK | IN_CLOEXEC)` creates the fd; the backend then
walks the tree in `filecat_open` and calls `inotify_add_watch` once per
directory, holding a `wd → path` map in userspace so events (which carry
only the basename and the `wd`) can be re-assembled into absolute paths.

On `IN_CREATE` for a directory, the backend adds a watch for the new
subtree (recursing into anything that may already exist underneath, since
inotify cannot deliver events for activity that happened before the
`inotify_add_watch` call). On `IN_IGNORED` (which the kernel sends when a
watched directory is removed or its filesystem is unmounted), the wd is
purged from the map.

The watch count is bounded by `fs.inotify.max_user_watches`. The backend
is best-effort: it stops adding new watches when the limit is hit but does
not fail `filecat_open`. The caller can detect partial coverage by checking
`bench_rss` / `bench_open` against the directory count.

The watched directory itself does not produce events: inotify SELF
events (`IN_DELETE_SELF`, `IN_MOVE_SELF`, root-level `IN_ATTRIB`) are
silently dropped. This mirrors `ReadDirectoryChangesW` on Windows, which
never reports changes to the watch root. Callers that need to know
when the root disappears should watch the parent directory.

Known limitation: a small window of events with stale paths may slip
out between the actual rename and our processing of IN_MOVED_FROM.

### 3.2 Windows — `ReadDirectoryChangesW`

A single `CreateFileW` with `FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED`
opens the watch root. `ReadDirectoryChangesW(bWatchSubtree=TRUE, ...)` is
issued against it with a generously sized buffer (subject to the 64 KB
limit on remote filesystems, see §5).

The OS handles recursion internally: one HANDLE covers the entire subtree,
so memory and cold-start are O(1) regardless of subtree size. The cost is
that the buffer is shared: a burst across many subdirectories can exhaust
it and produce `ERROR_NOTIFY_ENUM_DIR`, which is surfaced as the overflow
status.

Paths arrive as UTF-16 relative to the watch root, length-prefixed and not
null-terminated. The backend stitches the watch-root prefix back on,
normalizes separators, and converts to UTF-8 for the public API.

### 3.3 macOS — `FSEvents`

`FSEventStreamCreate` with `kFSEventStreamCreateFlagFileEvents |
kFSEventStreamCreateFlagNoDefer` produces a per-file event stream
rooted at the watch path. The stream is scheduled on a dedicated CFRunLoop
running on a private thread; the public `filecat_next_event` reads from an
internal SPSC queue fed by that thread.

`kFSEventStreamCreateFlagNoDefer` matters: without it, FSEvents coalesces
events over a latency window (default ~1 second) before delivery, which
makes latency benchmarks meaningless. The trade is more wakeups for
high-rate workloads, which is the right default for a notification library
that the caller is already prepared to drain in a tight loop.

`kFSEventStreamEventFlagMustScanSubDirs` is treated as overflow.

## 4. Rename semantics

Rename is the place where the three OSes diverge most loudly, and Filecat
does **not** try to paper that over inside the library. Pairing OLD with
NEW would require either a Linux-style move cookie that doesn't exist on
macOS, or inode-tracking heuristics that lie under load. Instead the
library surfaces what each backend can prove and lets the caller (or a
higher-level binding such as `filecat-go`) reconcile.

| Platform | Native shape | Filecat surface |
| -------- | ------------ | --------------- |
| Linux    | `IN_MOVED_FROM` (with a `cookie`) followed by `IN_MOVED_TO` (same cookie). Either may appear alone when the move crosses the watch boundary. | Two events emitted in order: `FILECAT_EVENT_RENAMED_OLD` for the FROM side, `FILECAT_EVENT_RENAMED_NEW` for the TO side. The cookie is **not** exposed — callers that need pairing rely on adjacency in the event stream. A half-move (FROM or TO without its mate) is emitted as just that one event. |
| Windows  | `FILE_ACTION_RENAMED_OLD_NAME` immediately followed by `FILE_ACTION_RENAMED_NEW_NAME` in the same `ReadDirectoryChangesW` buffer (MSDN-guaranteed adjacency). | Same surface as Linux: `FILECAT_EVENT_RENAMED_OLD` then `FILECAT_EVENT_RENAMED_NEW`. Adjacency is structurally guaranteed by the OS. |
| macOS    | `kFSEventStreamEventFlagItemRenamed` fires on **each** side of the rename, with no shared identifier and no guaranteed adjacency. The flag alone carries no "this is the old side" / "this is the new side" information. | The FSEvents backend can only emit `FILECAT_EVENT_RENAMED_OLD` — once per side. A single rename therefore appears as **two `RENAMED_OLD` events** whose adjacency is best-effort, never guaranteed by the OS. There is no `FILECAT_EVENT_RENAMED_NEW` on macOS. |

**Implication for binding authors.** Cross-platform pairing logic does
*not* belong in the C library; it belongs one layer up, where you also
have a binding-language string type to hold the paired result. The recommended pattern is:

- Linux / Windows: pair `RENAMED_OLD` with the immediately following
  `RENAMED_NEW`. If the next event is not `RENAMED_NEW`, treat the
  `RENAMED_OLD` as an unpaired half-move (the target left the watched
  subtree).
- macOS: treat every `RENAMED_OLD` as "this path may have just changed
  identity; re-`stat` if you care." Two adjacent `RENAMED_OLD` events
  with a parent/sibling relationship are *probably* the two sides of one
  rename, but the library makes no such promise and neither should the
  binding.

This asymmetry is the single most common source of "why does my watcher
behave differently on Mac" bugs in this category of library. Naming the
events `RENAMED_OLD` / `RENAMED_NEW` (instead of a single `RENAMED`)
makes the asymmetry explicit at the API surface rather than hiding it
behind a leaky abstraction.

## 5. Platform-specific notes

### 5.1 Windows long paths

The Windows backend internally normalizes inputs with `GetFullPathNameW`
and prepends `\\?\` (or `\\?\UNC\` for UNC paths) before calling
`CreateFileW`. This unlocks the ~32k character path limit without requiring
the system-wide `LongPathsEnabled` registry setting or a `longPathAware`
application manifest.

Inputs already prefixed with `\\?\` or `\\.\` are passed through unchanged
to respect any explicit caller intent (`\\.\` reaches device namespace,
which `GetFullPathNameW` would otherwise mangle).

Paths emitted in events are presented *without* the `\\?\` prefix, so
downstream consumers see the same path they passed in.

### 5.2 Windows remote filesystems

`ReadDirectoryChangesW` has a documented 64 KB buffer limit on
network shares (the kernel cannot map larger buffers across the SMB
boundary). The backend caps its buffer at 64 KB when the watch root
resolves to a network drive, which slightly raises the overflow rate on
remote trees with high churn. There is no way around this in user space.

### 5.3 Linux `fs.inotify.max_user_watches`

The recursive walk in `filecat_open` honors `EMFILE` / `ENOSPC` from
`inotify_add_watch` and stops adding new watches. The watcher continues
to deliver events for everything that *was* successfully watched. This is
preferred over failing `filecat_open` outright because most callers would
rather have partial coverage than none.

The `bench_rss` and `bench_open` benchmarks deliberately exercise this
boundary; their numbers above the limit reflect the cap, not the requested
subtree size.

### 5.4 FSEvents coalescing

See §3.3. The `NoDefer` flag is non-negotiable for any caller that cares
about p99 latency. The default `latency` parameter passed to
`FSEventStreamCreate` is `0.0`, which combined with `NoDefer` produces
per-event delivery.

## 6. Threading model

### 6.1 Where each backend blocks

- **Linux** ([`src/platform/linux/filecat_linux.c`](../src/platform/linux/filecat_linux.c)).
  The inotify fd is opened with `IN_CLOEXEC` only (blocking). The watcher
  also owns an `eventfd(0, EFD_CLOEXEC)` used purely as a cancel channel.
  `filecat_next_event` parks the caller's thread in `poll(2)` on both
  fds with timeout `-1`. `read(2)` on the inotify fd only runs after
  `POLLIN` fires on it; `POLLIN` on the eventfd, or `POLLERR|POLLHUP|POLLNVAL`
  on either, is translated to `FILECAT_ERR_CLOSED`.
- **Windows.** An IO completion port owned by the watcher. `GetQueuedCompletionStatus`
  is the blocking point. Cancellation is a sentinel completion posted
  by `filecat_close`. Semantics mirror Linux.
- **macOS.** FSEvents runs on a private `CFRunLoop` thread that pushes
  onto an internal SPSC queue. `filecat_next_event` blocks on a condition
  variable attached to that queue; `filecat_close` signals the cvar and
  tears down the run loop.

### 6.2 Lifecycle: refcount + two latches

Every watcher carries an atomic refcount plus two set-once latches,
`closing` and `destroyed`. The model is consistent across all three
backends and is the reason concurrent close/destroy is safe.

```
filecat_open       → refcount = 1                 (owner ref)
filecat_next_event → retain → poll() → release    (refcount transiently +1)
filecat_close      → CAS closing 0→1, wake eventfd, return
filecat_destroy    → CAS destroyed 0→1, ensure closing, release owner ref
last release       → watcher_free()
```

Three things fall out of this model:

1. **`filecat_close` from another thread interrupts an in-flight
   `filecat_next_event`.** The eventfd write is the wake; `poll(2)`
   returns immediately; the in-flight call observes `FILECAT_ERR_CLOSED`
   and returns. No spin, no signal handling.
2. **The watcher cannot be freed underneath an in-flight call.** While
   `filecat_next_event` is parked in `poll(2)`, refcount ≥ 2. Even if
   another thread calls `filecat_destroy` immediately, that call only
   drops the owner ref; the last release (and therefore `watcher_free`)
   doesn't run until the consumer's release also fires.
3. **`filecat_close` and `filecat_destroy` are idempotent.** Both use
   atomic CAS on their respective latches; the second concurrent call
   is a no-op. This matters for cgo finalizer patterns where the
   binding's `Close` and the runtime's finalizer can race.

### 6.3 The contract

- **One concurrent reader per watcher.** `filecat_next_event` is **not**
  internally serialized; two threads calling it on the same watcher
  would race on the internal buffer cursor. Don't do that.
- **`filecat_close` and `filecat_destroy` are safe from any thread, at
  any time**, including during an in-flight `filecat_next_event`. They
  are idempotent.
- **Different watchers are fully independent** and may be driven from
  different threads with no coordination.

### 6.4 The intended cgo shape

```
Goroutine A (owner)            Goroutine B (worker)
─────────────────              ────────────────────
                               for {
                                 filecat_next_event(w, &ev)  // parks in poll(2)
                                 ...send onto channel...
                               }

ctx.Done() fires
filecat_close(w)        ───►   eventfd wakes poll(2)
                               filecat_next_event returns ERR_CLOSED
                               worker exits
join worker
filecat_destroy(w)      ───►   last release → watcher_free
```

This is the model the C ABI was designed for: one goroutine per watcher,
context cancellation closes the watcher, the worker exits cleanly, the
finalizer (or explicit `Close`) destroys. No internal locking on the C
side, no thread attach/detach problems, no callback context confusion.

## 7. Future work

- **Filtering at the C ABI.** A coarse include/exclude prefix list passed
  to `filecat_open` would let the inotify backend skip whole subtrees at
  registration time and save watches; under consideration but not yet
  spec'd.
- **`filecat_interrupt(w)`.** An out-of-band wakeup so callers don't have
  to roll their own. Currently blocked on a coherent semantics for
  partial-event states across the three backends.
- **Coalescing knob.** Optional caller-side coalescing window for
  workloads that prefer fewer wakeups; the macOS backend would just pass
  the value through to `FSEventStreamCreate`'s `latency` parameter.
- **`filecat-go`.** Higher-level Go module on top of `bindings/go/`,
  shipping the goroutine-per-watcher boilerplate, context-based
  cancellation, channel-based fan-out, and a glob/regex filter.
- **Known limitation**: a small window of events with stale paths may slip out between the actual rename and our processing of IN_MOVED_FROM.