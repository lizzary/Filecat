# Benchmark results

Each `.txt` file in this directory is the verbatim stdout of `bench/run.sh`
on a single host. The harness records `uname`, the CPU model, and a UTC
timestamp at the top so every datapoint stays attributable to a specific
build + machine.

## Latest

### Linux — Intel Xeon Gold 6144 @ 3.5 GHz (ESXi VM, 12 vCPU, 6 GB)

Raw: [`2026-06-21-linux-xeon-gold-6144.txt`](2026-06-21-linux-xeon-gold-6144.txt)

| Metric                                  | Result          |
|-----------------------------------------|-----------------|
| Throughput (4 producers, 10 s)          | 84.9 k events/s |
| Throughput overflows                    | **0**           |
| End-to-end latency p50                  | 37 µs           |
| End-to-end latency p99                  | 75 µs           |
| End-to-end latency p999 / max           | 137 / 218 µs    |
| `filecat_open` @ recursive N=10000      | 107 ms          |
| RSS (final) @ recursive N=10000         | 2.2 MB total    |

Highlights:

- **No dropped events at 85 k/s sustained.** `events drained` exactly
  equals `producer ops` — the consumer kept up with 4 saturating
  producers for the full 10 s window, with zero kernel-buffer overflows.
- **Two-digit µs latency.** End-to-end touch → event is 37 µs at the
  median, 75 µs at p99. p999 is still under 140 µs.
- **`filecat_open` is linear in subtree size on Linux** — by design.
  Each subdirectory takes one `inotify_add_watch` syscall, so 10 → 100 →
  1k → 10k subdirs comes out at 0.29 → 0.91 → 9.99 → 107 ms. The slope
  is the kernel's, not the library's.

### Windows — partial smoke run

Raw: [`2026-06-21-windows-msvc-partial.txt`](2026-06-21-windows-msvc-partial.txt)

| N      | `filecat_open` | RSS delta |
|--------|----------------|-----------|
| 0      | 0.032 ms       | +24 KB    |
| 10     | 0.040 ms       | +0 KB     |
| 100    | 0.040 ms       | +8 KB     |
| 1000   | 0.068 ms       | +4 KB     |
| 10000  | 0.066 ms       | +4 KB     |

- **O(1) startup.** One `CreateFileW` covers the whole subtree via
  `bWatchSubtree=TRUE`. N=10 to N=10000 doesn't move the needle.
- **O(1) memory.** The 64 KB ring buffer + `filecat_watcher_t` is
  everything; no per-directory bookkeeping.

### macOS — TBD

Will be filled in after a run on real hardware (the GitHub-hosted runner
is fine for a smoke read but its Apple-Silicon scheduling profile isn't
representative of real-world deployments).

## The headline cross-platform datapoint

`filecat_open` on a recursive watch of 10,000 subdirectories:

| Platform   | Time     | Mechanism                                  |
|------------|----------|--------------------------------------------|
| Windows    | 0.066 ms | one `CreateFileW(..., FILE_FLAG_BACKUP_SEMANTICS)` + `ReadDirectoryChangesW(bWatchSubtree=TRUE)` |
| Linux      | 107 ms   | tree walk + N×`inotify_add_watch`                                                                |

That's a ~1600× spread, and it's **architectural, not a bug** — inotify
has no recursive mode at the kernel layer, so any inotify-based library
pays the same price. The right story to tell here is "Filecat uses the
most native mechanism on each platform," not "Filecat is fast on
Windows."

## Caveats on these numbers

- `bench_rss` reuses one process across all N values. Heap that
  `free()` returns to libc isn't returned to the OS, so the per-N
  `delta` column understates the cost of a fresh watcher and overstates
  for later iterations. The **`after` column** (absolute RSS at the
  moment the watcher is open) is the more honest number — peak ~2.2 MB
  on Linux with 10k watches, peak ~4.9 MB on Windows with one handle.
- These were ad-hoc runs, not the publishable kind. For numbers that
  should appear in marketing material:
  - run on bare metal, no virtualisation
  - pin the binary (`taskset -c 0` on Linux)
  - disable Turbo Boost / SpeedStep so p99 has a stable ceiling
  - take 3 independent runs, publish the median
  - build with `-DCMAKE_BUILD_TYPE=Release`, no sanitizers

## Adding a new result

1. Build the bench tree:
   ```bash
   cmake -B build -DCMAKE_BUILD_TYPE=Release -DFILECAT_BUILD_BENCH=ON
   cmake --build build -j
   ```
2. Run the suite:
   ```bash
   bench/run.sh | tee bench/results/YYYY-MM-DD-<host-slug>.txt
   ```
3. Update the "Latest" section above with a one-line summary + link.
