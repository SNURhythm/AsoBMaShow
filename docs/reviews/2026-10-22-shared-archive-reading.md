# Shared archive reading improvements

This follows the [large-archive diagnosis](2026-10-21-large-archive-read-bottlenecks.md).
The changes live in `ArchiveFile`, so callers share the improvements rather than
requiring gameplay-specific optimizations. Gameplay PCM ownership is unchanged:
all gameplay audio remains fully resident.

## Implementation and coverage

| Access path | Change or verified behavior |
| --- | --- |
| Single-file reads: chart parser/viewer, sprites, Jukebox images | Use the validated index instead of scanning ZIP filenames before lookup. |
| ZIP listing and index creation | Read central records sequentially without unused timestamp conversion; preserve ZIP64 sizes, Unicode extras and directory flags. |
| Single, bounded, vector, streaming and ranged ZIP reads | Reuse parsed miniz directory state through exclusive leases. |
| Concurrent preparation and full ZIP extraction | Reuse directory state during preparation/preflight and workers; each active reader has an independent cursor. |
| Library scanning, preview, rendering and music preparation | Benefit through their existing shared listing and batch APIs. |
| Storage measurement and preflight | Construct metadata without filename hash tables; persisted visitors remain streaming, and resident visitors borrow their index. |
| Storage Apply/compaction | Keep its independent positional reader, physical header checks, source identity checks, locking and crash-recovery protocol. These reads validate mutations and cannot safely be replaced by cached listing metadata. |
| 7z extraction | Existing selection batches use one extraction operation; full extraction groups solid blocks. No new full-archive decompression/cache. |

The reader pool retains at most four idle ZIP directories with a 64 MiB estimated
budget. Idle readers close their file descriptors and source leases; acquisition
reopens and checks source identity before and after initialization. The index
cache retains at most sixteen indexes with a separate 64 MiB estimated budget.
Both permit one oversized item alone to avoid repeatedly reparsing a very large
archive. Active leases can exceed these retention budgets: these are not process
RSS limits. Index eviction does not invalidate an active visitor or delete its
persisted index. Filename maps are built once, on first lookup, under `call_once`.

## Scale measurements

The paired macOS probe uses optimized archive code, the same dependency closure,
and a sparse ZIP64 with 100,003 entries and a 128 GiB unselected member. Each
operation runs in a fresh process with no persisted application index. Filesystem
cache is warm. Read medians cover ten operations after one warmup; index/listing
values are single runs and should be read as scale evidence rather than precise
latency guarantees.

| Operation | Before | After |
| --- | ---: | ---: |
| Metadata visitor, initial index | 491.4 ms | 58.8 ms |
| Listing | 480.0 ms | 47.4 ms |
| Last empty member, indexed archive | 243.54 ms | 0.087 ms |
| Repeated single WAV read | 1.519 ms | 0.563 ms |
| Repeated bounded WAV read | 1.504 ms | 0.550 ms |
| Stateless bounded streaming | 1.557 ms | 0.562 ms |
| Already-retained bounded reader | 0.531 ms | 0.531 ms |
| Metadata-visitor process peak RSS | 34.8 MB | 24.1 MB |

The selected WAV is 176,444 bytes. Separate instrumentation counted logical source
reads, including initial index construction and eleven WAV reads:

| Path | Before | After |
| --- | ---: | ---: |
| Single/stateless streaming | 106,277,690 B | 20,431,309 B |
| Bounded single | 103,394,744 B | 17,548,363 B |
| Already-retained reader | 28,233,480 B | 20,431,309 B |
| Metadata only | 15,604,261 B | 15,606,511 B |

Every mode read **zero bytes from the unselected 128 GiB payload**, and no mapping
fallback occurred. Metadata still reads the central directory through miniz and
its checked metadata view; its speedup comes chiefly from avoiding timestamp
conversion and unnecessary lookup-table construction, not skipping validation.
The 2,250-byte metadata difference is overlap at buffered record boundaries.
Payload-returning probes assert success and expected byte counts. Logical read
counts are not physical disk I/O. Peak RSS includes runtime memory; after a lookup
builds filename maps, process memory is similar to the baseline.

The actual `storage_scale_benchmark` uses the same optimized storage code and
libraries on both sides, substituting only the baseline `ArchiveFile` object:

| Storage operation | Before | After |
| --- | ---: | ---: |
| Fresh measurement, 100 groups / 100,000 entries | 521 ms | 78 ms |
| Process peak RSS for that measurement | 37.84 MiB | 28.78 MiB |
| Persisted-index review, 20 groups / 20,000 entries | 185 ms | 183 ms |

Each operation visited the archive once; metadata-read counters remained zero.
These counters describe storage entry-stat work, not underlying central-directory
I/O. The fixtures contain tiny media placeholders, so these measurements cover
metadata measurement/review, not media conversion. Persisted review was already
streaming and shows no meaningful latency change. Commands:

```sh
cmake-build-debug/storage_scale_benchmark archive 100 1000 6 cold
cmake-build-debug/storage_scale_benchmark archive-review 20 1000 6 persisted
```

The opt-in macOS benchmark target also needed its missing hardware-video source
and framework dependencies; it now participates in the existing platform-target
wiring.

## Solid archive constraints

A bounded 7z fixture contains a 1 MiB unselected prefix and eight selected 256 KiB
members in one solid block. Eight independent bounded requests read 18,877,237
source bytes and took 4.784 ms; the existing batch API read 3,277,772 bytes and took
0.766 ms, returning the same 2,097,152 selected bytes. This confirms existing batch
behavior; it is not a newly implemented speedup or a large-corpus benchmark.

Separate solid-block requests can still replay decompression. The library and
storage manager already exclude unsupported solid-chart/update operations.
Listening-mode preparation intentionally stages time windows for responsiveness;
merging all future windows would increase startup work and selected scratch
storage. This change does not do so or retain entire unselected solid payloads.

## Validation

- Desktop `main`, `tests_archives`, `tests_library`, `tests_audio`, and the opt-in
  storage benchmark built successfully.
- All 87 tests in the union of archive, library and audio groups passed (19.90 s
  at `-j 6`), including storage transaction, sparse archive and recovery coverage.
- Added regression coverage for reuse across public ZIP APIs, same-size/mtime
  source replacement, concurrent/reentrant readers, lazy lookup tables, bounded
  retention with active visitors, and ZIP64/Unicode/directory metadata.
- Storage measurement explicitly asserts that metadata visits build no filename
  lookup maps. Existing corruption, cancellation and resource-limit tests pass.
- Independent read-only review found no material issues.
- Signed Android restricted-file release compile passed via
  `scripts/android_firebase_deploy.sh --build-only` (4m 58s).
- On the connected Galaxy S20 FE, a native probe linked against the rebuilt
  production library read a real 44 MB ZIP at 48 kHz. Direct, prepared and early
  handoff modes all loaded 370 referenced IDs; prepared handoff opened no sounds
  and decoded no additional frames. Gameplay ownership assertions passed after
  releasing preview/cache ownership. This is a native functional check, not a
  paired Android timing claim, new APK installation, or UI/playback test. Owned
  phone inputs and binaries were removed; app/user data was unchanged.

Raw probes, compiler/link manifests and logs are kept locally in
`/tmp/asobm-archive-read-20261021`. Sparse fixtures are removed after measurement;
private source media remains outside the repository.
