# Large-library storage management implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task if native execution is selected; use superpowers:subagent-driven-development only if the user selects that method. Steps use checkbox syntax for tracking.

**Goal:** Make scanning and compression practical for libraries of at least
10,000 folders and ten million assets without library-sized asset inventories or
per-file checks of unrelated folders.

**Architecture:** Parallelize size-only directory walking and visit each supported
archive index once for all its groups. Prepare detailed fingerprints only for
selected local roots, reusing measurements and refreshing changed groups only.
Load only active directory working sets during conversion,
then add a bounded audio pool across independent directories while retaining
existing per-file media verification and recovery.

**Tech Stack:** C++23, filesystem/platform metadata APIs, existing SHA-256 and
SQLite libraries, libsndfile, FFmpeg, existing CMake/Xcode/Gradle builds.

**Spec:** `docs/superpowers/specs/2026-10-18-storage-management-scale-design.md`

## Global constraints

- Work in the provided current checkout and branch; do not create a worktree.
- Commit and push verified task-related changes; do not run a distribution helper
  in upload mode or change the user's installed chart library.
- Preserve recursive union selection, group identity, sandbox access roots,
  no-follow/no-clobber behavior, source permissions and hard-link size accounting.
- Preserve sample-exact FLAC validation and full candidate decoding. Do not
  reduce per-file journal durability or claim cross-process filesystem locking.
- Initial audio concurrency caps are four on desktop and two on mobile; choose
  a lower default if representative measurements do not support the higher cap.
- Keep one active video conversion with at most two encoder threads, and one
  application-wide storage job reservation shared with automatic downloads.
- Archive measurements cover existing non-solid chart entities only, in logical
  uncompressed bytes. Archive rewriting is outside this plan.
- Never restart all-library measurement for selection, policy changes, Apply or
  job completion. Preserve unrelated measurements and refresh affected roots only.
- Scan caps start at eight desktop/four mobile, bounded by hardware with fallback
  two, and are tuned separately from encoder caps. Keep at most `2 * workers`
  submitted scan jobs; spill pending paths at 4,096 records or 8 MiB.
- Publish bounded UI updates at most ten times per second. Detailed final reports
  retain at most 100 file results; aggregate counters include omitted details.
- Use existing `cmake-build-debug`, one build at a time, `-j 6`, and affected
  subsystem tests. Large scale benchmarks are opt-in, outside ordinary CTest.

## Review focus

1. Duplicate/nested selection roots and prefix siblings must preserve per-group
   totals without duplicate work or per-file loops over every selected group.
2. A directory with very many entries must spill bounded metadata privately;
   disk-full, cancellation or restart must not leave authoritative stale state.
3. A same-stem alternative, replaced ancestor, or changed chart arriving during
   encoding must retain the original and report stale/collision state.
4. Cancel/recovery failure with multiple workers must stop admission and settle
   active transactions without stale UI delivery or lost recovery records.
5. An archive with thousands of folders, a skewed local tree or a subset action
   must not multiply index scans, deadlock backpressure or rescan unrelated roots.

## File boundaries

- `StoragePathIndex.{h,cpp}`: component-aware registered/selected root lookup.
- `StorageDirectoryInventory.{h,cpp}`: cancellable metadata enumeration, one
  directory's ordered entries/collisions/fingerprint, and temporary spill storage.
- `StorageScanScheduler.{h,cpp}`: rolling admission over the existing chart scan
  pool, bounded discovery batches/frontier, cancellation and directory completion.
- `StorageMeasurement.{h,cpp}` and `StorageArchiveMeasurement.{h,cpp}`: size-only
  local reduction, one archive-index pass and per-source measurement provenance.
- `ArchiveFile.{h,cpp}`: borrowed/streamed index visitor with shared cache validation.
- `ChartResourceInventory.{h,cpp}`: selected compact preflight;
  retain the existing small-fixture API only as a temporary migration adapter.
- `StorageInventoryTask.{h,cpp}` and `StorageManagementScene.{h,cpp}`: incremental
  measurement delivery, selected review and ownership transfer.
- `StorageDirectoryEvidence.{h,cpp}`: active-directory validation and advancement
  from exact transaction changes, extracted from `StorageOptimizer.cpp`.
- `StorageWorkScheduler.{h,cpp}`: bounded directory admission, shared disk/video
  budgets, cancellation and result/progress aggregation.
- Existing encoder/transaction implementations retain responsibility for file
  content correctness and durable publication.

### Task 1: Index roots and enumerate metadata without repeated ancestor walks

**Files:** Create `src/storage/StoragePathIndex.{h,cpp}` and
`src/storage/StorageDirectoryInventory.{h,cpp}`; modify
`src/storage/ChartResourceInventory.{h,cpp}`, `cmake/StorageManagementTests.cmake`,
`cmake/TestGroupCatalog.cmake`; extend `tests/storage_inventory_tests.cpp`.

**Interfaces:** `StoragePathIndex(std::vector<std::filesystem::path>)` exposes
`Ancestors(const path&) -> std::vector<std::size_t>` and
`MinimalRoots() -> std::vector<path>` using indices from the input vector.
`StorageDirectoryEntry` carries relative name, entry kind, logical bytes and
optional safe `FileIdentity`. `StorageVisitMode::{SizeOnly, Preflight}` controls
whether mutation identity metadata is requested. `StorageDirectoryStamp` contains
device/file ID, modification time and permissions. `VisitStorageDirectory(path,
accessRoot, StorageVisitMode, Checkpoint, EntryVisitor, StorageDirectoryStamp&,
StorageScanMetrics*, std::string&)
-> bool` visits each entry once;
`EntryVisitor` is `std::function<bool(const StorageDirectoryEntry&)>`.
`StorageScanMetrics` counts entries, metadata reads and root-index steps; callers
may pass null outside tests/benchmarks. `StorageScanDependencies::enumerate` is a
callable with the visitor function's signature, defaulting to that production
function. Root/directory metadata validation belongs to this seam so synthetic
tests can feed metadata into the real reducer without creating filesystem nodes.

- [x] Add failing cases `rootIndexUsesComponentsAndUnions` and
  `metadataVisitIsIndependentOfUnrelatedRoots`. Assert exact memberships for
  duplicates, nested roots and `pack`/`package`, one logical metadata visit per
  regular entry, and no per-entry loop over 10,000 unrelated roots.
- [x] Build/run `storage_inventory_tests` and record the failing assertions.
- [x] Implement the prefix index and directory visitor. Authorize/open each
  directory boundary once, inspect children without following links, and obtain
  identity/size from the same metadata result where the platform supports it.
  Preserve the existing `FileIdentity` timestamp representation. Expose the
  visitor seam for synthetic benchmark entries without a fake aggregation path.
- [x] Run inventory and file-transaction tests, including sandbox-root and link
  cases. Confirm group lookup work depends on path depth, not library group count.
- [x] Commit and push `perf: index storage roots and reuse directory metadata`.

### Task 2: Schedule parallel directory discovery with bounded backpressure

**Files:** Create `src/storage/StorageScanScheduler.{h,cpp}` and
`tests/storage_scan_scheduler_tests.cpp`; use `src/ChartScanWorkScheduler.{h,cpp}`;
modify `cmake/StorageManagementTests.cmake`, `cmake/TestGroupCatalog.cmake` and
`src/storage/StoragePlatform.{h,cpp}` for private expendable frontier files.

**Interfaces:** `StorageScanLimits { workers, archiveBuilders, maxSubmitted,
frontierRecords=4096, frontierBytes=8*1024*1024 }`;
`DefaultStorageScanLimits() -> StorageScanLimits` caps workers at eight desktop,
four mobile, hardware count with fallback two, `maxSubmitted=2*workers`, and
`archiveBuilders=max(1, workers/2)`, subject to Task-8 measurements.
`StorageScanJob` contains directory/archive kind, source path, access root,
parent ID and group-index context. `StorageScanBatch` contains discovered child
jobs and aggregate bytes/categories/status; no ordinary file records.
`ScanBatchSink` is `std::function<bool(StorageScanBatch)>`.
`ScanJobWork` is `std::function<void(const StorageScanJob&, Checkpoint,
ScanBatchSink)>`. `RunStorageScan(std::span<const StorageScanJob>,
StorageScanLimits, ScanJobWork, ScanBatchSink, Checkpoint) -> bool` owns the
coordinator and existing shared worker pool. The returned bool means completed;
individual unavailable-source diagnostics travel in batches.

- [x] Add failing `independentDirectoriesOverlap`, `skewedRootsShareWorkers`,
  `wideTreeBoundsFrontier`, `saturatedCancellationWakesAllWaiters` and
  `archiveJobsShareOnePool`. Use barriers and counters, asserting one iterator
  owner per directory, at most `2*workers` submitted jobs, no per-file tasks,
  eventual progress for both local/archive jobs and bounded frontier memory.
- [x] Build/run `storage_scan_scheduler_tests` to record the expected failure.
- [x] Implement a single coordinator with rolling admission into
  `chart_scan::WorkScheduler`. Workers send batches of at most 128 child jobs or
  256 KiB, allowing one oversized path record only when the batch is empty;
  retain at most two batches per worker. The coordinator continuously drains
  batches while admitting jobs, never joining while producers need it to drain.
  Spill excess pending paths into private SQLite with an 8 MiB page cache.
  Reuse archive lane admission; feed local/archive pending jobs fairly. Do not
  change normal chart-scan scheduling defaults or add a nested worker pool.
- [x] Roll up parent completion only after discovery and all children finish;
  use directory-local counters and batched coordinator reduction. Wake blocked
  sinks on cancellation/failure, settle active jobs before cleanup, and reject
  retired-generation batches. Preserve unknown private files on cleanup.
- [x] Run scheduler tests plus `chart_scan_work_scheduler_tests` and
  `chart_scan_startup_failure_tests`; include spill failure and cancellation with
  one worker to catch producer/consumer deadlocks without timing assertions.
- [x] Commit and push `perf: schedule bounded parallel storage scans`.

### Task 3: Measure local folders and archive groups without mutation inventories

**Files:** Create `src/storage/StorageMeasurement.{h,cpp}`,
`src/storage/StorageArchiveMeasurement.{h,cpp}` and
`tests/storage_measurement_tests.cpp`; modify `src/ArchiveFile.{h,cpp}`,
`tests/archive_file_concurrency_tests.cpp`, inventory measurement types and
test build/catalog registration. Preserve `ChartLibraryScanner` solid handling.

**Interfaces:** Add `StorageSourceKind::{Local, Archive, Unsupported}` independently
of mutation eligibility in `FolderSelection`. `StorageMeasurements` carries
groups, compact directory rollups/source provenance and complete/cancelled flags.
`MeasurementCallback` is
`std::function<void(const std::vector<GroupStorageMeasurement>&)>`.
`MeasureStorageGroups(const InventoryRequest&, Checkpoint, MeasurementCallback,
StorageScanMetrics* = nullptr, StorageScanDependencies = {}) -> StorageMeasurements`
uses Tasks 1/2 in `SizeOnly` mode. In `archive_file`, add
`visitIndexEntries(const path&, const std::function<bool(const Entry&)>&,
std::string*, PauseCallback) -> bool`; callback values are borrowed and valid only
during that call. It returns success only after full cache/source validation.
`MeasureArchiveGroups(const path& archive, std::span<const FolderSelection>,
Checkpoint, StorageScanMetrics*) -> std::vector<GroupStorageMeasurement>` privately
reduces entries and publishes totals only on successful complete validation.

- [x] Add failing `sizeOnlyDoesNotBuildPreflight`, `hardLinksCountLogicalBytes`,
  `thousandArchiveGroupsVisitIndexOnce`, `archiveRootsAreSourceScoped`,
  `solidArchivesStayExcluded`, `invalidCachePublishesNoTotals` and
  `scanDoesNotAccumulateResidentArchiveIndexes`. Include implicit directories,
  duplicate/nested/prefix-sibling roots, truncated footers, source changes,
  encrypted entries, overflow and cancellation. Assert zero media reads/extracts
  for a valid cached index and no asset candidate/identity maps in browsing.
- [x] Run measurement and archive tests to establish failure before changes.
- [x] Implement local size aggregation using worker-local directory totals and
  parent rollups. Keep no file-level scan state and mark inaccessible scopes
  unavailable. Respect existing no-follow boundaries and logical hard-link rules.
- [x] Refactor the existing persisted index record parser for callback streaming;
  retain current format validation and source identity checks, including footer.
  Borrow a fresh resident immutable index without vector copies. Reuse existing
  single-flight building once for a stale/missing index; release storage-created
  retention without evicting a preexisting/replaced resident cache entry.
  Stream each archive once into an inner-root prefix reducer, with no
  `listEntries`/`entryRangeForFolder` call per virtual folder. Preserve current
  non-solid discovery semantics and propagate unsupported-source diagnostics.
- [x] Run measurement, archive concurrency/source-access/index-coordinator and
  chart-library scanner tests. Verify no archive writer or new solid support was
  introduced. Make archived rows explicitly describe uncompressed logical bytes.
- [x] Commit and push `perf: measure storage with size walks and archive indexes`.

### Task 4: Stream summaries and create compact, bounded preflight evidence

**Files:** Extend the directory inventory and chart resource inventory files;
modify `src/storage/StoragePlatform.{h,cpp}` for private expendable scan paths;
extend `tests/storage_inventory_tests.cpp` and create
`tests/storage_directory_inventory_tests.cpp` with catalog/build registration.

**Interfaces:** Add `StorageDirectorySummary` with directory identity, child
directory names, SHA-256 fingerprint, logical/category totals, eligible
count/bytes and bounded reason counters. Add `StoragePreflight` with the request,
group totals, directory summaries, total eligible count/bytes and completeness.
`BuildStoragePreflight(const InventoryRequest&, Checkpoint,
StorageScanMetrics* = nullptr, StorageScanDependencies = {}) -> StoragePreflight`
accepts selected local roots only; browsing never calls it.
Extend `StorageScanBatch` with an optional compact `StorageDirectorySummary`
result so Task-2 coordination also delivers completed preflight directories.
`LoadStorageDirectory(const StorageDirectorySummary&, const InventoryRequest&,
Checkpoint, std::string&) -> std::unique_ptr<StorageDirectoryInventory>` returns
an iterable working set only when its fingerprint matches.

- [x] Add failing `summariesDoNotRetainAllAssets`, `fingerprintsRejectChangedEntries`
  and `largeDirectorySpillRetainsCollisionRules`. Assert precise hard-link totals,
  bounded skip samples, deterministic fingerprints independent of enumeration
  order, case-varied collisions, and rejection of replaced directory identities.
- [x] Run the new focused tests to establish failure before implementation.
- [x] Use Task 2 to scan only selected local roots in `Preflight` mode; aggregate
  per directory and roll up indexed ancestors at directory/group boundaries.
  Do not build all-file maps or global candidate vectors. Fingerprint ordered
  relative names, entry kinds and identity metadata
  using the existing SHA-256 implementation. Do not hash media contents here.
- [x] Keep at most 4,096 entry records or 8 MiB of name/record payload in a
  working set; cross either threshold by spilling that directory into a private
  temporary SQLite index, with an 8 MiB page-cache budget and ordered iteration.
  The spill is expendable and never a source of recovery authorization. Check
  cancellation between entries; retain unavailable totals on traversal failure.
- [x] Test cancellation, disk-full injection, unknown private files, overlapping
  groups, and spill cleanup/restart. Keep the whole-library retained state to
  groups/directories/summaries, without candidate vectors.
- [x] Commit and push `perf: stream compact storage preflight summaries`.

### Task 5: Publish folder sizes progressively and review selected work promptly

**Files:** Modify `src/storage/StorageInventoryTask.{h,cpp}`,
`src/scene/StorageManagementScene.{h,cpp}` and relevant localization only if new
user-visible wording is needed; extend `tests/storage_inventory_task_tests.cpp`
and the existing scene extraction tests. Inspect/update
`src/repositories/MusicPlaylistRepository.{h,cpp}`, `ChartRepository.{h,cpp}` and
`tests/music_playlist_repository_tests.cpp` for requested DB efficiency.

**Interfaces:** Replace `Start` with `StartMeasurements(InventoryRequest)` and
`StartPreflight(InventoryRequest)`. `TakeMeasurements() ->
std::vector<GroupStorageMeasurement>` drains size updates. `TakeResult() ->
std::optional<StorageInventoryTaskResult>` returns mode, complete/cancelled flags
and optional moved `StoragePreflight`. Builders use Task 3 for sizes and Task 4
for selected evidence. The scene stores measurements independently of preflight.
Add `GroupStorageDelta { groupId, bytesBefore, bytesAfter, needsRefresh }` to
`StorageReport` and a bounded-by-group-count accumulator, separate from file
diagnostics; byte fields are `std::uint64_t`, the ID is `std::string`, and
`needsRefresh` is bool. Task 6 produces the deltas and this task consumes them.
Keep the existing inventory adapter for Apply until Task 6 migrates the optimizer,
so this intermediate task still builds.

- [x] Add failing `measurementsArriveBeforeScanCompletion`,
  `selectedReviewCancelsUnrelatedScan`, `replacedScanCannotPublishOldRows`,
  `subsetApplyNeverStartsFullMeasurement` and `jobRefreshPreservesUnrelatedSizes`.
  Use latches/controlled clocks, not fixed sleeps, to assert early visible totals,
  cancellation and no stale generation delivery.
- [x] Run `storage_inventory_task_tests` and the scene fixture to confirm failure.
- [x] Publish coalesced per-group updates at most every 100 ms, plus completion.
  Bound the queue by group count by replacing older updates for the same group.
  Enable selected Review during browsing; replace/reprioritize that scan rather
  than waiting for every unrelated folder. Preserve unavailable/pending wording,
  selected groups, expanded rows and scroll position while batches arrive.
- [x] Separate group-list reload, measurement invalidation and explicit full
  Refresh. Keep completed sizes on Review; policy/selection changes invalidate
  only selected preflight. Move preflight into Apply. Update sizes using all
  committed group deltas and remeasure only changed/uncertain roots, including
  affected nested/ancestor groups through the root index. Do not call
  `ReloadGroups()`/all-library `Measure` merely because a storage job completed.
  Reconcile external library revisions by changed group/source identities.
  Apply arithmetic deltas only to measurements whose source/generation baseline
  remains valid; otherwise refresh the affected scope and mark its total pending.
- [x] Inspect SQL query plans and existing statement reuse. Add a batched group
  lookup for expanded/affected rows where current code performs N+1 queries; use
  bound reusable statements and indexed group keys. Keep SQLite use on repository
  owners, with no per-file scan SQL and no shared mutable statement across workers.
  Extend repository tests with query/VM-step counts and group identity fixtures;
  batch affected-root invalidation writes within existing transaction boundaries
  in Task 6. Record measured query plans and timings in Task 8.
- [x] Verify progressive browsing, selected preflight and cancellation with the
  focused task/scene tests, then compile `main`.
- [x] Commit and push `perf: show storage measurements incrementally`.

### Task 6: Convert with active-directory evidence and no whole-library copies

**Files:** Create `src/storage/StorageDirectoryEvidence.{h,cpp}`; modify
`StorageOptimizer.{h,cpp}`, `StorageTask.{h,cpp}`,
`StorageApplicationIntegration.cpp`, `src/bms_search/DownloadStorageOptimization.cpp`
and `tests/storage_optimizer_tests.cpp`/`tests/storage_task_tests.cpp`.

**Interfaces:** `OptimizationRequest::preflight` becomes
`std::optional<StoragePreflight>`. `StorageOptimizer::Run(OptimizationRequest,
Checkpoint = {}, ProgressCallback = {}) -> StorageReport` consumes the request;
the task's run callback consumes it by value and the worker moves it.
`StorageDirectoryEvidence` owns one Task-4 working set and exposes
`Candidate`, `BeforeCommit`, and `Advance` with the existing evidence arguments.
Transactions still receive `StorageCommitPrecondition` unchanged.

- [x] Add failing `unrelatedFoldersDoNotMultiplyCommitChecks`,
  `changedDirectoryIsSkippedWithoutAcceptingNewAssets` and
  `preflightOwnershipMovesWithoutAssetCopies`. Retain tests for edits during
  encoding, case-varied alternatives, symlinked ancestors and chart changes.
- [x] Run optimizer/task tests to establish the regressions.
- [x] Scan compact preflight when absent; verify directory membership/fingerprints
  when loading each working set. Process one directory at a time initially.
  Select relevant authorized/access boundaries through Task 1 so transaction
  checks also avoid repeatedly searching all registrations. Preserve source hash,
  space admission, codecs, per-file reports, union refresh roots and download
  staging behavior. A stale directory stays skipped; do not rebuild its baseline.
  Populate Task-5 group deltas for every committed change; apply each physical
  change once per affected group, with no dependence on capped detail reports.
  Existing selected preflight must bypass any new global preflight build.
- [x] Extract local evidence and remove loops over every selected chart/directory
  from per-file paths. Keep local same-stem and external-change checks intact;
  instrument their enumeration costs before consolidating redundant work.
  Remove the full-inventory migration adapter once production callers are moved.
- [x] Run inventory/optimizer/task/file-transaction tests and compile `main`.
- [x] Commit and push `perf: bound storage conversion evidence by directory`.

### Task 7: Bound audio concurrency, scratch reservations and failure propagation

**Files:** Create `src/storage/StorageWorkScheduler.{h,cpp}`; modify
`StorageOptimizer.{h,cpp}`, `StorageTypes.h`, `StorageFileTransaction.cpp` cleanup
recognition as needed, and `tests/storage_optimizer_tests.cpp`; create
`tests/storage_work_scheduler_tests.cpp` and register it in `area_library`/`area_audio`.

**Interfaces:** `StorageExecutionLimits` contains `audioWorkers`, `videoWorkers=1`
and `videoThreads=2`; `DefaultStorageExecutionLimits()` caps audio at four desktop
or two mobile and available processors. Add limits to `OptimizerDependencies` so
deterministic existing tests can explicitly use one worker.
`StorageWorkBudget::Acquire(MediaKind, std::uint64_t temporaryBytes, Checkpoint)
-> std::optional<Lease>` returns an RAII reservation for codec slots and scratch.
`DirectoryWork` is `std::function<StorageReport(const StorageDirectorySummary&,
StorageWorkBudget&, Checkpoint, ProgressCallback)>`.
`RunStorageDirectories(const StoragePreflight&, StorageExecutionLimits,
std::function<std::optional<std::uint64_t>()> availableScratchBytes, DirectoryWork,
Checkpoint, ProgressCallback) -> StorageReport` coordinates the pool. Workers
reserve estimated temporary bytes (source size plus existing 1 MiB reserve)
through the shared budget before encoder admission.

- [ ] Add failing `independentDirectoriesOverlapWithinBudget`,
  `sameDirectoryAndVideoNeverOverlap`, `cancelStopsAdmissionAndDrainsTransactions`
  and `recoveryFailureStopsOtherAdmissions`. Use barriers and explicit fault points.
- [ ] Run scheduler tests to record failure.
- [ ] Keep the global storage-job lease outside the pool. Recover journals once
  before workers start; create a separate transaction/validator per active
  directory and unique job-owned output paths per worker. Serialize overlapping
  root/directory mutations and enforce one video slot without nested thread pools.
- [ ] Aggregate progress/counters and at most 100 details through one synchronized
  coordinator. Use monotonic atomic cancellation/failure state. Reserve scratch
  capacity across workers, recheck real free space, and release reservations on
  every outcome. Never release maintenance until active workers settle safely.
- [ ] Extend crash/restart tests to two independent journals and abandoned worker
  output paths. Keep unknown files and unresolved recovery state untouched.
  Run scheduler, optimizer, transaction and task tests together.
- [ ] Commit and push `perf: schedule bounded parallel audio storage work`.

### Task 8: Measure scale, choose defaults and complete platform verification

**Files:** Create `tests/storage_scale_benchmark.cpp`, an opt-in
`storage_scale_benchmark` target in `cmake/StorageManagementTests.cmake`, and
`docs/testing/storage-scale-2026-10-18.md`; update `docs/storage-management.md` and
this plan with results. Do not add the large benchmark to default CTest.

- [ ] Add small operation-count regressions to the owning tests for prefix lookup,
  bounded summary/result queues and unrelated-folder independence. Add an opt-in
  ten-million-entry synthetic mode through the real Task-1 visitor/Task-3 reducer
  and Task-2 scan scheduler; generate entries lazily rather than allocating ten
  million records. Assert queue/working-set bounds and report peak RSS.
- [ ] Build the benchmark's production sources with optimization and compare the
  baseline's fixed-file-count layouts plus real 1,000-/10,000-entry directories.
  Report enumeration/bookkeeping time, metadata/check counts and peak RSS.
  Compare one/two/four/eight scan workers, warm resident/persisted archive indexes
  and cold index builds separately. Include skewed trees, wide roots, many
  archives and one archive with thousands of groups. Measure shared-dispatch
  overhead before considering a more complex scheduler; choose actual defaults
  from throughput and cancellation latency, recording platform gaps.
- [ ] Benchmark actual short WAV/FLAC media with one/two/four audio workers and
  record separate encode, verification and durable commit timings. Reduce default
  concurrency when contention erases the benefit. Keep mobile at one if no native
  mobile measurement supports increasing it; document that limit explicitly.
- [ ] Investigate repeated local-directory enumeration if it dominates short
  audio; consolidate only checks whose removal preserves the same collision/edit
  regression assertions. Record residual costs rather than bypassing them.
- [ ] Build `main tests_app tests_library tests_archives tests_audio tests_video tests_settings
  tests_music_select` at `-j 6`; run CTest with
  `-L '^area_(app|library|archives|audio|video|settings|music_select)$' --output-on-failure
  --no-tests=error -j 6`. Run the iOS build-only helper with `--skip-init` and
  Android `--build-only`; verify native concurrent transaction/cancellation tests
  on disposable simulator/emulator storage where available.
- [ ] Update docs with measured results, platform limits and safe cancellation;
  do not extrapolate a small scan into an asserted ten-million-file elapsed time.
  Commit and push `test: verify storage scaling and conversion budgets`.

## Handoff

The written design is approved. This plan awaits user review and execution-method
selection. Native execution in this session is recommended because the inventory,
evidence and scheduler interfaces are tightly coupled. The native workflow ends
with one independent whole-branch review, followed by fixes and appropriate
verification. No additional worktree is needed or authorized.
