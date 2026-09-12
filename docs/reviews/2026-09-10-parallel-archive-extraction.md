# Parallel archive extraction

This records the initial two-archive implementation. The current scheduling and
single-archive optimizations are documented in
[Budgeted full-archive parallelism](2026-09-11-budgeted-unzip-parallelism.md).

## Scope

Unzip All now runs two independent archive workers, rather than overlapping
reads within a solid compression stream. Each worker deletes an original only
after that archive finishes successfully. All workers finish before the existing
single final indexing phase. A caller can retain strictly ordered execution with
`UnzipLimits::maximumConcurrentArchives = 1`; larger requests are capped at two.
Single-archive extraction does not acquire additional decoding workers.

The speedup does not remove the preceding review's resource protection:

- Actual expanded-byte accounting is synchronized across workers.
- Every data write still checks current available space. Pending writes from
  other workers are reserved against that space until flushed; failed attempts
  continue to consume the cumulative byte budget.
- Resource exhaustion stops active workers and the remaining queue, while
  completed output is retained and indexed. User cancellation stays distinct
  from resource failure, and the first resource diagnostic is preserved.
- Output-folder selection and recovery-journal writes are serialized. Archives
  sharing a filename stem receive separate output folders.
- Progress callbacks are serialized, with a monotonic aggregate fraction.

## Paired measurements

Baseline: `43cb651de2d2ab0c3b403dffb60328afca6034d2`.
Both executables use the same desktop debug CMake target's production objects
and dependencies. The benchmark invokes the real `ArchiveUnzipOperation::RunAll`
in Keep Archives mode; it is not a mock or a decoder-only microbenchmark.

Each format has four archives containing two 8 MiB files and 256 8 KiB files per
archive: 72 MiB expanded and 1,032 files per batch. Payloads repeat a deterministic
128 KiB pseudorandom block. Archives are generated with libarchive's ZIP or 7z
writer. Output folders are removed before each iteration, so completion-marker
reuse cannot produce a speedup.

Three paired process runs per mode and format each execute four iterations;
the first iteration in each process is excluded. Process order is reversed for
the middle pair. The table reports medians of the remaining nine extraction
times, from RunAll entry to the final indexing phase. After every process run,
all 1,032 output files are compared byte-for-byte with the expected payloads.
Builds and other native tests do not overlap these measurements.

| Format | Serial median | Two-worker median | Time reduction | Throughput ratio |
| --- | ---: | ---: | ---: | ---: |
| 7z | 0.408320 s | 0.282964 s | 30.7% | 1.44x |
| ZIP | 1.100250 s | 0.612719 s | 44.3% | 1.80x |

Observed whole-process peak RSS was approximately 35.7 to 43.9 MiB for 7z and
40.1 to 57.5 MiB for ZIP. These are fixture measurements, not maximum decoder
memory guarantees: real compression dictionaries and media sizes differ.

These results measure warm local-storage desktop debug execution. They are not
mobile-device speedup estimates, cold-storage measurements, or RAR benchmarks.
The payloads are `.bin` files, so the final indexing pass does not parse charts;
indexing time is excluded from the table. Retained local investigation artifacts
live under the ignored `cmake-build-debug/extraction-benchmark/`: the harness,
baseline and parallel executables, and raw verified timing logs. They are not a
standalone benchmark target shipped in a fresh clone.

## Regression coverage

The operation fixture exercises distinct worker threads, the two-worker cap,
one final indexing pass, monotonic progress, cancellation for actual ZIP and 7z
archives, progress-callback exceptions, shared byte-limit enforcement, and
matching-stem output reservation. Strictly ordered recovery/crash regressions
continue exercising the one-worker mode. The backend fixture retains its
understated-size, free-space, exact-limit, and partial-write accounting coverage.

## Verification

The desktop main and all-target builds, unsigned iOS build, and Android
Firebase Release build-only verification pass. No distribution was performed.
The operation suite passes ten consecutive runs, and the archive backend suite
passes. Independent review reports no remaining introduced P1/P2 findings.

Both full CTest runs pass 356 of 357 tests. `visual_catch_up` times out after
40 seconds, including in an isolated rerun. A live stack sample shows
`testTimelineEdges` tearing down Jukebox/VideoPlayer and joining a decoder thread
that remains in its condition-variable wait. The relevant video implementation
and test files are unchanged; that separate video-teardown issue is not modified
by this extraction change. The full suite is therefore not reported as passing.

ThreadSanitizer instrumentation of the changed production units and operation
fixture builds, but the installed Xcode 26.0 runtime crashes in
`__tsan::SlotLock` during sanitizer/dyld initialization, before `main`. LLDB and
the macOS crash report confirm that startup failure; this is not a passing
sanitizer run or evidence that the program is race-free.
