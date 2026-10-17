# Storage Management Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans
> (recommended for this tightly coupled feature) or, if the user chooses parallel
> agent execution, superpowers:subagent-driven-development to implement this plan
> task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Safely reduce installed and downloaded BMS media storage with grouped
selection, FLAC/Vorbis audio, H.264 BGA compression, BGA deletion, and optional caps.

**Architecture:** A synchronous, cancellable optimizer operates on a resource
inventory and delegates encoding and recoverable file changes to separate units.
The manual worker and Find BMS worker reuse that optimizer. UI grouping and row
presentation are extracted from the existing music player, with the repository's
existing grouping queries preserved.

**Tech Stack:** C++23, existing View/RecyclerView UI, libsndfile, FFmpeg/libx264,
SQLite repository queries, current JSON/profile settings, CMake/CTest.

**Spec:** [Storage management design](../specs/2026-10-17-storage-management-design.md).
Read the spec before executing; the details below implement its proposed design.

## Global constraints

- Chart bytes and paths never change.
- Compression enumerates all eligible media recursively under selected folders,
  including sabun-only and unreferenced files. Representative charts are display
  data, never the operation manifest. BGA deletion removes all supported videos
  recursively and preserves all images. No chart-reference or ownership scan.
- Defaults: audio Keep, BGA Keep, video quality Balanced, resolution Original,
  FPS Original, and automatic download processing Off.
- Ogg Vorbis quality is `0.5`; no resampling, gain normalization, or channel remix.
- H.264 uses `libx264`, preset `medium`, CRF 18 / 23 / 28, at most two encoder
  threads, and one active media conversion.
- Resolution presets are Original / 1920 × 1080 / 1280 × 720 / 854 × 480 /
  640 × 360; FPS presets are Original / 60 / 30 / 24 / 15.
- Only validated, strictly smaller conversions replace originals. Reclamation
  excludes any original/backup still present after cleanup failure.
- Mutate supported writable local folders only; preserve source assets on
  per-file failure and report unreadable folder scopes.
- No CLI encoder dependency, generated-parser edits, whole-file formatting,
  deployment, worktree creation, or mutations of the developer's real library.
- Work in the current checkout. Commit and push verified task-related work to
  its upstream; establish `origin/feature/storage-management` if still absent.
- Use one CMake/Ninja invocation at a time per build directory, with `-j 6`.

## Review focus

These less obvious inputs need explicit coverage, assigned below:

1. Malformed/unindexed charts must not prevent recursive media discovery, and
   all images must survive Delete BGA videos: Task 2.
2. Installing `sample.ogg` beside a same-stem alternative changes playback
   fallback: conservatively skip ambiguous conversions in Tasks 2 and 6.
3. Cancellation or a crash between publishing output and retiring input must
   retain a playable asset and recover accurate reclaimed bytes: Task 3.
4. 29.97 FPS, VFR gaps, rotated/anamorphic video, or embedded audio must not become
   sped-up, stretched, silent, or shorter after caps: Task 5.
5. A download's skip-extraction preference, mismatch Keep decision, or retry must
   not bypass verification or pick up a newly edited destructive policy: Task 9.
6. A representative chart omits another sabun's audio/video, or media is nested
   and unreferenced: Tasks 2 and 6 must still compress the full selected subtree;
   Task 9 must do the same for the entire extracted package.

## Files and dependencies

| Files | Responsibility |
| --- | --- |
| `src/storage/StoragePolicy.{h,cpp}`, `StorageTypes.h` | Options, common results/checkpoints, capability/default policy |
| `src/storage/ChartResourceInventory.{h,cpp}` | Recursive media enumeration, collision checks, eligibility and byte union |
| `src/audio/ChartAssetExtensions.h`, new `ChartVisualAssetResolver.{h,cpp}` | Shared existing audio/visual resolution semantics |
| `src/storage/StorageFileTransaction.{h,cpp}`, `StoragePlatform.{h,cpp}` | Recoverable filesystem operations and platform paths/access |
| `src/storage/AudioTranscoder.{h,cpp}`, `BgaTranscoder.{h,cpp}` | Streaming codecs and output validation |
| `src/storage/VideoCompressionGeometry.{h,cpp}` | Testable rational geometry/timestamp/cap policy |
| `src/storage/StorageOptimizer.{h,cpp}` | Sequential transform loop with injectable dependencies |
| `src/storage/StorageTask.{h,cpp}`, `StorageMaintenanceGuard.{h,cpp}` | Manual worker and admission/lifecycle ownership |
| `src/library/GroupedChartListModel.{h,cpp}`, `src/view/MusicTrackRowView.{h,cpp}` | Reused group/row presentation |
| `src/scene/StorageManagementScene.{h,cpp}`, `src/view/StoragePolicyView.{h,cpp}` | Manual UI and shared policy editor |
| Existing settings, context, download and scene files named in tasks | Integration only |
| `src/storage/CMakeLists.txt`, `cmake/StorageManagementTests.cmake` | Main source wiring and compact native test targets |

Order: 1 → 2 → 3 → 4 → 5 → 6 → 7 → 8 → 9 → 10. Codec tasks are logically
independent after the contracts, but sequential execution avoids competing
builds and interfaces. Each task includes its own tests and focused commit.

## Task 1: Policy, persistence, and common contracts

**Files:** Create `src/storage/StoragePolicy.{h,cpp}`, `StorageTypes.h`,
`CMakeLists.txt`; modify `src/AppSettings.{h,cpp}`, `AppSettingsStore.cpp`,
`src/CMakeLists.txt`, root `CMakeLists.txt`, and `cmake/TestGroupCatalog.cmake`.
Create `cmake/StorageManagementTests.cmake`, `tests/storage_policy_tests.cpp`;
extend `tests/profile_settings_persistence_tests.cpp`.

**Interfaces:** Use namespace `storage_management` for the new engine.

```cpp
enum class AudioAction { Keep, LosslessFlac, LossyVorbis };
enum class BgaAction { Keep, CompressH264, Delete };
enum class VideoQuality { High, Balanced, Smaller };
enum class ResolutionCap { Original, FullHd, Hd, Sd, Low };
struct StoragePolicy {
  AudioAction audio = AudioAction::Keep;
  BgaAction bga = BgaAction::Keep;
  VideoQuality videoQuality = VideoQuality::Balanced;
  ResolutionCap resolution = ResolutionCap::Original;
  int maxFps = 0; // 0, 60, 30, 24, 15 only
  bool operator==(const StoragePolicy &) const = default;
};
struct StoragePreferences {
  StoragePolicy manual;
  StoragePolicy download;
  bool afterDownload = false;
  bool operator==(const StoragePreferences &) const = default;
};
bool IsNoOp(const StoragePolicy &);
StoragePolicy EffectiveDownloadPolicy(const StoragePreferences &);
StoragePreferences Sanitize(StoragePreferences);
int VideoCrf(VideoQuality);
```

`StorageTypes.h` defines `Checkpoint = std::function<bool()>` (may wait for pause
on the worker; false means cancel), `StoragePhase` (Inventory, Encode, Verify,
Commit, Complete), `StorageProgress` (phase, current path, files done/total,
bytes reclaimed), and `ProgressCallback`. Define `FileOutcome` (Converted,
Deleted, Skipped, Failed, Cancelled), `FileResult` (source, destination, outcome,
reason, bytesBefore, bytesAfter, reclaimedBytes), `StorageReport` (file results,
aggregate counts/bytes, cancelled, recoveryRequired), and `EncodeResult`
(outcome, output path, reason). Reasons are typed codes plus diagnostic detail;
localization happens at presentation boundaries.

- [x] Add failing assertions for exact defaults, Keep/Keep no-op, disabled
  automation, CRF mapping, and invalid enums/FPS reverting safely. Add settings
  round-trip tests proving manual/download independence and that a malformed
  destructive policy disables automation rather than choosing Delete.
- [x] Register `storage_policy_tests` in `area_settings` and `area_library`.
  Build/run it and the existing settings persistence test; record the expected
  missing-policy or failed-assertion result before implementation.
- [x] Implement the types and persist an optional `storagePreferences` JSON
  object. Missing fields use defaults; a malformed supplied policy fails closed.
  Add `AppSettings::storagePreferences` and sanitation without changing unrelated
  settings. An additive optional object does not require a schema migration.
- [x] Run `cmake --build cmake-build-debug --target storage_policy_tests -j 6`
  and `ctest --test-dir cmake-build-debug -R '^storage_policy_tests$'
  --output-on-failure --no-tests=error`. Build the existing
  `profile_settings_persistence_tests` target and run CTest with
  `-R '^foundation_profile_settings_persistence$'`; require round-trip/default
  tests to pass.
- [x] Review the focused diff and commit `feat: define storage saving policies`.

## Task 2: Resource inventory and playback resolution parity

**Files:** Create `src/storage/ChartResourceInventory.{h,cpp}` and
`src/audio/ChartVisualAssetResolver.{h,cpp}`; modify
`src/audio/ChartAssetExtensions.h`, `src/audio/Jukebox.{h,cpp}`,
`src/audio/CMakeLists.txt`; create `tests/storage_inventory_tests.cpp` and extend
`tests/archive_asset_pipeline_tests.cpp`. Wire sources/tests in Task 1's CMake.

**Interfaces:** `InventoryRequest` contains selected group IDs/folder paths,
policy, and an isolated-staging flag. `AssetCandidate` contains source,
destination, media kind, effective operation, containing selected groups,
source identity, and bytes. `StorageInventory` contains candidates, per-group
totals, skips, completeness status, and filesystem evidence for freshness checks.
Hash large media only when preparing that candidate for conversion/commit;
ordinary browser measurement uses file identities and byte counts.

```cpp
StorageInventory BuildInventory(const InventoryRequest &, Checkpoint,
                                ProgressCallback);
bool RevalidateInventory(const StorageInventory &, Checkpoint,
                         std::string &error);
```

Expose `chart_assets::ResolveVisualAsset(path, lookup)` with an injected lookup
returning an optional resolved path; return path plus video/image kind. The helper
must work with existing local and archive lookups. Share the ordered extension
lists with inventory classification and filesystem collision checks.

- [x] Add failing fixtures for Unicode/mixed-case paths, subfolders, overlapping
  roots, preserved images, preview audio, unreadable folders, links, virtual
  sources, and cancellation. Use tiny text/assets, no media decoding.
- [x] Add a folder fixture with a representative chart and a sabun using disjoint
  audio/video, nested media, and unreferenced media. Assert each eligible file
  enters compression once. Changing representative/index/UI expansion must not
  change inventory. Malformed chart text must not prevent enumeration. Delete BGA
  must include all supported videos and preserve all audio/images.
- [x] Test same-stem audio/video alternatives: skip ambiguous conversions without
  scanning chart references. Test visual extension priority and ensure deletion
  enumerates every video alternative while preserving same-stem images.
- [x] Run the target to confirm failures, then extract visual resolution from
  all three Jukebox paths (ordinary loading, archive loading, reconciled assets).
  Preserve its current priority and audio's exact-path-first behavior.
- [x] Implement bounded recursive filesystem enumeration, selected-folder unions,
  classification, byte totals, and conservative collision checks. Resolve
  chart-path fallback groups to their containing folder. Reject unsupported
  paths/links/backends and report unreadable scopes. No chart declaration scan,
  parser, or ownership graph is part of this feature.
- [x] Verify `storage_inventory_tests` and `archive_asset_pipeline_tests`; label
  inventory tests `area_library`, `area_audio`, and `area_video`. Run `tests_audio`
  and the archive target because shared runtime resolution changed.
- [x] Commit `feat: inventory chart storage with shared asset resolution`.

## Task 3: Recoverable file changes and private staging

**Files:** Create `src/storage/StorageFileTransaction.{h,cpp}` and
`StoragePlatform.{h,cpp}`, `tests/storage_file_transaction_tests.cpp`; use
`src/AtomicFile.{h,cpp}` and `src/Utils.{h,cpp}` as durability/path references.
Add narrow platform declarations/implementations in `src/AndroidNatives.{h,cpp}`,
`src/iOSNatives.hpp`, `src/iOSNatives.mm`, or `src/MacNatives.{h,mm}` only where required by existing
platform path/security-scope APIs; do not invent generic filesystem access for SAF.

**Interfaces:** `StoragePlatformPaths` supplies private durable journal and
temporary roots. `FileTransactionDependencies` injects filesystem identity,
exclusive creation, rename/no-clobber install, flush, unlink, and fault points.

```cpp
class StorageFileTransaction {
public:
  StorageFileTransaction(StoragePlatformPaths, FileTransactionDependencies);
  FileResult Replace(const AssetCandidate &, const EncodeResult &, Checkpoint);
  FileResult Delete(const AssetCandidate &, Checkpoint);
  StorageReport Recover(Checkpoint);
};
```

- [ ] Write fault-driven tests for extension-changing and same-path replacements,
  deletion, destination appearance, source changes, disk full, and cleanup errors.
  Restart from every durable journal state. Assertions: original or validated
  output remains playable; uncommitted deletion is restored; committed cleanup
  only touches matching identities; retained backups contribute zero reclaimed
  bytes until actually removed. No fixed sleeps.
- [ ] Run `storage_file_transaction_tests` and confirm failure before writing
  transaction bodies. Register it in `area_library`.
- [ ] Implement the spec's journal state transitions and exclusive same-volume
  installation files. Hash/check source and output at commit, flush journals and
  directories, and keep recoverable backups for same-path replacement. Reject
  corrupt/out-of-root journal targets. Treat inconsistent recovery as blocking
  library access to the affected scope, not permission to remove files.
- [ ] Implement deletion as journaled rename → committed decision → unlink.
  Recheck cancellation before commit; after commitment finish consistent cleanup
  or leave an explicit recovery record. Keep temporary/journal roots outside
  Android's user-visible Documents and preserve iOS scope lifetime through commit.
- [ ] Run the transaction target once all fault scenarios are implemented and
  require every injected boundary/restart case to pass.
- [ ] Commit `feat: make chart media changes recoverable`.

## Task 4: Lossless and lossy audio conversion

**Files:** Create `src/storage/AudioTranscoder.{h,cpp}` and
`tests/storage_media_tests.cpp`; wire one shared `storage_media_tests` target
against libsndfile and FFmpeg, reusing the existing media dependency targets.

**Interfaces:** `StorageCodecCapabilities` reports usable FLAC, Vorbis, x264,
AAC, and MP4 support with unavailable reasons. `ProbeStorageCodecs()` performs
real format/capability checks; declare both in `StorageTypes.h` and implement the
probe in `AudioTranscoder.cpp` using the already linked libraries.
`EncodeAudio(const AssetCandidate &,
const StoragePolicy &, const std::filesystem::path &temporaryOutput,
Checkpoint, ProgressCallback) -> EncodeResult` creates only temporary output.

- [ ] Add small generated PCM fixtures covering 8/16/24-bit mono/stereo WAV.
  Assert FLAC fully decodes with bit-exact samples/rate/channels/frame count.
  Assert float/32-bit WAV is skipped in lossless mode. Include an impulse at the
  start and a nonzero final sample in Vorbis fixtures to catch delay/truncation;
  verify exact decoded length, rate/channels, and playable nonempty output.
- [ ] Add unavailable-codec, unsupported layout, playback-critical metadata,
  corrupt/truncated input, cancellation, and equal/larger-output tests. Test
  WAV→Vorbis and FLAC→Vorbis; Ogg/MP3 inputs must be skipped. Put policy decisions
  in pure tests and use only a few tiny real codec fixtures.
- [ ] Confirm tests fail, then implement bounded libsndfile read/write loops,
  format validation and decode-back verification. FLAC comparison uses native
  integer PCM, avoiding the app playback decoder's 16-bit conversion. Apply
  Vorbis quality `0.5`. The transcoder never installs output or deletes input.
- [ ] Probe linked capabilities instead of falling back to a different codec.
  Verify iOS packaged libsndfile support via `scripts/get_ios_libs.py` and the
  linked library/header configuration before changing dependency declarations.
- [ ] Run `storage_media_tests`; label it `area_audio`, `area_video`, and
  `area_library`. Require exact FLAC and timing assertions to pass, then commit
  `feat: encode storage optimized FLAC and Vorbis audio`.

## Task 5: H.264 BGA conversion and rational caps

**Files:** Create `src/storage/BgaTranscoder.{h,cpp}` and
`VideoCompressionGeometry.{h,cpp}`; extend `tests/storage_media_tests.cpp` and
`tests/storage_policy_tests.cpp`. Read existing FFmpeg ownership/error patterns
in `src/video/VideoPlayer.cpp` and `src/ReplayVideoExporter.cpp` without coupling
the new encoder to replay rendering.

**Interfaces:** `VideoDescription` contains coded size, sample aspect ratio,
rotation, rational frame/time bases, duration, video/audio stream descriptions,
and unsupported-content flags. `VideoCompressionPlan` contains destination
geometry/SAR, timestamp selection policy, CRF, audio treatment, and skip reason.

```cpp
VideoCompressionPlan PlanVideoCompression(const VideoDescription &,
                                         const StoragePolicy &);
EncodeResult EncodeBga(const AssetCandidate &, const StoragePolicy &,
                      const std::filesystem::path &temporaryOutput,
                      Checkpoint, ProgressCallback);
```

- [ ] Add pure assertions: 1920×1080 capped to HD → 1280×720; 1024-square →
  720-square; portrait 1080×1920 → 720×1280; 320×240 under HD stays 320×240;
  odd/SAR/rotated inputs preserve display aspect ratio within the alignment rule.
  Check every cap enum and CRF. Preserve rational rates below the cap.
- [ ] Add real, tiny MPEG-1 fixtures that produce smaller H.264 at original
  dimensions/frame timing. Include existing H.264 under caps (skip), H.264 over
  caps (convert), VFR gaps, 30000/1001 under 30, 60000/1001 capped to 30, and
  embedded audio. Assert final timeline duration and that frame dropping does
  not speed up playback; validate no duplicate/fill frames are introduced.
- [ ] Add malformed timestamp, unavailable x264/AAC, alpha/HDR/interlaced input,
  corrupt packet, truncated output, cancellation, and larger-output assertions.
  Generate fixtures once within the target; do not invoke a host FFmpeg CLI.
- [ ] Run failing tests, then implement the rational geometry/timeline policy
  and streaming FFmpeg pipeline. Use `libx264`, `medium`, CRF 18/23/28,
  two threads maximum, compatible 4:2:0 output, and preserved metadata. Remux
  supported audio or encode AAC at 128 kbit/s without timeline/layout changes.
- [ ] Decode the complete temporary output for validation in bounded buffers;
  compare duration with the spec's frame/packet tolerance and reject non-smaller
  output. Report that a requested cap was not applied when retaining the source.
- [ ] Run `storage_policy_tests` and `storage_media_tests`, then commit
  `feat: compress BGA video with resolution and FPS caps`.

## Task 6: Shared optimizer and accurate operation reports

**Files:** Create `src/storage/StorageOptimizer.{h,cpp}` and
`tests/storage_optimizer_tests.cpp`; extend inventory/transaction contracts only
where their spec responsibilities require it.

**Interfaces:** `OptimizerDependencies` injects capability probing, audio/video
encoders, transaction factory, private paths, and the shared cancellable
single-conversion admission permit. `OptimizationRequest` carries an inventory
request plus a captured policy and optional preflight inventory.

```cpp
class StorageOptimizer {
public:
  explicit StorageOptimizer(OptimizerDependencies);
  StorageReport Run(const OptimizationRequest &, Checkpoint, ProgressCallback);
};
```

- [ ] Add dependency-fake tests that assert inventory/revalidation → encode →
  verify → transaction order, distinct-source deduplication, preserved images,
  complete BGA video deletion, stale preflight rejection,
  and unchanged chart hashes. Revalidate resolver results after each installation
  so a prior replacement cannot invalidate a later candidate's assumptions.
  Refresh evidence for exactly the job's committed changes; an external edit
  between two commits must still invalidate the affected candidate.
- [ ] Verify full-folder orchestration converts representative-only, sabun-only,
  nested, and unreferenced eligible media once each, while retaining non-media
  files and preserving every image/audio file during BGA deletion. Filesystem
  collision checks must still apply to compression of the enumerated file union.
- [ ] Test partial successes, no-op, codec unavailable, skipped caps, cancellation,
  cleanup failure, and fatal recovery. Reports must distinguish attempted from
  installed bytes and avoid double-counting shared files. Unknown sizes stay
  unknown. A normal failure continues to the next asset; recoveryRequired stops
  the affected scope. Verify the single-conversion permit with deterministic
  synchronization rather than sleeps.
- [ ] Run failing tests, then implement sequential orchestration with one private
  candidate at a time. Honor checkpoints between chunks/frames and before each
  commit. Cap progress notifications and keep diagnostics bounded (counts plus
  a bounded detail list, with a private full report if needed).
- [ ] Run `storage_optimizer_tests` and all new storage targets in one build,
  followed by `ctest --test-dir cmake-build-debug -R '^storage_.*_tests$'
  --output-on-failure --no-tests=error -j 6`. Register optimizer tests in
  `area_library`; require all new tests to pass.
- [ ] Commit `feat: orchestrate cancellable chart storage optimization`.

## Task 7: Manual job lifecycle and mutation admission

**Files:** Create `src/storage/StorageTask.{h,cpp}`,
`StorageMaintenanceGuard.{h,cpp}`, and `tests/storage_task_tests.cpp`; modify
`src/context.h`, `src/ApplicationStartup.{h,cpp}`, `src/scene/SceneManager.cpp`,
`src/library/ChartLibraryTaskService.{h,cpp}`,
`src/audio/MusicPlayerService.{h,cpp}`, and relevant preview admission points
(`MainMenuPreviewController`, `MusicSelectPreview`). Extend
`src/AndroidNatives.{h,cpp}` and Android `AsoBMaShowActivity.java` /
`DocumentsMutationGuard.java` with a narrow subtree reservation bridge.

**Interfaces:** `StorageTask::Start(OptimizationRequest) -> bool`,
`RequestCancel()`, `Snapshot() -> StorageTaskSnapshot`,
`TakeCompletion() -> std::optional<StorageReport>`, and `Shutdown()`.
`StorageTaskSnapshot` carries running/cancelling status and immutable progress.
`StorageMaintenanceGuard` is move-only RAII ownership supplied after application-
thread admission and media/preparation shutdown acknowledgments. Its dependencies
are callbacks so lifecycle tests do not need the renderer/audio device.

- [ ] Add failing tests for simultaneous Start, cancel before/after commit,
  bounded progress handoff, scene destruction, app shutdown, profile change, and
  startup recovery before any library reads. Assert no callback can outlive its
  owner and no worker holds UI references.
- [ ] Add admission tests for pending library tasks, a preview producer, music
  playback/preparation, native next/play events, gameplay, export, and new imports.
  Existing owners must quiesce or refuse maintenance; newly arriving owners cannot
  enter until release. Use explicit barriers and fake callbacks.
- [ ] Implement application-thread admission before spawning the job. Stop/join
  preview and music sources; retain the reservation through refresh handoff.
  Wire mobile backgrounding to cancellation and profile-switch/shutdown sequencing
  to worker completion. Integrate recovery before normal startup library access.
- [ ] Extend existing library-task admission with a maintenance reservation rather
  than relying on a racy active-count check. Reserve Android Documents subtrees
  through the shared Java mutation monitor; test writes/renames/deletes reject
  reserved files and release correctly after cancellation.
- [ ] On completion, invalidate affected resolution/media state and storage
  measurements, then enqueue existing affected-path refresh. Resume playback only
  on a new user/native play request; never reuse a stale progressive source.
- [ ] Run `storage_task_tests`, the affected `area_library` lifecycle tests,
  `area_audio` tests, and `python3 tests/android_documents_mutation_guard_tests.py`.
  Commit `feat: coordinate storage maintenance with media and library tasks`.

## Task 8: Shared grouped browser and storage controls

**Files:** Create `src/library/GroupedChartListModel.{h,cpp}`,
`src/view/MusicTrackRowView.{h,cpp}`, `src/view/StoragePolicyView.{h,cpp}`,
`src/scene/StorageManagementScene.{h,cpp}`, and
`tests/grouped_chart_list_tests.cpp`; modify `src/scene/MusicPlayerScene.{h,cpp}`,
`SettingsScene.{h,cpp}`, `SettingsSceneLayout.cpp`, `SettingsSceneControls.cpp`,
`src/view/CMakeLists.txt`, `src/scene/CMakeLists.txt`, and `src/i18n/Messages.inc`.

**Interfaces:** `GroupedChartListModel::SetGroups(std::vector<MusicTrack>)`,
`SetChildren(std::string groupId, std::vector<MusicTrack>)`,
`SetExpanded(std::string groupId, bool)`, `SetQuery(std::string)`, and
`Rows() -> const std::vector<MusicTrack>&` preserve existing music-player filter
semantics. Keep multi-selection in a storage-specific model keyed by groupId.
`MusicTrackRowView` accepts the existing track plus an explicit trailing action
(favorite in music, group checkbox/storage summary in management).
`StoragePolicyView` edits a value `StoragePolicy` via a change callback; it does
not access workers or global settings directly.

- [ ] Characterize existing group identity/search/expansion behavior in tests
  before extraction. Assert group selection survives search, expansion, sorting,
  and revision refresh; hidden selections remain counted; child rows cannot
  silently request an isolated shared-media mutation.
- [ ] Extract row formatting, artwork lifetime, and grouped filtering into shared
  units. Update every music-player row consumer (library, favorites, playlists,
  and queue) without changing favorites/playback/scroll behavior. Keep grouping
  SQL in `MusicPlaylistRepository`; do not route storage through the playback queue.
- [ ] Build the storage scene using asynchronous inventory, typed per-group
  measurements, multi-select/preflight, policy controls, confirmation, progress,
  cancellation, and reports. Unsupported sources display the actionable reason
  from the spec. Clear all views' stale callbacks on teardown.
- [ ] Add Settings navigation and both policy editors; display download policy
  independently from manual policy. Save through the existing profile persistence
  coordinator. Localize all new UI text in the existing message catalog. Confirm
  automatic Delete BGA when enabled, without prompting on subsequent downloads.
- [ ] Run `grouped_chart_list_tests` (label `area_library` and `area_ui`), music
  player repository/UI lifecycle tests, and `localization_catalog_tests`. Build
  `main`; inspect responsive desktop/portrait layouts and music controls using
  a disposable test library. Verify idle UI redraw follows existing scheduling.
- [ ] Commit `feat: add grouped chart storage management UI`.

## Task 9: Automatic storage saving in Find BMS

**Files:** Modify `src/BmsSearchService.h`, `src/bms_search/DownloadSupport.cpp`,
`DownloadedArchiveWorkflow.{h,cpp}`, `src/scene/FindBmsModal.{h,cpp}`,
`FindBmsProgressPresentation.{h,cpp}`, `MainMenuScene.cpp`, `MusicSelectScene.cpp`,
and `src/i18n/Messages.inc`; extend `tests/find_bms_download_tests.cpp`,
`tests/find_bms_task_tests.cpp`, and existing progress tests. Introduce
`src/bms_search/DownloadStorageOptimization.{h,cpp}` for optimizer adaptation and
wire it in `src/bms_search/CMakeLists.txt`.

**Interfaces:** Add `StoragePolicy storagePolicy` to `BmsSearchDownloadOptions`
(already effective, immutable policy; Off maps to Keep/Keep). Add an injected
`optimizeExtracted(root, policy, checkpoint, progress) -> StorageReport` callback
to `DownloadedArchiveWorkflowDependencies`. Carry an optional structured
`StorageReport` in `BmsSearchResult`; do not change its Downloaded status for
recoverable optimization warnings. Progress gains an optional typed storage phase
without repurposing downloaded byte counters as encoding percentages.

- [ ] Add workflow fake assertions in order: extract → verify chart hash →
  optimize → verify unchanged chart identities → commit → filesReady/index.
  Off and Keep/Keep must not call the optimizer or override skip-unarchiving.
  Enabled effective policy must force extraction while preserving the setting.
- [ ] Verify optimization receives the entire extracted package subtree, including
  nested media and resources absent from the hash-matched chart but used by a
  different sabun. Reuse the folder-scope inventory/optimizer fixture for coverage.
- [ ] Add cases for archive and extracted mismatch, inconclusive verification,
  explicit mismatch Keep, unsupported codecs, per-file errors, failed rollback,
  cancellation during encoding and immediately before commit. Assert mismatches
  are kept unchanged, warnings reach successful results, and cancellation/fatal
  recovery prevents commit/index.
- [ ] Test both lookup/candidate entry paths in both selector scenes, retry
  policy stability, profile/policy changes during an attempt, and a retained
  dialog returning after completion. Test optimization progress does not display
  as downloaded bytes or prematurely reach the final completion state.
- [ ] Run failing download tests, then insert the hook after successful extracted
  verification and before `commitArtifact`. Use the same optimizer/codec permit,
  isolated-staging inventory, and captured settings; honor cancellation and the
  existing foreground-performance pause signal. Leave mismatch resolution on
  the existing path and explain that kept mismatches were not optimized.
- [ ] Surface the saved strategy and effective extraction override in the dialog;
  expose completion warnings/counts and reclaimed media bytes without conflating
  them with downloaded archive size. Reuse existing artifact cleanup and index
  callbacks. No extra network request is needed for optimization.
- [ ] Run `cmake --build cmake-build-debug --target tests_downloads -j 6`, then
  `ctest --test-dir cmake-build-debug -L '^area_downloads$' --output-on-failure
  --no-tests=error -j 6`. All existing verification/mismatch tests must still pass.
- [ ] Commit `feat: apply storage saving policies after BMS downloads`.

## Task 10: Integration verification and documentation

**Files:** Extend focused tests above where integration exposes a gap; add
`docs/storage-management.md`; update `docs/testing/README.md` only if necessary
to explain group membership. Update this plan's checkboxes/evidence as executed.

- [ ] Use a disposable library with two chart difficulties, disjoint sabun audio
  and video, nested and unreferenced media, a shared keysound,
  a protected stage image also used as BGA, a small MPEG-1 video, and an archive
  group. Record chart hashes, scores/playlists identities, and initial file bytes.
  Exercise manual FLAC, Vorbis, H.264 with/without caps, and BGA deletion; verify
  gameplay, preview, and music player load the resulting assets. Delete only the
  test fixture directory afterward. Verify compression covers the whole folder
  independently of the displayed representative and that deletion remains limited
  to supported video files, including unreferenced ones, while preserving images.
- [ ] Exercise the download workflow with a local fake archive/transport fixture,
  including effective skip-extraction override, warnings, cancel, and unchanged
  mismatches. Verify registration occurs only after optimization finishes. Do not
  download a real package just to test the feature.
- [ ] Build the affected groups together:
  `cmake --build cmake-build-debug --target main tests_library tests_audio
  tests_video tests_downloads tests_settings tests_ui -j 6`.
  Run `ctest --test-dir cmake-build-debug -L
  '^area_(library|audio|video|downloads|settings|ui)$' --output-on-failure
  --no-tests=error -j 6`. These commands are each one shell command despite line
  wrapping. Broaden only for unresolved cross-subsystem risk or a new failure.
- [ ] Verify iOS and Android linked encoders and native local-folder operation
  through the established build-only paths (`scripts/ios_firebase_deploy.sh
  --build-only`, `scripts/android_firebase_deploy.sh --build-only`, with required
  private signing setup). Compile on the established Windows runner and check
  no-clobber/rename behavior there. Report unavailable platform validation rather
  than claiming cross-platform completion. Never run distribution/upload commands.
- [ ] Document caps, quality, lossless limitations, deletion/artwork scope,
  supported backends, partial cancellation, recovery, warning meanings, and
  automatic extraction precedence. Include that cap requests can be skipped
  when the candidate is not smaller, and that existing under-cap H.264 is skipped.
- [ ] Review spec coverage and the full task diff, record focused test evidence,
  commit `docs: explain chart storage management`, and push the current feature
  branch. Do not merge or deploy as part of this plan.

## Planning review

The plan covers the requested manual operations, grouped-list reuse, automatic
downloads, and resolution/FPS upper bounds. The primary review choices are the
proposed quality defaults, folder-wide BGA video deletion preserving images, and local-file
mutation scope. Archived/SAF libraries remain visible but require the existing
extraction/import route before manual optimization.

Recommended execution: implement in this session with the native execution
workflow, because inventory, lookup behavior, transactions, and lifecycle share
contracts. Review this spec and plan before starting product changes; if per-task
subagents are preferred, select that execution method explicitly.
