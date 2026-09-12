# Budgeted full-archive parallelism

## Execution model

This extends the earlier two-archive implementation, measured against
`ce2e198c58a182711e632a67b51290d3d2931733`.

- One CPU/memory scheduling budget is divided between archive workers and work
  inside each archive. The default CPU ceiling is SDL's reported CPU count.
  Memory defaults to one eighth of reported RAM, clamped to 64 MiB–1 GiB;
  scheduling allows one worker per 64 MiB.
- Automatic archive concurrency is bounded by queued archives, available workers,
  and the larger of two or one quarter of the worker budget. On the measured
  eight-worker device this means two archives with four workers each, or eight
  workers for one archive. Sixteen workers can admit four archives. This is not
  the previous hard two-archive cap. Explicit `maximumConcurrentArchives`
  overrides the preference, without exceeding the shared worker budget.
- Allocations are fixed for a batch; idle capacity is not dynamically reassigned
  when one archive finishes. `maximumConcurrentArchives = 1` orders archives;
  `maximumWorkers = 1` also disables parallel work inside them.
- Supported ZIP entries use independent miniz readers and 64 KiB streaming
  buffers. Each entry's actual size and CRC are checked. Unsupported compression
  retains the existing fallback, not a new cross-backend filename interpretation.
  A ZIP containing only one entry retains that fallback too.
- Large 7z and native-libarchive outputs overlap decoding with one bounded writer
  queue. Small outputs bypass copying. Queued files remain owned until writing
  finishes, and the queue drains before fallback, completion markers, indexing,
  or original deletion. Each queue accounts for staging and in-flight allocations
  within 8 MiB, with at most 128 queued chunks.
- Full 7z extraction uses a private handler with allocated `mt` and `memuse`
  settings and filter threading disabled. The SDK already supported decoder
  threads; this change coordinates them rather than enabling a previously absent
  feature. LZMA2 needs independently decodable chunks. A solid LZMA1 stream cannot
  be split arbitrarily, but its decode and output stages can overlap.

The memory allowance is not a hard process-memory limit. SDK `memuse` controls
threading decisions, not mandatory dictionaries or encoded-header allocations;
archive indexes and decoder allocations can exceed the scheduling allowance.

## Safety and compatibility

Actual expanded-byte/free-space accounting remains shared across the operation.
Writes inside an archive serialize through the existing accounting guard so
pending disk-space reservations remain valid. Failed writes consume the budget;
resource exhaustion does not restart extraction with another backend. Originals
are removed only after successful finalization, and final indexing runs once.

Before overlapping outputs, a filesystem-backed reservation pass detects
equivalent names, including case and Unicode-normalization aliases. Such archives
retain serial overwrite order. Preflight must use the same filename interpretation
as extraction: a ZIP Unicode Path extra-field regression proved that switching
from a miniz index to full libarchive extraction changed output names. That new
route was removed instead of merely disabling its writer thread.

Progress/pause callbacks are serialized, and the modal shows at most three active
archive descriptions plus a remaining-active count. Completion counts still mean
completed archives, not whichever archive most recently reported progress.

## Measurement method

Measurements use the production `ArchiveUnzipOperation::RunAll` in Keep Archives
mode, linked from the same desktop debug target and dependencies. They run on an
Apple M1 Pro with eight logical CPUs and 16 GiB RAM. Each archive contains eight 8 MiB files and 256
8 KiB files: 66 MiB and 264 files, or 264 MiB and 1,056 files for four archives.
Payloads repeat a deterministic 128 KiB pseudorandom block.

ZIP and solid LZMA1 7z fixtures use libarchive's writers. The additional single
LZMA2 fixture uses `7zz a -t7z -m0=lzma2 -md=1m -mmt=8 -ms=on` on the same payloads.
Output folders are removed before every iteration. Every output is compared
byte-for-byte after every iteration, outside the extraction timer.

Each reported case uses two paired processes per mode, four iterations per
process, reversing process order in the second pair. The first iteration in
each process is excluded, leaving six warm samples per median. The timer runs
from RunAll entry to the final indexing phase. No builds or native tests overlap
timed measurements. These are warm local-storage debug measurements, not mobile,
cold-storage, RAR, or BMS parsing benchmarks. The payloads are `.bin` files.

| Workload | Previous median | New median | Time reduction | Throughput ratio |
| --- | ---: | ---: | ---: | ---: |
| One ZIP | 0.944654 s | 0.226736 s | 76.0% | 4.17x |
| Four ZIPs | 2.164870 s | 0.970590 s | 55.2% | 2.23x |
| One solid LZMA1 7z | 0.317388 s | 0.249660 s | 21.3% | 1.27x |
| Four solid LZMA1 7z archives | 0.653507 s | 0.660373 s | -1.1% | 0.99x |
| One LZMA2 7z | 0.246519 s | 0.219527 s | 10.9% | 1.12x |

The LZMA1 batch is approximately unchanged, not a measured speedup. These results
do not imply every archive or format benefits equally.

An initial four-archive/eight-worker schedule made the LZMA1 batch slower than
the baseline despite improving single-archive extraction. Restricting competing
archive streams while retaining the same total worker budget reduced that
regression from about 25% to about 1%; increasing thread count alone was not the
optimization criterion.

Local benchmark source, link helper, and binaries are retained under the ignored
`cmake-build-debug/extraction-benchmark/`. They depend on local production build
objects and are not a standalone benchmark target in a fresh clone.

## Regression coverage and verification

The focused suites cover explicit four-archive admission, independent single-ZIP
entry workers, non-multiplying batch/entry budgets, automatic scheduling at
different CPU/memory allowances, exact output bytes, unsupported ZIP compression,
filesystem aliases, Unicode Path extra-field fallback, writer-thread cancellation,
incomplete markers, and the existing resource/recovery/progress cases. New
concurrency, alias, unsupported-method, filename, and scheduling regressions were
observed failing before their fixes. Independent review has no outstanding
introduced P1/P2 findings; this is not a race-free guarantee.

Desktop main and all-target builds pass. All three focused archive suites pass;
the operation and modal suites additionally pass ten consecutive runs each.
Full parallel CTest passes 356 of 357 tests. The unchanged `visual_catch_up` test
again times out after 40 seconds, matching the earlier extraction review's
video-teardown issue. It is not modified here, and the full suite is not reported
as passing. The earlier installed ThreadSanitizer runtime initialization failure
also remains a verification limitation; no passing sanitizer run is claimed.

The unsigned iOS build (`scripts/ios_firebase_deploy.sh --build-only --skip-init`)
and Android Firebase Release build (`scripts/android_firebase_deploy.sh
--build-only`) pass. No archive distribution or upload was performed. Mobile
build success does not establish on-device speedup.
