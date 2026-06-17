# Threading & Cancellation

## Current model (v0.1, Windows backend)

Single-threaded blocking, by design:

- `filecat_next_event` calls `ReadDirectoryChangesW` synchronously (no
  `OVERLAPPED`), so it blocks until either the kernel buffer has events
  or the directory handle is closed.
- `filecat_close` calls `CloseHandle(hDir)` and then `free`s the watcher.

A caller that owns the watcher from a single thread does not hit any
trouble. The race only appears when a second thread tries to cancel a
pending `filecat_next_event`.

## The "close from another goroutine" scenario

The library is intended to be wrapped via cgo into a Go module. The
idiomatic Go API looks like:

```go
w, _ := filecat.Open(path, true)

// goroutine A: drain events
go func() {
    for {
        ev, err := w.Next()   // blocks inside filecat_next_event
        if err != nil {
            return
        }
        handle(ev)
    }
}()

// goroutine B: stop the watcher (e.g. on ctx cancel)
<-ctx.Done()
w.Close()
```

Goroutine A is parked inside `ReadDirectoryChangesW`. B calls
`filecat_close`. Two things happen:

1. `CloseHandle(hDir)` wakes A: `ReadDirectoryChangesW` returns
   `ERROR_OPERATION_ABORTED`, which we map to `FILECAT_ERR_CLOSED`. OK.
2. `filecat_close` then `free`s the watcher struct. If A has not yet
   read `w->current` / `w->utf8_path` on its return path, the next load
   touches freed memory -- use-after-free.

Point (2) is the bug. The current code is safe for purely single-
threaded use, which is the contract v0.1 promises. The cgo binding is
the natural moment to fix this.

## Planned fix (when Go bindings land)

Switch the Windows backend to asynchronous I/O with an explicit cancel
event:

1. `CreateFileW` with `FILE_FLAG_OVERLAPPED`.
2. Per watcher: an `OVERLAPPED` struct with a manual-reset event in
   `hEvent`, plus a second `hCancel` manual-reset event.
3. `filecat_next_event` issues `ReadDirectoryChangesW` (async), then
   `WaitForMultipleObjects(2, {hEvent, hCancel}, ...)`.
4. `filecat_close` does `SetEvent(hCancel)` and `CancelIoEx`, then waits
   on a "drained" flag before freeing memory.

The other backends already lend themselves to this:

| Backend          | Cancel primitive                                       |
|------------------|--------------------------------------------------------|
| Linux `inotify`  | `poll` / `epoll` on the inotify fd + an `eventfd`      |
| macOS `FSEvents` | callback-driven; close stops the stream + run loop     |
| macOS `kqueue`   | `kevent` + a user-event filter as the cancel signal    |

## Until then

The public API does not promise thread safety. `filecat_close` must run
on the same thread as `filecat_next_event`. The Go wrapper, when first
written, should serialize C calls through one owning goroutine until the
async Windows backend is in place.
