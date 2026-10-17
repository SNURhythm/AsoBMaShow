# Storage management at ten million files

Status: design approved by the user on 2026-10-18, incorporating subsequent
requirements for size-only walking, archive-index reuse, parallel scanning and
selected-folder operation without a full-library rescan. Implementation plan
pending review. Not implemented.

## Intent and constraints

The user requires efficient scanning and compression for real libraries with
10,000 or more folders and 1,000 or more audio assets per folder: at least ten
million assets. Opening the browser must remain responsive, memory must not grow
with every asset in the library, and work on one asset must not repeatedly walk
all other selected folders.

Keep the agreed media behavior, recursive folder selection, overlapping-root
union accounting, hard-link measurement without mutation, sandbox access roots,
sample-exact FLAC verification, full output validation, no-clobber publication,
source permissions, per-file recovery, and cancellation. Do not use a real user
library for destructive benchmarks. Speed does not justify dropping durable
recovery or output validation.

An initial exact measurement still requires enumerating the files. The product
must show progressive results and honest pending/unavailable states; it cannot
promise instant cold scans or a fixed encoding duration independent of media,
storage hardware, and codec settings.

## Current costs

- `BuildInventory` compares each file against every group when accumulating
  totals. Ten million files and ten thousand groups imply approximately one
  hundred billion containment checks, even with no overlapping folders.
- Root deduplication and unavailable-scope propagation also scan lists of roots.
- Browsing uses the full mutation inventory: paths are retained in `files`,
  `entries`, candidates and skip details. The current review fix adds a separate
  measurement map. These representations are inappropriate for ten million
  files and must not become the new measurement architecture.
- Apply copies the preflight inventory. `Evidence` builds another child-entry
  index and checks every selected chart and directory around each conversion.
- Commit checks enumerate the current asset's directory several times per file.
  This local cost must also be measured; removing library-wide work alone does
  not prove that folders with many small audio files are fast.
- The browser waits for the complete inventory before publishing row sizes and
  disables Review while the all-library scan is active.

These are findings from the production call paths, not measured ten-million-file
timings. Benchmarks must distinguish filesystem enumeration, bookkeeping,
encoding, decoded verification, and durable installation.

An optimized (`-O2`) baseline against the current production inventory, after
other builds finished, used disposable 17-byte `.wav` metadata fixtures on this
Mac. Fixture creation preceded timing, so metadata was warm; no encoding ran:

| Folders | Files | Inventory seconds | Process peak RSS MiB |
| ---: | ---: | ---: | ---: |
| 100 | 10,000 | 0.721 | 13.6 |
| 1,000 | 10,000 | 4.038 | 14.4 |
| 2,000 | 10,000 | 8.032 | 15.2 |
| 100 | 100,000 | 7.683 | 109.8 |

These single-run measurements demonstrate the group-count and retained-metadata
costs. They are not a cold-scan guarantee or a ten-million-file extrapolation.

## Options and recommendation

1. Add lookup indexes to the current inventory. This removes group cross-products
   but retains library-sized memory and preflight copies. Insufficient alone.
2. Stream folder summaries and process bounded folder working sets. Recommended:
   small library-wide metadata, responsive partial results, and reuse of the
   existing encoder and transaction implementations.
3. Persist a full asset catalog. This can help repeated analysis, but adds a large
   private database and difficult external-edit invalidation. Do not require a
   permanent ten-million-row cache for the initial correction.

## Proposed data flow

### Root indexing and measurements

Normalize and authorize selection roots once, build a component-aware prefix
index, and enumerate the selected physical union once. Carry the relevant group
membership down traversal rather than testing every group for every file. A
duplicate or nested group receives its own totals while the union counts a path
once. Avoid prefix-string comparisons that confuse `pack` with `package`.

Browsing is a size-only filesystem walk, comparable in purpose to `du` or
TreeSize. Retain totals and compact directory rollups, not asset candidates,
collision indexes, per-file results, fingerprints or full identity maps. Read
each regular entry's required metadata once, reuse it for size/classification,
and never decode media while measuring. Validate traversed directory boundaries
without repeating the entire ancestor walk for every child. Detailed mutation
preflight is a separate operation on selected local roots only.

### Parallel scan scheduling

Use one bounded shared worker pool with dynamic directory jobs, building on
`chart_scan::WorkScheduler`. Do not create one thread/task per asset or assign
an entire large root permanently to one worker. Discover subdirectories as
independent jobs so uneven trees keep available workers busy. One directory
enumeration owns its iterator; no concurrent reads of the same iterator.

A coordinator admits at most twice the worker count of queued/running jobs,
drains bounded discovery/result batches continuously, and rolls up directory
totals after their children finish. Keep pending directory paths within 4,096
records or 8 MiB; spill a larger frontier privately. Workers must never block
waiting to enqueue descendants onto their own exhausted pool. Publish aggregates
per directory/batch, with no shared lock or atomic increment per file. Wait on
condition variables rather than polling or busy spinning. Cancellation wakes
all producers/consumers and discards obsolete-generation results.

Local directory jobs and archive-index jobs share this worker budget. Limit cold
archive builders separately; borrow warm indexes without opening a nested pool.
Start scan tuning with desktop caps of eight and mobile caps of four, bounded
by available processors (fallback two), and compare one/two/four/eight workers
where supported. These are scan caps, separate from encoder limits. Select
defaults from measured metadata throughput, latency and peak memory; do not
claim a universally fastest scheduler or add lock-free/work-stealing machinery
without evidence that the existing shared dispatch is the bottleneck.

### Archive measurements

Measure archive entities already supported by chart discovery. Solid archives
remain excluded as chart groups, matching `ChartLibraryScanner` and its existing
solid-archive tests; do not infer solidity from a filename extension. Group all
virtual folders by archive source identity, then aggregate their logical,
uncompressed entry sizes in one index pass using an inner-path prefix index.
Thousands of folders in one archive must not cause thousands of full index
copies/scans, repeated extraction, or repeated indexing.

Borrow a fresh resident immutable index without copying its entries. For a
persisted index, stream validated records through the existing cache parser
without constructing lookup maps or filling the unbounded resident cache. A
missing/stale index uses the existing cancellable single-flight builder once per
archive; storage scans must not retain every newly built index indefinitely.
Cold-index construction remains format dependent and is measured separately.
Validate the entire cache, including its source-identity footer, before
publishing any accumulated result. Reject partial/truncated indexes, encrypted
or unsupported entries according to existing discovery rules, changed sources,
invalid entry paths and arithmetic overflow.

Label archived folder bytes as uncompressed logical size. They are neither
physical container size nor estimated reclaimable bytes. Do not add container
bytes to virtual-entry totals. Archive measurements are read-only: the earlier
archive-replacement feasibility discussion does not add an archive writer to
this implementation. Non-solid ZIP replacement could later batch all edits into
one rewrite, copying unchanged compressed entries, under a separate writer plan.

### Selection and incremental refresh

Publish completed group measurements in bounded batches at most ten times per
second. Keep title/size sorting and selection stable, and refresh only when a
published batch changes visible data. Selecting Review cancels or reprioritizes
the browsing scan and processes only the selected roots; unrelated pending
folders must not make the selection unusable. Preserve completed measurements
when switching modes. Selection or policy changes must not call the all-library
measurement path. Size-only results are estimates, never mutation authorization:
build detailed evidence only for the selection and transfer that preflight into
Apply instead of rebuilding it. Apply retains required live checks of its active
directory and transaction paths.

After conversion, update known totals from committed per-group byte deltas.
Refresh only changed/uncertain directories and their ancestor rollups; retain
unrelated row sizes. Reports must carry aggregate changes for every affected
group, independent of the 100-item diagnostic limit. An external library revision
updates added/removed/changed groups; a user's explicit Refresh can still request
a complete new size walk. Cache provenance includes source/root identity and
scan generation; stale measurements stay visibly stale until refreshed.

### Database access

Keep database work out of per-file scan/encode loops. Reuse bound prepared
statements within a repository session, batch related reads and writes, and
inspect query plans/index coverage for group selection and affected-root refresh.
Avoid per-group query loops when refreshing many expanded rows and avoid loading
all chart metadata for a subset operation. Retain transaction boundaries and
repository thread ownership; do not share a mutable SQLite statement across
worker threads. Use query/VM-step counts with representative group fixtures to
verify improvements, rather than adding caches without invalidation rules.

### Compact preflight and execution

Preflight stores selected policy/root identity, per-directory membership and
identity fingerprints, aggregate eligible counts/bytes and bounded skip samples.
Fingerprint construction covers all entries relevant to collision/freshness
decisions and file identity metadata, without reading chart declarations or
hashing audio contents. Use a deterministic ordering and the existing checksum
implementation, not a weak timestamp-only freshness check.

On execution, enumerate a directory into a working set and compare it with its
preflight summary before changing it. Process its eligible assets, release the
working set, and continue. A stale directory is skipped with an explicit reason;
unrelated directories can continue. Never silently accept newly added assets as
part of the earlier confirmation. Retain compact ancestor/subdirectory evidence
so replacing a directory or changing recursive membership is detected.

Memory scales with group/directory summaries and active working sets, not total
asset count. Apply transfers ownership of the compact preflight rather than
copying it. For an unusually large single directory, use a bounded temporary
spill/index in private storage instead of growing an unlimited in-memory vector.
The spill is expendable scan state, separate from durable mutation journals.

### Conversion scheduling and validation

Retain one application-wide storage job reservation. Within a manual audio job,
allow a small bounded pool across independent directories: an initial cap of
four workers on desktop and two on mobile, reduced for available processors and
temporary-space budget. Own a directory with one worker at a time and serialize
mutations of overlapping roots. Keep video conversion at one active video with
its existing two encoder threads; avoid multiplying FFmpeg thread pools. Download
jobs continue to obey foreground pause and the same shared resource budget.

Each worker retains source hashing, decoded output verification, strict smaller
output checks, permissions and the existing per-file durable transaction. Use
separate job-owned candidate paths; bound in-flight outputs and detailed reports.
Cancellation stops admission of new files and drains each active transaction to
a safe checkpoint. A recovery-required error stops admission for the whole job.

Scope freshness checks to the active directory, relevant charts, ancestors and
transaction paths. Never walk every library directory or chart for each file.
Preserve same-stem collision protection, including case variants and unsafe
entries. Begin with the current local directory checks; instrument their cost
and consolidate redundant checks within a transaction only where the same
external-change protection can be demonstrated. Do not infer that every
directory-timestamp change was caused by this process.

This proposes changing the original single-conversion limit for audio. Benchmark
one, two and four workers before choosing final defaults; retain a lower cap
where storage contention, mobile memory or verification cost erases the gain.

## Verification and acceptance

- Add small deterministic tests that count group matching, file metadata reads
  and unrelated-directory checks. Increasing unrelated selected groups must not
  multiply per-file work. Avoid wall-clock thresholds in ordinary tests.
- Assert actual overlap between independent directory jobs, balanced progress on
  skewed trees, bounded queue/frontier/batches, and cancellation under saturated
  backpressure. Compare scan worker counts independently of encoder counts.
- Verify one index visit for thousands of virtual folders in one archive,
  correct duplicate/nested inner-root rollups, no extraction, no retained cache
  growth across archives, solid exclusion and stale/truncated-cache rejection.
- Assert selecting/compressing a subset never enumerates unrelated folders or
  restarts the whole-library measurement; post-job refresh remains incremental.
- Exercise a ten-million-entry synthetic traversal through the real aggregation
  and scheduling code without creating ten million files. Confirm bounded queue,
  candidate and result counts and no eager inventory copies.
- Use disposable real filesystem fixtures for directories containing 1,000 and
  10,000 entries, mixed links, nested/duplicate roots, cancellation, inaccessible
  ancestors and changing chart/resource/collision entries.
- Provide an opt-in optimized-build benchmark outside the default test suite.
  Compare fixed file counts across different folder counts; separately report
  scan time, peak RSS, operation counts, encode/verify/commit time and throughput
  at each worker count. Report warm/cold-cache conditions and hardware honestly.
- Measure real short WAV-to-FLAC and WAV/FLAC-to-Vorbis jobs, retaining bit-exact
  and complete-decode verification. Check that concurrency improves elapsed time
  before increasing a platform's default.
- Keep the browser usable before the full library scan completes. Cancellation
  must be observed between entries/codec blocks, with no full-library join delay.
- Rerun affected app/library/archive/audio/video/settings/music-selector tests and
  desktop/iOS/Android build checks. Extend crash/fault coverage to concurrent
  independent transactions, using only disposable data.

No claimed ten-million-file performance result is accepted merely by multiplying
a small-fixture throughput number. Validate algorithmic bounds with synthetic
scale, and validate filesystem/codec costs with representative real fixtures.
