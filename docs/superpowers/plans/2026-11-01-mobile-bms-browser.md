# Mobile BMS Browser Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add mobile site browsing and move existing Find BMS downloads onto persistent background jobs.

**Architecture:** Resolve provider selections using existing lookup code, then submit a serial durable archive job. Native transports own background file transfer; an application-owned coordinator performs the existing archive workflow and delivers durable results. Native browsers submit to the same service.

**Tech Stack:** C++20, nlohmann JSON, Objective-C++/Foundation/WebKit/BackgroundTasks, Java/Android WebView/JobScheduler/foreground service, CMake/CTest.

**Spec:** `docs/superpowers/specs/2026-11-01-mobile-bms-browser-design.md` (approved 2026-11-01).

## Global Constraints

- iOS and Android first; keep existing minimum OS support and desktop transport.
- Both Find BMS and browser downloads use the same mobile service.
- One active archive job initially; subsequent selections are durably queued.
- No scene/stack pointers in platform callbacks; no waiting for background dialogs.
- Preserve TLS, staging, validation, bounded-memory, atomic publication and artifact decisions.
- Private durable state is outside Documents/caches; no secrets in logs or backups.
- No deployments, parser edits, worktrees or whole-file formatting.

## Review Focus

- Cancellation racing download completion must prevent a cancelled archive from becoming visible (Tasks 1–3).
- A killed process between file movement and manifest publication must recover without losing the completed archive (Tasks 1–3).
- Cross-origin redirects must not leak browser cookies or weaken HTTPS downgrade rejection (Tasks 3–4).
- A recreated scene must observe existing progress and publish/index each result once (Tasks 2, 5).
- Browser-generated non-replayable requests must use a disclosed foreground fallback without duplicate requests (Task 4).

### Task 1: Durable job state and browser request policy

**Files:** Create `src/downloads/DownloadJobs.h`, `src/downloads/DownloadJobs.cpp`, `src/downloads/BrowserPolicy.h`, `src/downloads/BrowserPolicy.cpp`, `tests/download_jobs_tests.cpp`, `cmake/DownloadJobsTests.cmake`; modify `CMakeLists.txt`, `cmake/TestGroupCatalog.cmake`.

**Interfaces:** Produce `downloads::Job`, `downloads::Request`, `downloads::JobStore` with `submit(Request)`, `jobs()`, `get(id)`, `update(Job)`, and durable revision/state validation. Request records include resolved URL, source/title, expected chart hash, destination root and captured archive options. Browser policy produces validated HTTP(S) requests and GingerRush table URLs without executing page scripts.

- [ ] Add behavioral tests for reload, queue order, terminal-state races, invalid/corrupt manifests, private paths and unsafe request rejection. A reopened store must retain a completed job's ID and refuse stale updates.
- [ ] Run the new test target, observing missing implementation failures.
- [ ] Implement atomic private manifest replacement, bounded reads, stable job IDs and explicit allowed state transitions. Never persist callback objects.
- [ ] Run `cmake --build cmake-build-debug --target download_jobs_tests -j 6` and `ctest --test-dir cmake-build-debug -R '^download_jobs_tests$' --output-on-failure`. Expected: pass.
- [ ] Commit the tested core.

### Task 2: Application coordinator and Find BMS handoff

**Files:** Create `src/downloads/DownloadCoordinator.h`, `src/downloads/DownloadCoordinator.cpp`, `src/downloads/DownloadPlatform.h`; modify `src/BmsSearchService.h`, `src/bms_search/DownloadSupport.cpp`, `src/scene/FindBmsModal.h`, `src/scene/FindBmsModal.cpp`, `src/context.h`, `src/main.cpp`; extend `tests/download_jobs_tests.cpp` and existing Find BMS behavioral coverage.

**Interfaces:** Consume Task 1 Job/Request/JobStore. Produce `downloads::DownloadCoordinator` admission, snapshot, cancel, retry, artifact-decision and completion acknowledgement methods. Add an optional resolved-archive handoff in `BmsSearchDownloadOptions`; default behavior stays synchronous for desktop/tests. Add an import-from-completed-file entry to the existing archive workflow so resumed work never repeats a successful transfer.

- [ ] Add tests proving resolved handoff preserves expected hash/storage policy and does not download twice; detached UI cannot cancel a durable job; retained archives can be imported without network.
- [ ] Run focused tests and verify failures at the missing behavior.
- [ ] Implement durable coordinator transitions and shared archive import, capturing settings by value. Pause import on runtime withdrawal; retry/keep/delete are explicit states. Stage and reconcile before publication; acknowledge library indexing separately and idempotently.
- [ ] Migrate mobile Find BMS admission and progress observation; bounded lookup retains FindBmsTask ownership. Keep desktop behavior intact.
- [ ] Run download subsystem tests. Expected: all pass, including existing artifact and scene lifecycle coverage.
- [ ] Commit the coordinator and migration.

### Task 3: Native background transports and processing

**Files:** Create `src/downloads/DownloadPlatformIOS.mm`, `src/downloads/DownloadPlatformAndroid.cpp`, `src/downloads/DownloadPlatformDesktop.cpp`, `android/app/src/main/java/com/snurhythm/asobmashow/BmsDownloadService.java`, `android/app/src/main/java/com/snurhythm/asobmashow/BmsDownloadJobService.java`; modify iOS application delegate integration/Info.plist/Xcode links, Android manifest and JNI initialization as needed.

**Interfaces:** Implement Task 2 platform contract for start/reconcile/cancel, foreground admission, progress and completed-file ownership. The coordinator is the only job-state authority. Platform callbacks identify jobs by stable IDs and can durably record completion without a live scene.

- [ ] Add native callback/reconciliation tests covering completion/cancel races, secure redirects, protected staging, missing system tasks and duplicate completion delivery.
- [ ] Run targeted tests and observe the missing behavior.
- [ ] Implement background NSURLSession with stable identifier, delegate reconnection and timely completion handlers. Add availability-gated iOS 26 continued processing from foreground action with progress, cancellation and expiration handling.
- [ ] Implement Android 14+ UIDT transfer jobs and older dataSync foreground service with notification cancellation and independent native initialization. Preserve resume validators; wait for foreground when queued work lacks legal admission.
- [ ] Compile unsigned iOS and Android release builds using repository helpers with `--build-only`; run native focused tests. Expected: successful builds and behavioral tests. Device-only limitations remain explicit until exercised.
- [ ] Commit platform support.

### Task 4: Native browsers and selected-download handoff

**Files:** Create `src/downloads/BmsBrowser.h`, `src/downloads/BmsBrowserIOS.mm`, `src/downloads/BmsBrowserAndroid.cpp`, `android/app/src/main/java/com/snurhythm/asobmashow/BmsBrowserActivity.java`; extend BrowserPolicy tests and platform build registrations.

**Interfaces:** Consume validated Request and coordinator submission from Tasks 1–3. Produce `downloads::openBrowser()` and table-import events consumed by application integration. Browser lifetime never owns jobs.

- [ ] Test GingerRush table recognition, untrusted origin rejection, attachment filename handling, new windows and request replay eligibility.
- [ ] Run focused tests; expected initial failures for unimplemented behavior.
- [ ] Implement bookmarks/navigation/last URL, explicit download handoff and session-scoped request context. Keep arbitrary native access unavailable to page JavaScript. Provide explicit foreground or external fallback for non-replayable requests; never silently replay POST/blob actions.
- [ ] Verify native compilation and browser-policy tests. Expected: pass.
- [ ] Commit browser integration.

### Task 5: Shared Downloads UI and library/table delivery

**Files:** Create `src/scene/DownloadsModal.h`, `src/scene/DownloadsModal.cpp`; modify `src/scene/MainMenuScene.h`, `src/scene/MainMenuScene.cpp`, `src/main.cpp` and localization catalogs; use `DifficultyTableImporter` and existing library task interfaces.

**Interfaces:** Consume coordinator snapshots/actions and browser table events. Deliver idempotent application-owned library indexing and table import; no callback retains a destroyed scene.

- [ ] Test scene detachment/recreation, retry and artifact decisions, duplicate library completion and duplicate table import.
- [ ] Run relevant tests; expected failures at missing integration.
- [ ] Add Browse BMS and Downloads controls on mobile; render real phases and recoverable states. Closing a browser/dialog detaches; explicit Cancel cancels.
- [ ] Run download/library/UI affected tests and desktop main build. Expected: pass.
- [ ] Commit UI and delivery.

### Task 6: Integration verification and review

**Files:** Update feature documentation and this plan with evidence; fix implementation issues in their owning modules.

**Interfaces:** Exercise the complete contracts from Tasks 1–5.

- [ ] Run affected CTest groups and both unsigned mobile builds once after integrated changes.
- [ ] Exercise available devices for background/lock, connectivity interruption, cancellation, process recovery, browser handoff and archive import. Record unavailable hardware coverage honestly; no deployment.
- [ ] Obtain one fresh whole-branch code review against the approved spec, resolve material findings, and run appropriate regressions.
- [ ] Commit and push verified task changes to the existing branch; update/link PR #174 with the final scope and evidence.

## Execution record

- Tasks 1 and 2 implemented and committed as `3fba811e` and `fa52b380`.
- Tasks 3–5 implemented and committed. Desktop, unsigned iOS, and Android
  release builds pass. Android installation and foreground-service device checks
  completed; unavailable physical-device coverage is recorded below.
- Task 6 fresh whole-branch review completed. Its four Important findings have
  regression tests and fixes; no Critical or Minor findings remain from that review.
- The user requested a push immediately after Android installation and before
  device testing, so iOS testing can proceed in parallel. This intermediate push
  does not claim completed physical-device verification.
- Evidence and remaining platform coverage: [verification record](../../testing/mobile-bms-downloads-2026-11-01.md).

- Runtime fixes committed and pushed as `62d55ce1`: owned iOS callback snapshots,
  direct retained-file import on Android, Manbow URL, table sidebar refresh,
  embedded iOS link navigation, address contrast, and Downloads width constraints.
- Final Android checks passed for retained import without redownload, notification
  cancellation, popup confirmation, POST fallback, and Manbow navigation. Final
  documentation records the remaining physical iOS and Android 14+ boundaries.
