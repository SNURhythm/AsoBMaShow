# Independent 7z block extraction

## Scope and scheduling

Full unzip now schedules independent 7z compression blocks across private SDK
handlers. Every selected file in a block stays with that block; a solid dictionary
is never split between workers or redundantly decoded for separate file jobs.
Blocks are assigned largest-first by expanded size, with a small per-file weight,
and each handler receives one sorted extraction request. Non-solid RAR continues
to use its existing file assignments through shared worker orchestration.

The concurrent route accepts ordinary single-coder LZMA and LZMA2 blocks only.
It validates live SDK paths, sizes, types, encryption/link/anti-item properties,
block membership, unique ordinals, and complete index coverage before starting
parallel extraction. Filesystem aliases already force serialization. One-block
archives, insufficient budgets, filtered indices, and unsupported coder chains
retain the existing route. Empty files do not create artificial parallel jobs.

The writer counts against the same worker budget and shares its existing bounded
queue between decoders. Each private data decoder uses one thread, with nested
SDK threading disabled. Its allowance is the larger of 64 MiB and twice the
largest data dictionary plus 16 MiB and estimated retained metadata. Metadata is
estimated at 512 bytes per indexed entry plus four times its native path length.
Queue memory is deducted before admitting workers. These are scheduling estimates,
not hard process-RSS limits or a new hard cap on SDK archive-header allocations.

Review identified that concurrent SDK `Open()` calls could multiply transient
compressed-header decoder workspaces before data-decoder configuration applies.
The final implementation prepares handles sequentially before starting any data
decoder, reusing the already-open preflight handle for worker zero. A compressed-
header fixture checks that every handle is prepared before extraction progress.
Follow-up review found no additional regressions in this preparation change.

Cancellation, integrity errors, byte quotas, and free-space failures are terminal
after the concurrent route starts: no batch fallback rewrites partial output.
Workers join and queued writes drain before returning. The original archive and
incomplete marker remain on failure; successful completion still requires all
files and output writes to finish. Cached library-reading handlers are unchanged.

## Measurement method

- Apple M1 Pro, eight logical CPUs, 16 GiB RAM, local APFS storage.
- Baseline production code is `cc856aaf`; both versions use the existing CMake
  Release production objects and dependencies, with an independently compiled
  baseline `ArchiveFile.cpp`. Benchmark assertions remain enabled.
- The harness and deterministic payload match
  [the RAR streaming measurements](2026-09-11-rar-streaming-extraction.md).
  Each archive expands to 69,206,016 bytes: eight 8 MiB files and 256 8 KiB files.
- New CPU-heavier fixtures use the full 8 MiB pseudorandom payload masked to
  64 byte values. Creation commands are `7zz a -t7z -m0=LZMA:d=1m -ms=16m
  -mmt=1 -mtm=off -mta=off -mtc=off pack0.7z payload` and the same command with
  `LZMA2`. Both have seven compression blocks and compressed headers.
- Single-solid LZMA1, internally chunked LZMA2, and non-solid RAR5 controls reuse
  the preceding report's repeating-payload fixtures.
- Timing runs are serialized with builds and tests. Each case has two seven-
  iteration processes per version, reversing baseline/new order in the second
  pair. Discarding the first iteration of each process leaves 12 warm samples.
  Observations overlapping a test run are discarded and their paired cases rerun.
- Every iteration removes prior outputs and verifies every extracted byte.
  The timer starts at `ArchiveUnzipOperation::RunAll` and ends at its first
  indexing notification: discovery, admission, setup, extraction, writer drain,
  and completion markers are included; removal, indexing, and verification are not.
- Automatic single-archive admission resolves to eight workers and 1 GiB on this
  machine. The new multi-block fixtures use seven data decoders and one writer.

## Results

| Single archive | Packed bytes | Baseline median | New median | Baseline/new |
| --- | ---: | ---: | ---: | ---: |
| Seven LZMA blocks, CPU-heavier payload | 51,540,542 | 2.350380 s | 0.730837 s | 3.22x |
| Seven LZMA2 blocks, CPU-heavier payload | 51,545,414 | 2.342850 s | 0.712095 s | 3.29x |
| Single solid LZMA1 control | 143,247 | 0.447019 s | 0.406353 s | 1.10x |
| Internally chunked LZMA2 control | 2,248,971 | 0.433723 s | 0.375198 s | 1.16x |
| Non-solid RAR5 control | 3,184,342 | 0.407224 s | 0.410425 s | 0.99x |

These are synthetic, local measurements, not promises for other archives or
devices. The controls have wide overlapping ranges; their lower 7z medians are
not evidence that this block scheduler speeds up the unchanged single-block
routes. The RAR median is about 1% slower. No general tail-latency improvement is
claimed. Largest-block size, codec chains, storage, and memory admission still
limit available parallelism.

Warm LZMA samples range from 2.306890–2.518250 seconds before and
0.653424–0.839666 seconds after; LZMA2 ranges from 2.302350–2.449230 seconds before
and 0.650304–0.920921 seconds after. Single-solid LZMA1 control ranges are
0.321708–0.633878 and 0.335474–0.542396 seconds, respectively.

Process CPU/RSS includes all seven iterations, removal, indexing, and byte
verification, unlike the extraction timer:

| Multi-block workload | Version | User CPU per process | System CPU per process | Peak RSS |
| --- | --- | --- | --- | --- |
| LZMA | Baseline | 15.73 / 15.74 s | 3.22 / 3.13 s | 30.8–33.8 MiB |
| LZMA | New | 17.42 / 17.18 s | 3.59 / 3.33 s | 73.0–75.7 MiB |
| LZMA2 | Baseline | 15.76 / 15.77 s | 3.22 / 3.34 s | 35.3–39.7 MiB |
| LZMA2 | New | 17.20 / 17.33 s | 3.41 / 3.23 s | 74.3–74.9 MiB |

Parallel decoding spends additional CPU and memory to reduce elapsed time.
A separate five-second `sample` capture of the final Release binary shows
`LzmaDec_DecodeReal2` beneath SDK extraction on multiple private-handler threads,
alongside writer and staging waits. This capture is excluded from timing samples;
sampling counts include waiting threads and are not CPU percentages.

## Regression coverage

Embedded fixtures cover LZMA/LZMA2 multi-block extraction, a single solid block,
unsupported filter chains, empty files/directories, compressed headers, and
3 MiB/64 MiB dictionary metadata. Tests check observed decoder threads, exact
bytes and file counts, monotonic aggregate progress, single-worker and constrained
memory operation, stop-token/pause cancellation, corrupt payloads, runtime byte
quotas, free-space failures, pending-write cleanup, retained originals, incomplete
markers, and absence of batch fallback after parallel failure.

## Validation

- `cmake --build cmake-build-debug --target main -j 6`: passed.
- `cmake --build cmake-build-debug -j 6`: passed.
- `ctest --test-dir cmake-build-debug --output-on-failure -j 6`: all 357 passed
  in 119.84 seconds.
- Release benchmark compilation, byte verification for every timing iteration,
  and a separate 30-iteration profiling run: passed.
- `scripts/ios_firebase_deploy.sh --build-only --skip-init`: build succeeded.
- `scripts/android_firebase_deploy.sh --build-only`: Firebase release build
  succeeded. Neither platform was deployed.
- Read-only review and targeted follow-up completed; the header-workspace
  finding is addressed by serialized preparation and its regression test.
