# Storage scale and conversion budgets — 2026-10-18

## Method and limits

Measured on Apple M1 Pro, macOS 27.0.1, local APFS storage. The opt-in
`storage_scale_benchmark` compiles its own production sources at `-O2 -g0` in
the existing build directory. Dependencies are the existing developer build's
libraries (including Debug vcpkg codecs and SQLite); these are **not shipping
Release codec timings**. Fixture creation precedes timing; filesystem metadata
and file data are warm. Each row is a fresh process and a single run, not a
statistical confidence interval. No other build ran during the throughput matrix.
The additional active-evidence run measures memory, not a throughput comparison.

[Raw measurements](storage-scale-2026-10-18.csv) retain all 75 runs and counters.
Peak RSS includes fixture construction, libraries and the complete operation.
Worker seconds are summed across workers and overlap; subtracting them from
parallel wall time is invalid. Enumeration worker time includes the visitor's
reducer callback. At one worker, wall minus enumeration approximates the rest of
the coordinator/setup cost, rather than an isolated kernel-I/O measurement.

The synthetic producer lazily generates entries through the production
`StorageScanDependencies.enumerate` visitor interface, measurement reducer and
shared scan scheduler. It does not call the OS directory enumerator and retains
no ten-million-element fixture vector. Real-directory cases exercise
`VisitStorageDirectory`. Synthetic timing is **not** a prediction of scanning ten
million physical files. Synthetic metadata counters count generated records;
real metadata counters count actual reads.

```sh
cmake --build cmake-build-debug --target storage_scale_benchmark -j 6
cmake-build-debug/storage_scale_benchmark synthetic 10000 1000 4 wide
cmake-build-debug/storage_scale_benchmark synthetic 10000 1000 4 skewed
cmake-build-debug/storage_scale_benchmark dispatch 10000 0 4 wide
cmake-build-debug/storage_scale_benchmark cancel 10000 1000 4 wide
cmake-build-debug/storage_scale_benchmark real 100 1000 4 roots
cmake-build-debug/storage_scale_benchmark real 1 10000 1 roots
cmake-build-debug/storage_scale_benchmark archive 3000 4 4 persisted
cmake-build-debug/storage_scale_benchmark archives 100 100 4 cold
cmake-build-debug/storage_scale_benchmark audio 8 32 4 wav-flac
cmake-build-debug/storage_scale_benchmark audio 8 32 4 wav-vorbis
cmake-build-debug/storage_scale_benchmark audio 8 32 4 flac-vorbis
cmake-build-debug/storage_scale_benchmark evidence 4 10000 4 roots
```

Repeat scan commands with 1/2/4/8 workers, audio with 1/2/4. The harness is POSIX
only and excluded from both the default build and CTest. It asserts exact totals,
at most `2 × workers` admitted jobs/result batches, at most 4,096 in-memory
frontier records, directory-only summaries, and synthetic peak RSS below 512 MiB.

## Directory measurement

The previous baseline used the old full mutation inventory; the new path is the
browser's metadata-only measurement. Both count logical bytes of 17-byte media
fixtures, but the new path deliberately omits mutation evidence work. That API
separation is part of the improvement.

| Folders / files | Previous seconds | New 1 worker | New 2 | New 4 | New 8 | New peak MiB (8) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 100 / 10,000 | 0.721 | 0.0298 | 0.0198 | 0.0136 | 0.0121 | 9.2 |
| 1,000 / 10,000 | 4.038 | 0.0757 | 0.0485 | 0.0390 | 0.0300 | 11.1 |
| 2,000 / 10,000 | 8.032 | 0.1295 | 0.0790 | 0.0652 | 0.0587 | 13.0 |
| 100 / 100,000 | 7.683 | 0.2320 | 0.1600 | 0.1080 | 0.0905 | 9.2 |

Every real fixture made exactly one metadata read per file. At one worker,
enumeration/reduction took 0.0289/0.0670/0.1118/0.2310 seconds respectively;
remaining coordinator/setup time was about 0.001–0.018 seconds. A single directory
of 1,000 entries took 0.0021–0.0024 seconds, and 10,000 entries took
0.0215–0.0289 seconds. One directory is one serial scan job; increasing the worker
cap cannot parallelize that job.

| Synthetic case | 1 worker | 2 | 4 | 8 |
| --- | ---: | ---: | ---: | ---: |
| 10,000 directories × 1,000 files | 0.656 | 0.340 | 0.209 | 0.236 |
| 10 million files concentrated in one directory | 0.795 | 0.638 | 0.648 | 0.698 |
| 10,000 empty children: dispatch/reduction/frontier cost | 0.188 | 0.182 | 0.204 | 0.296 |

Wide and skewed cases each visited 10,010,000 records including child directories;
logical bytes were exactly 170,000,000. Peak RSS stayed near 27 MiB, frontier
records never exceeded 4,096, and excess jobs spilled to the disposable SQLite
frontier. Result/admission peaks stayed within 2/4/8/16. Completed summaries grow
with directories/groups, not media count. The empty-job measurement includes
prefix/group setup, SQLite spill and result reduction; it is an upper bound on
shared-dispatch overhead, not a pure queue microbenchmark. It does not justify
replacing the shared scheduler with a more complex dispatcher.

Cancellation requested at 100 ms returned at 101.8–101.9 ms across worker counts.
This measures cooperative cancellation during warm synthetic visits, not worst
case filesystem/codec latency. Four simultaneous active directories with 10,000
entries each (real preflight plus live/candidate/chart evidence stores) peaked at
28.6 MiB. Store limits apply separately to each live/candidate/ancestor index;
deep chart-bearing ancestor chains can require more memory than this flat case.

## Archives

“Cold” means no application index, **not** a flushed OS cache. “Persisted” primes
the streaming index without retaining a resident index; “resident” explicitly
loads the browsing index before timing. All cases avoid extraction and retain
only directory/group totals for measurement.

| Layout / cache | 1 worker | 2 | 4 | 8 |
| --- | ---: | ---: | ---: | ---: |
| One ZIP, 3,000 groups, 12,000 entries / cold | 0.125 | 0.165 | 0.184 | 0.213 |
| Same / persisted | 0.117 | 0.134 | 0.192 | 0.120 |
| Same / resident | 0.093 | 0.091 | 0.098 | 0.095 |
| 100 ZIPs × 100 entries / cold | 0.062 | 0.059 | 0.033 | 0.027 |
| Same / persisted | 0.039 | 0.039 | 0.029 | 0.035 |
| Same / resident | 0.0071 | 0.0074 | 0.0051 | 0.0047 |

One archive always made one index visit and visited exactly 12,000 entries, even
with 3,000 selected groups. It remains one job; the variation is not evidence of
parallel acceleration within an archive. Many-archive runs made exactly 100
visits/10,000 entry callbacks. Peak RSS was 20.1–22.3 MiB for the large archive and
10.8–13.2 MiB for many small archives.

## Actual audio and durable transactions

Fixtures are 256 half-second stereo 44.1-kHz, 16-bit WAV/FLAC files in eight
independent directories (32 per directory). Each file is actually encoded,
decoded for verification and installed through the durable transaction. FLAC
verification compares exact decoded samples. All 256 outputs were smaller and
committed. Times include preflight, which took less than 0.002 seconds.

| Conversion | Workers | Wall s | Encode worker s | Verify worker s | Commit worker s | Peak MiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| WAV → FLAC | 1 | 2.710 | 1.544 | 0.358 | 0.724 | 12.5 |
|  | 2 | 1.419 | 1.568 | 0.367 | 0.808 | 13.4 |
|  | 4 | 0.761 | 1.620 | 0.375 | 0.911 | 15.1 |
| WAV → Vorbis | 1 | 6.166 | 4.759 | 0.509 | 0.809 | 13.3 |
|  | 2 | 3.143 | 4.815 | 0.516 | 0.857 | 14.4 |
|  | 4 | 1.668 | 4.958 | 0.541 | 1.037 | 16.4 |
| FLAC → Vorbis | 1 | 6.377 | 5.010 | 0.564 | 0.728 | 14.1 |
|  | 2 | 3.309 | 5.128 | 0.572 | 0.825 | 15.5 |
|  | 4 | 1.682 | 5.141 | 0.577 | 0.867 | 18.2 |

Encode timing includes input/container validation and output finalization. Verify
is the codec's full decode-back phase. Commit includes copies, hashes, permission
preservation, membership preconditions, journal writes, renames and durability
flushes. The outer optimizer also hashes source data and advances evidence; these
costs are in wall time, outside those three counters.

The dense one-directory case converted 1,000 WAVs to FLAC in 13.211 seconds:
6.093 encode, 1.415 verify and 4.701 commit worker seconds; peak RSS 13.1 MiB.
It performed 3,000 membership enumerations and inspected 3,002,000 names.
Three checks protect different boundaries: prepared installation, final commit
after source retirement, and evidence advancement after cleanup. Removing a pass
could adopt a new resolver collision or external edit. They were retained; no
whole-library revalidation occurs per file. This remains **quadratic work within
a densely populated directory**, bounded in memory and independent of unrelated
folders. It is a residual conversion cost, not evidence of linear conversion
time at ten million assets. On these half-second fixtures it did not exceed the
codec cost; shorter assets or slower storage can change that balance.

## Defaults and verification

Desktop defaults retain up to eight scan workers (four archive builders) and
four audio workers, bounded by processor count. Eight scan workers improved the
real multi-directory and many-cold-archive cases, although four won the CPU-only
wide synthetic case. Audio improved 3.6–3.8× at four workers without disproportionate
RSS growth. A directory and its selected ancestors serialize mutation; siblings
can run concurrently. Video stays at one job with two FFmpeg threads. Shared
temporary-space reservations are released on every outcome and real free space
is checked again before admission.

Following the user's request after the benchmark, both mobile scanning and audio
conversion reuse chart audio loading's `parallel_worker_count()` policy instead
of separate one-worker defaults. It reserves one logical
processor at up to four processors, two at five through eight, and four above
eight; unknown hardware uses the same four-processor fallback as chart loading.
For example, six/eight processors allow four/six workers. Archive index builders
use half the scan-worker count (at least one), and admitted scan jobs/result
batches remain bounded by twice that count. Audio concurrency is bounded further
by the number of available independent directories and temporary-space budgets.
This shares an existing application policy; it is not a measured mobile optimum.
Simulator/emulator correctness is not mobile performance evidence. External/slow
volumes, Windows, physical mobile hardware and production Release codec builds
are unmeasured.

Focused regressions already cover depth-bounded prefix lookup with 10,000
unrelated roots, bounded queues/spill/cancellation, and conversion checks that do
not multiply with unrelated folders. Scoped repository regression uses 3,000
rows and checks zero `FULLSCAN_STEP` and fewer than 200 `VM_STEP` operations for
the first selected path query, including literal `%`/`_`, nested/duplicate roots
and exact-file scopes. Statements are reused per batch; this is an operation-count
guard, not a database throughput benchmark.

Final review also added regressions for a missing private scan directory on first
use, durability of newly created journal ancestors before source mutation,
secondary-chart/resource-only cache invalidation, and subset refreshes that must
not enumerate an ancestor's unrelated children. Journal creation syncs the full
chain through the previously existing ancestor on every transaction/recovery,
including retries after a failed directory sync. The final audio matrix was rerun after all application builds completed and
includes that barrier; scan/archive rows did not need rerunning.

Source-change history retains at most 4,096 paths; overflow or unscoped legacy
mutations conservatively invalidate all sizes. Scan batches publish their scopes
only after a committed transaction, including progress commits. Normal path
refreshes publish their scope even when no chart row changes. The storage job's
maintenance refresh avoids a duplicate resource-only invalidation, but real
chart/folder changes still invalidate affected rows. Stable cached ancestor
estimates can receive exact committed deltas; uncertain strict ancestors remain
unavailable until explicit Refresh, avoiding recursive sibling scans.

Native correctness checks after all fixes passed on iOS 27.1 Simulator and the
Android 10 emulator: scan scheduler, work scheduler and transaction suites (three
on each platform). This includes missing-spill-directory creation, platform
worker defaults, concurrent commit/cancel with a shared journal, complete
journal ancestor sync, and recovery after failure. The disposable Android
emulator was stopped afterwards. These results do not establish physical-device
throughput.

Final desktop `main` and all seven affected subsystem test targets built at
`-j 6`. The combined CTest selection passed **166/166** tests in 29.36 seconds.
The iOS helper (`--build-only --skip-init`) and Android helper (`--build-only`)
both passed after the final fixes; neither action distributed a build. An initial
concurrent desktop/Android build attempt caused memory pressure, so desktop was
interrupted and resumed successfully after Android; the remaining application
build and audio benchmarks ran sequentially. Existing third-party compiler and
duplicate-link-library warnings remain.

The subsequent user-requested mobile worker-policy update passed its updated
default-policy regressions on iOS Simulator (both failed against the old
one-worker defaults first), five focused desktop scheduler/measurement/optimizer
and shared-worker tests, and renewed desktop/iOS/Android application builds.
Desktop benchmark settings and measurements are unchanged by this mobile-only
policy update; physical-device speedup is still unmeasured.
