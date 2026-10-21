# Remaining large-archive read bottlenecks

Investigation against `d4b0e431`, following the archive audio preparation changes.
This is a diagnosis; production behavior is unchanged. Gameplay must continue to
hold all decoded audio in memory, with no new reading or decoding during play.

## Confirmed: single-file ZIP reads scan filenames despite a resident index

`ArchiveFile.cpp:readFile` calls `readZipEntriesByName` before consulting the
existing archive index. That helper opens the ZIP central directory, then
normalizes and searches filenames sequentially. Cost depends on entry position,
not just selected payload size. Callers include chart parsing, chart viewer
source reads, sprite loading, and Jukebox image reads. The normal batched archive
library scanner already supplies extracted bytes to its parser; do not attribute
this single-file cost to every library scan.

With a resident index and 100,003 ZIP entries, reading the last empty member via
`readFile` took **241.689 ms** median. Reading the same member through
`readFileBounded`, which uses indexed lookup, took **0.991 ms**. Both returned
zero bytes successfully. The gap isolates filename traversal from payload decode.

First optimization priority: use a validated resident index for single-file ZIP
reads. Preserve existing filename matching, source-change detection, CRC checks,
and a sensible cold-read path. Merely switching to an indexed reader still leaves
the repeated central-directory opening cost below.

## Confirmed: ZIP central directories are reread for independent reads/batches

`readZipEntryBounded`, `readZipEntriesByIndex`, the streaming reader without a
retained handle, and concurrent-reader preparation each initialize miniz from the
archive again. Application-level indexes do not retain that miniz directory state.

Two sparse ZIP64 fixtures had identical selected data and a 128 GiB unselected
member; only the unrelated empty-entry count changed. Warm operation medians
(milliseconds, ten measured reads after one warmup):

| Read path, 176,444-byte WAV | 1,003 entries | 100,003 entries |
| --- | ---: | ---: |
| `readFile`, early member | 0.504 | 1.503 |
| `readFileBounded` | 0.522 | 1.449 |
| Stateless bounded streaming | 0.552 | 1.550 |
| `BoundedArchiveReader`, reused | 0.510 | 0.520 |

A separate I/O-instrumented run counted source read bytes, including initial index
creation and all eleven WAV reads:

| Read path, 100,003 entries | Source bytes read |
| --- | ---: |
| `readFile` | 106,277,690 |
| `readFileBounded` | 103,394,744 |
| Stateless bounded streaming | 106,277,690 |
| Reused `BoundedArchiveReader` | 28,233,480 |

Selected WAV output totaled 1,940,884 bytes. All paths read **zero bytes** from the
unselected 128 GiB payload. These are logical source read counts, not physical
storage traffic; OS caching can hide much of the disk cost. Reusing a handle saved
78,044,210 bytes versus stateless streaming, consistent with avoiding ten repeated
~7.8 MB directory loads.

The existing retained reader already validates its cached index and resets its
handle on source changes. There are no uses outside its implementation in `src`
at this revision. Reusing appropriate worker-owned directory state during repeated
preparation batches is the second priority. Concurrent extraction needs its own
safe ownership design; a shared mutable reader must not serialize all workers.

## Confirmed scale cost: initial index construction and resident metadata

Fresh-process index creation (without persisted index caching enabled in this
probe) took about **5.3–5.8 ms** for 1,003 entries and **467–477 ms** for 100,003.
The filesystem cache was warm: these are not cold-storage measurements.
Process peak RSS was about **10 MB versus 36 MB**, including probe/runtime memory,
not a direct index allocation measurement.

`CachedIndex` owns the entry vector plus exact-name and lowercase hash maps.
Browsing indexes are intentionally retained without a global size bound. Large
multi-archive libraries can therefore accumulate metadata and create memory
pressure. This warrants allocation profiling across representative libraries
before changing retention. Archive metadata retention is separate from gameplay
PCM ownership; the latter must remain unchanged.

## Code-supported follow-up: solid archive replay

7z readers cache the archive handle but issue fresh `Extract` operations. Solid
members can require replay from a solid-block boundary. The code already logs
predicted replay bytes and batches selections; these mitigations do not establish
that successive extraction requests preserve decompressor state.

`ProgressiveMusicSource::prepareArchivedAssets` stages selections in successive
time windows and uses stateless bounded archive reads. Repeated requests into the
same solid block are a candidate for amplification, including when a window falls
back to a single asset. This concerns music preparation and is not a proposal to
stream gameplay audio. No representative large solid 7z/RAR timing or byte-count
measurement was made in this investigation, so its rank relative to ZIP costs is
unknown. Next check: log actual compressed bytes read and replay work per window
on a representative solid archive, then compare a single ordered extraction pass.

## Method and limits

- macOS desktop, optimized `-O2` native probe; archive implementation source hash
  matched the existing benchmark object. No production edits or app deployment.
- Fixtures created with `scripts/create_archive_audio_scale_fixture.py`, using
  `--entries 1000` and `--entries 100000`. Each has a correct-CRC sparse 128 GiB
  unselected member. No full payload scan, hash, or extraction was performed.
- Warmed the application index via `resolveArchiveEntriesWithExtensions`; timed
  eleven reads per mode and reported the median of the last ten. Every read
  checked success and expected output size. The late-entry comparison used
  `unrelated/099999.txt` through both public single-file APIs.
- Read counting ran separately from the reported uninstrumented timings, through
  `fread`, `read`, and `pread` interception filtered by source inode/device.
  No mapping fallback occurred in these paths.
- Probe source, build commands, raw timings, and byte counts are retained locally
  under `/tmp/asobm-archive-read-20261021`. Sparse ZIP fixtures were removed after
  measurement. No Android performance claim is made from desktop results.

Recommended order: indexed single-file ZIP access; retained directory state for
repeated preparation reads; measure solid replay and library-wide index memory.
