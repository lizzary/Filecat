# Filecat

A small cross-platform C library for watching directories and emitting raw
filesystem events (created, removed, modified, renamed) from the underlying
OS APIs.

The C library is designed to be wrapped as a Go module via cgo (see
`bindings/go/`, planned).

## Status

| Platform | Backend                 | Status        |
|----------|-------------------------|---------------|
| Windows  | `ReadDirectoryChangesW` | Implemented   |
| Linux    | `inotify`               | Implemented   |
| macOS    | `FSEvents`              | Implemented   |

## Usage

```c
#include <filecat/filecat.h>
#include <stdio.h>

int main(void) {
    filecat_watcher_t *w;
    filecat_status_t s = filecat_open("C:/some/dir", /*recursive=*/1, &w);
    if (s != FILECAT_OK) {
        fprintf(stderr, "open: %s\n", filecat_strerror(s));
        return 1;
    }

    filecat_event_t ev;
    while ((s = filecat_next_event(w, &ev)) == FILECAT_OK) {
        printf("event=%d path=%s\n", (int)ev.type, ev.path);
    }

    filecat_close(w);
    return 0;
}
```

`filecat_next_event` is **blocking and single-threaded**. The string in
`ev.path` is owned by the watcher and remains valid only until the next call
to `filecat_next_event` or `filecat_close`.

`recursive` maps to Windows' `bWatchSubtree`: pass `0` to watch only the
target directory; non-zero to watch all descendants.

### Windows long paths

The Windows backend internally normalizes inputs with `GetFullPathNameW` and
prepends `\\?\` (or `\\?\UNC\` for UNC paths), so directories whose absolute
paths exceed `MAX_PATH` (260) are accepted without requiring the system-wide
`LongPathsEnabled` registry setting. Paths the caller has already prefixed
with `\\?\` or `\\.\` are passed through unchanged.

## Build

```bash
cmake -B build
cmake --build build
```

This produces `libfilecat.a` and the demo CLI `filecat-watch`:

```bash
./build/filecat-watch.exe C:/some/dir 1
```

## License

MIT — see [LICENSE](LICENSE).
