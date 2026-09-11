# Streaming non-solid RAR extraction

## Scope

This pass adds bounded streaming extraction for eligible non-solid RAR4/RAR5
archives. It does not implement 7z solid-block scheduling or dynamic redistribution
of worker budgets between archives.

Each private SDK handler receives a size-balanced assignment, sorted into SDK
index order, and makes one extraction call. That avoids repeatedly constructing
decoder dictionaries for small chunks. All decoders share one bounded output
pipeline; its staging is serialized between producers, and its writer counts
against the same worker budget. Actual write accounting remains serialized by
the existing guard. No complete expanded file is buffered by this route.

Preflight declines solid, multi-volume, encrypted, linked, aliased, or mismatched
entries before starting concurrent output. RAR4 is allowed 320 MiB per decoder
for its hidden PPM allocation and declines mixed unpack versions: the SDK can
retain separate decoders for different version numbers during one extraction.
RAR5 uses the larger of 64 MiB and twice its dictionary plus 16 MiB. Queue memory
is deducted before admitting decoders. These are scheduling allowances, not hard
process-memory caps; unsupported cases retain the existing backend behavior.

## Measurement method

- Machine: Apple M1 Pro, eight logical CPUs, 16 GiB RAM, local APFS storage.
- Baseline production code: `205b9250`. Both sides use the existing CMake Release
  `archive_unzip_operation_tests` production objects and Release dependencies;
  benchmark assertions remain enabled. A separate baseline `ArchiveFile.cpp`
  object is compiled from that commit when relinking the expanded harness.
- The test executables now explicitly link the RAR4/RAR5 handler and RAR codec
  registration objects. Earlier exploratory numbers that accidentally used
  libarchive are discarded. Fresh SDK index-cache directories are selected after
  repository initialization, and backend selection is verified in debug logs.
- The timer starts at `ArchiveUnzipOperation::RunAll` entry and ends at its first
  indexing notification. It includes discovery, admission, output-path preflight,
  decoder setup, extraction, writer drain, and completion-marker publication;
  it excludes output removal, subsequent indexing, and byte verification.
- Each case has two 11-iteration processes per version. The second pair reverses
  baseline/new process order. Discarding each process's first iteration leaves
  20 warm observations per version. Every iteration removes outputs beforehand
  and compares every extracted byte afterward. Native builds and timing runs are
  serialized; normal desktop services remain active.
- The default single-archive budget resolves to eight workers and 1 GiB here.
  Eligible RAR uses seven decoders plus one writer. Solid RAR retains its existing
  sequential decoder and writer route.

Every archive expands to 69,206,016 bytes: eight 8 MiB files and 256 8 KiB files.
The original payload repeats a deterministic 128 KiB pseudorandom block. The
CPU-heavier payload instead uses a full 8 MiB block with 64 possible byte values.
Both use the recurrence `state = state * 1664525 + 1013904223` modulo 2^32,
starting at one and taking its high byte; the CPU-heavier variant masks that byte
with 63. Small files use the first 8 KiB of the corresponding payload.

RAR fixtures use RAR 7.22 with `a -ma5 -m3 -md4m -ts-`, plus `-s-` or `-s`.
The existing solid LZMA1 fixture uses libarchive's writer; LZMA2 uses
`7zz a -t7z -m0=lzma2 -md=1m -mmt=8 -ms=on`.

## Results

| Single archive | Packed bytes | Baseline median | New median | Baseline/new |
| --- | ---: | ---: | ---: | ---: |
| Non-solid RAR5, repeating payload | 3,184,342 | 0.211503 s | 0.181343 s | 1.17x |
| Non-solid RAR5, CPU-heavier payload | 53,564,095 | 0.513623 s | 0.204573 s | 2.51x |
| Solid RAR5 control | 155,682 | 0.176536 s | 0.176411 s | 1.00x |
| Solid LZMA1 7z control | 143,247 | 0.192182 s | 0.201460 s | 0.95x |
| LZMA2 7z control | 2,248,971 | 0.177239 s | 0.194980 s | 0.91x |

These synthetic RAR5 results are not promises for all archives or devices.
The 7z control medians were approximately 5% and 10% slower in this run; no 7z
speedup is claimed. Their ranges overlap substantially, but that alone does not
prove equivalence or rule out a small regression. The shared pipeline now has an
additional uncontended staging lock in the single-producer case; this pass does
not attribute the measured control differences solely to noise or that lock.

Timing tails remain important: the CPU-heavier new case ranged from 0.177136 to
1.334020 seconds, versus 0.488383 to 0.713632 seconds for baseline. The median
improved substantially, but this is not a demonstrated tail-latency improvement.

Process CPU and peak RSS include all 11 iterations, output deletion, verification,
and final indexing, unlike the extraction-only timer:

| RAR5 workload | Version | User CPU per process | System CPU per process | Peak RSS |
| --- | --- | --- | --- | --- |
| Repeating | Baseline | 0.37 / 0.37 s | 2.40 / 2.62 s | 49.3–51.2 MiB |
| Repeating | New | 0.46 / 0.46 s | 3.45 / 3.38 s | 96.2–96.6 MiB |
| CPU-heavier | Baseline | 4.62 / 4.63 s | 2.66 / 2.55 s | 40.0–40.1 MiB |
| CPU-heavier | New | 5.05 / 5.02 s | 3.53 / 3.61 s | 87.4–89.7 MiB |

Parallel decoding spends more CPU and memory to reduce elapsed time. The solid
RAR control remains approximately 49 MiB RSS on both sides.

## Profiling and design revisions

Five-second `sample` captures of Release binaries show RAR5 `DecodeLZ2` beneath
the extraction call on the CPU-heavier fixture. The final parallel capture shows
decoder work on multiple private-handler threads, alongside writer work and
staging-lock waits. Removal and verification stacks are outside the extraction
timer and are not treated as extraction bottlenecks. Sample counts include
waiting threads, so they are not CPU percentages.

The first implementation wrote directly from every decoder and repeatedly
scheduled small chunks. On the repeating fixture its warm medians were around
0.27–0.30 seconds, slower than the roughly 0.19–0.21 second baseline. Sampling
showed repeated SDK header reads and contention on the actual-write guard.
Private RAR input read-ahead was reduced from 1 MiB to 64 KiB, assignments became
one extraction call per worker, and one shared bounded writer replaced direct
large-file writes. The reported results use that revised design; cached library
reader buffering and decoder settings are unchanged.

## Regression coverage and verification

Portable fixtures cover serial and solid controls, independent RAR5 workers,
RAR4 low-memory serialization, higher-budget RAR4 workers, mixed RAR4 versions,
large dictionaries, duplicate output names, and mismatched persisted paths.
Tests check monotonic progress, exact expanded-byte accounting, released pending
reservations, output contents, cancellation, CRC failure, runtime byte limits,
reserved free space, retained originals, and incomplete/completion markers.

The concurrency test was observed failing with the new route disabled and passing
when restored. Both RAR4 memory-review findings also received failing regression
tests before their fixes. Read-only reviews checked the streaming worker and
shared-writer lifetimes; the two reported RAR4 budgeting findings are addressed.

- Desktop `main` and all-target builds passed.
- Full parallel CTest passed all 357 tests, including `visual_catch_up`.
- Backend, operation, and modal targets passed three consecutive focused runs;
  the final RAR4 version regression also passed three consecutive backend runs.
- Unsigned iOS build passed with
  `scripts/ios_firebase_deploy.sh --build-only --skip-init`.
- Android `firebaseRelease` build passed with
  `scripts/android_firebase_deploy.sh --build-only`.
- After the final RAR4-only version safeguards, the Release target rebuilt and
  all five benchmark fixtures passed two additional byte-verified smoke runs.
  Those post-build checks are not added to the paired performance dataset.
- `git diff --check` passed. Verification performed no distribution archive or upload;
  completed work follows the user's standing commit-and-push preference.

Local measurement artifacts remain outside version control under
`cmake-build-debug/extraction-benchmark/`; final timing logs are
`/tmp/asobmashow-final-<case>-<before|after>-r<1|2>.log`, and final sample captures
are `/tmp/asobmashow-rar-final-<before|after>.sample.txt`.
