# Android folder import performance

## Scope and implementation

Skin folder import and chart Copy/Move now use a bounded pool sized from
`Runtime.availableProcessors()`, with the chart parser's four-worker fallback
when hardware parallelism is unavailable. The pool caps at eight concurrent
streams and eight reusable 1 MiB buffers; no more than twice the worker count
is queued or running. A single-core device uses one worker.

Chart Copy keeps work queued across directory boundaries. Move still finishes
all copies and fsyncs within each subtree, checks destination hashes and source
metadata, and holds the existing destination reservation before deleting that
subtree. Import cleanup closes source streams and drains writers before deleting
owned staging. Progress callbacks are serialized and aggregate byte limits are
reserved before concurrent writes.

Every SAF query/open receives a separate cancellation signal. A shared signal
cannot safely represent concurrent ContentResolver calls. User cancellation
fans out to outstanding skin provider calls; internal failure cleanup aborts
provider calls without changing the user-cancellation flag or hiding the error.

Provider access remains in Java. The measured native CPU hot path was SHA-256,
which Android previously implemented with portable scalar C++. Android now uses
OpenSSL EVP from the TLS dependency already bundled with the application, with
hardware dispatch and the same digest/value semantics. Apple CommonCrypto and
the other platforms' fallback are unchanged.

## Device measurements

Galaxy S20 FE (SM-G781N), Android 13, eight reported processors. The read-only
fixture was `Download/Skins/simple-play-simple`: 661 files, 235,049,999 bytes.
Both Java engines copied this same asset tree, including when measuring the
chart engine. These are warm-cache measurements, not cold-storage or remote
cloud-provider benchmarks. Output was private temporary storage, and every
Java output file was checked against its source SHA-256 outside the timing.

Initial sequential engine measurements were 16,905 ms for skin copy and
15,854 ms for chart Copy, including provider traversal. The initial parallel
version measured 5,143 ms for skin and 9,515 ms for charts; the chart measurement
exposed unnecessary waits at subfolder boundaries, subsequently removed.
The final signed application measured **5,106 ms for skin copy** and
**5,997 ms for chart Copy**, with every output hash verified. Relative to the
initial sequential observations, these are 3.31× and 2.64× faster respectively.

The initial baseline used the SDL activity to obtain the picker grant; the
later harness substitutes a picker-only activity to avoid a competing renderer.
These separate runs are observational comparisons rather than controlled
whole-application latency benchmarks.

An isolated transfer probe with the same provider, data and reusable 1 MiB
buffer measured 11,856–12,536 ms with one worker, 5,894–6,493 ms with two, and
2,810–2,901 ms with four. A subsequent four/eight-worker probe measured
2,874–4,315 ms with four and 2,162–2,296 ms with eight. These probes exclude
provider traversal and support using device parallelism with a bounded ceiling.

The native follow-up used the actual `SkinArchiveImporter::prepareFolder`,
`liveSources=true`, and an internal copy of the fixture, matching the staged
input of the Android skin flow. Separate NDK executables differed only in the
SHA-256 backend; the order was before/after/after/before with no concurrent
phone benchmark:

| Native preparation | Before | After |
| --- | ---: | ---: |
| First run | 7,903 ms | 5,189 ms |
| Second run | 8,628 ms | 5,014 ms |
| Mean | 8,266 ms | 5,102 ms |

This is a 38% reduction in the complete native preparation step, including
copying, hashing, read-back verification, synchronization and snapshots; all
runs found the same 13 skin entries. The standalone SHA-256 probe over 256 MiB
measured approximately 1,203 ms before and 148 ms after with identical digests.
The engine and native figures describe separate stages, not a measured
picker-to-ready total.

## Reproduction

Use a signed restricted-file-access build and the release Android test APK.
The instrumentation mode `import-copy-benchmark` accepts `-e source` with a
DocumentsProvider document ID (default
`primary:Download/Skins/simple-play-simple`). Select that exact folder in the
system picker. The harness reads the source, creates disposable outputs under
the app cache, verifies all output hashes, and logs timings under `ImportBench`.

The optional `file_checksum_tests --benchmark` reports 256 MiB checksum time;
ordinary CTest has no timing assertion. The Android OpenSSL configuration can
be exercised by compiling that target's sources with the NDK and
`ASOBMASHOW_USE_OPENSSL_SHA256=1`, linked against the bundled `libcrypto`.

## Regression coverage

- Device-budget tests cover one/two processors, unavailable hardware count, and
  the eight-worker ceiling, plus blocked-read cancellation and failure cleanup.
- Engine tests verify exact concurrent outputs, monotonic progress, aggregate
  bounds, Copy overlap across subfolders, and completed/fsynced Move files before
  source deletion. They also pass with `-XX:ActiveProcessorCount=1`.
- Instrumentation includes two blocked provider opens plus a completed query,
  cancellation fan-out, late-open rejection and internal-error versus user-cancel
  separation. Its in-process ContentProvider checks signal isolation; it is not
  a remote Binder or third-party cloud-provider test.
- Native checksum vectors cover padding boundaries, chunking, state copying,
  finalization stability and known large-input digests.

## Final verification

- Signed `restricted_file_accessRelease` Android build and desktop `main` build
  passed. The final APK was installed without clearing app data.
- Seven focused CTests passed: checksum, skin snapshotter, skin archive importer,
  Android folder picker lifecycle, folder transfer, skin directory import and
  DocumentsProvider mutability. The transfer suite includes 6 worker, 26 folder
  transfer and 4 pause/cancel-control cases.
- Single-CPU Java runs passed for both copy engines. Android NDK checksum vectors
  and the optional 256 MiB probe passed with the actual OpenSSL backend.
- On Android 13, instrumentation passed for parallel provider cancellation, skin
  directory bounds/ownership/cleanup, DocumentsProvider Copy/Move and mutation
  reservations, foreground native library refresh, and the complete copy
  benchmark. Review findings about shared cancellation, error classification and
  single-core tests were fixed and re-reviewed.
- The test fixture and native probe executables were removed from the phone.
  No iOS build or Firebase upload was performed.
