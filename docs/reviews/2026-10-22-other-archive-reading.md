# Shared reading for other archive formats

This extends the [shared ZIP/index improvements](2026-10-22-shared-archive-reading.md)
to the other existing backends. Changes are in `ArchiveFile`, below gameplay,
preview, library, media rendering and storage callers. Gameplay audio remains
fully resident; no gameplay decoding, PCM eviction or background refill was added.

## Format and API coverage

| Backend | Change | Boundary |
| --- | --- | --- |
| 7z / RAR5 SDK | Exclusive reusable directory readers for ordinary reads, RAR5 workers and compatible full-extraction preparation. Concurrent and nested consumers acquire separate readers instead of waiting on the same cursor. | Solid blocks still require their dependency decompression. Full-extraction readers with customized SDK limits are discarded afterward. |
| RAR4 / unarr | Bounded streaming uses one checked random-access reader for the entire ordered batch, instead of reopening per member; output is decoded in cancellable bounded chunks. | The public unarr API cannot suspend/reopen its stream. No private ABI dependency or idle file-descriptor pool was added. |
| Plain TAR | Index stores validated header offsets; selected reads start libarchive at those headers, preserving PAX/GNU extensions and sparse reconstruction. | Global PAX headers disable direct offsets because later entries can inherit their attributes. Those archives retain sequential parsing. |
| gzip / bzip2 TAR | Forward selections reuse an exclusive suspended decoder across single, bounded, vector, streaming and ranged requests. Backward requests reopen. | At most two idle cursors; only codecs with bounded small decoder state qualify. No decompressed member cache. |
| XZ / LZMA / Zstd TAR | The shared ordered batch reader handles the selection in one pass and enforces bounds while decoding. | Existing batching is preserved, but idle decoder retention is deliberately disabled: libarchive exposes no usable accounting for archive-selected dictionary memory. |

Single-file and `BoundedArchiveReader` operations use the same paths. Existing
concurrent APIs retain their supported-format boundary; callers' serial batch
fallback benefits from these changes. Metadata visitors and persisted indexes
remain shared with storage measurement and preflight. Storage mutation retains
its own physical validation, locking and crash-recovery protocol; this change
does not add mutation support for previously unsupported formats.

## Reader lifetime and validation

SDK retention allows four idle readers under a 64 MiB **estimated metadata** budget,
with one oversized reader allowed alone. It is not an RSS limit: active readers
and opaque SDK memory are not included. Idle SDK and sequential readers close
file descriptors and source-access leases. Reacquisition checks source identity;
replacement with equal size/mtime cannot reuse the old cursor. Failed and cancelled
readers are discarded. Checkpoint callbacks are cleared before idle retention.

TAR uses cache format version 8 and a distinct backend tag. Every selected offset
is reparsed by libarchive and checked against the indexed path and logical size.
Ordinary payload reads stop at the padded member boundary; sparse members keep
block-sized source reads. Global-header detection is conservative: a matching
extension-data block can disable the optional direct path, never enable it.
Compressed sequential readers validate order, name, size, limits and generation;
errors after selection are terminal, preventing a second fallback emission.

## Measurements

Paired optimized macOS probes substitute the previous commit's `ArchiveFile`
object (`3d2253dd`) into the same dependency closure. Fresh processes have no
persisted application index and warm filesystem caches. The TAR fixture has
100,002 entries, a sparse 128 GiB untouched member, and a four-byte late member.
Compressed fixtures have eight seeded 256 KiB members; each probe reads two rounds.
These are focused scale/work measurements, not whole-app loading guarantees.

Separate runs without I/O instrumentation measured the following totals. Each is
one paired run, not a percentile or a sustained mobile benchmark:

| Work | Before | After |
| --- | ---: | ---: |
| Plain TAR initial index | 117.4 ms | 155.8 ms |
| Ten late TAR reads after indexing | 800.5 ms | 11.8 ms |
| Sixteen forward gzip reads | 7.79 ms | 3.05 ms |
| Sixteen forward bzip2 reads | 926.9 ms | 165.3 ms |
| Two XZ batches | 7.77 ms | 8.06 ms |
| Two small RAR4 batches | 0.335 ms | 0.189 ms |
| Two RAR5 batches | 13.44 ms | 13.43 ms |
| Two solid 7z batches | 1.63 ms | 1.70 ms |

RAR5 and solid-7z batching were already efficient; these timings show no meaningful
serial-batch gain. Their new value is reader independence/reuse under concurrency
and reentrant callbacks. XZ batching also remains essentially unchanged.

Logical source reads include index construction and all selected reads:

| Work | Before | After |
| --- | ---: | ---: |
| Plain TAR index + ten late reads | 563,943,424 B | 51,212,288 B |
| Bytes from untouched 128 GiB TAR member | 715,264 B | **0 B** |
| gzip TAR index + sixteen forward single reads | 21,893,467 B | 6,295,899 B |
| bzip2 TAR index + sixteen forward single reads | 27,050,322 B | 6,321,150 B |
| XZ TAR index + two complete batches | 6,305,844 B | 6,305,844 B |

Read counters measure logical requests, not physical disk traffic. Only the plain
TAR fixture has the defined untouched-payload range; that counter is not applicable
to compressed fixtures. Byte totals and successful extraction were checked; the
behavioral tests also compare payload content. Plain TAR indexing intentionally
uses 512-byte metadata reads to avoid reading neighboring unselected payloads;
this costs more calls than the previous buffered scan, while selected reads avoid
walking earlier headers altogether.

Deterministic regression counters additionally verify:

- Nested 7z reads complete with two independent SDK readers, reused in the next round.
- Two RAR5 workers reuse the listing reader plus one additional reader across rounds.
- A bounded RAR4 selection opens its reader once per batch.
- Six increasing gzip/bzip2 requests use one decoder and six headers; XZ/Zstd use
  six decoders and 21 headers, explicitly verifying that large dictionary states
  are not retained.
- Six selected TAR API calls parse six headers total, including long PAX/GNU names.
- Global PAX falls back to walking preceding headers. Persisted offset corruption,
  sparse reconstruction, source replacement, cancellation and size limits are checked.

## Validation

- Desktop `main`, `tests_archives`, `tests_library` and `tests_audio` built.
- The 87 tests in the affected group union passed at `-j 6` in 20.28 seconds.
- The final TAR-only run also passed after adding explicit callback-cancellation,
  bounded-output and truncated-source checks.
- Android restricted-file release compiled with
  `scripts/android_firebase_deploy.sh --build-only` in 4m 57s.
- A native probe linked against that production Android library ran on Galaxy
  S20 FE: single and batch reads passed for TAR, gzip, bzip2, XZ, RAR4, RAR5 and
  solid 7z, with expected aggregate payload sizes. This checks native extraction,
  not UI/playback or paired Android performance. No APK installation or cloud
  upload was performed; owned device staging files were removed.
- Independent review identified global-PAX context and opaque decoder retention;
  both were fixed and the follow-up review found no further important issues.

Raw probes, link manifests and logs are retained locally in
`/tmp/asobm-other-archive-20261022`. Sparse fixtures are removed after measurement.
