# PR 103 Reviewer–Fixer Cycle

> **For agentic workers:** Use superpowers:subagent-driven-development for fixes and superpowers:requesting-code-review for independent review. The user requests continued cycles until reviewers report no remaining actionable problems.

**Goal:** Resolve the eight new GitHub findings, then independently review the entire PR and fix verified findings until review converges.

**Architecture:** Preserve the existing selector, immutable repository snapshots, cancellable workers, and bounded page providers. Fix root causes with behavioral regressions rather than adding unrelated features or weakening compatibility checks.

**Tech Stack:** C++23, SQLite, Lua, CMake/CTest, existing Python scene-contract fixtures.

**Execution status (2026-09-09 KST):** Implementation, independent source review and build/test gates are complete at code freeze `f9f6bd667cbfbe413227bd0c34105335f172c281`. Publication follows this documentation commit; the remaining publication checkbox records that ordering, not an unresolved code finding.

**Spec:** The user-requested review remediation and the eight GitHub threads listed in the tasks below. Existing pinned behavior authority: `docs/superpowers/specs/2026-09-01-beatoraja-lua-music-select-design.md`.

## Global Constraints

- Work in the existing `feature/chart-select-skin` checkout. No new worktrees, branches, parser edits, distributions, or whole-file formatting.
- Leave the unrelated untracked `docs/reviews/` directory untouched.
- Use regression-first fixes; preserve tests that prove success, failure, cancellation, and retry behavior.
- Controller owns integration, per-fix commits, and one final push. Fixers never spawn agents or commit. Only one fixer owns the working tree at a time; reviewers inspect frozen committed ranges.
- Native build ownership is exclusive. Use `cmake-build-debug`, `-j 6`, and parallel full CTest at integration.
- Inherit the session model. Use medium reasoning for isolated fixes, high for multi-file lifecycle/SQL changes, and xhigh for final whole-PR review. Increase effort only if a difficult finding warrants it.
- The user's convergence request overrides fixed review-round caps. Do not hide unresolved findings to claim a clean review.

### Task 1: Lua dispatch/input bounds and archive suffix safety

**Files:** `src/skin/beatoraja/MusicSelectSkinSession.cpp`, `src/skin/beatoraja/MusicSelectSkinStateBridge.cpp`, `src/ArchiveFile.cpp`, and their existing tests; corresponding headers only if needed.

**Interfaces:** Keep existing action/diagnostic APIs and safety-policy semantics. Do not alter MusicSelectScene in this task.

- [x] Reproduce review 3956217796 with a real Lua event callback dispatching itself and a mutual callback cycle. Bound host-side callback dispatch across floats, strings, and events; a valid finite nested chain still completes. Test strict and compatibility safety policies so neither can hang the host.
- [x] Reproduce review 3956217803 for float properties 17–19. Reject non-finite values without publishing an override/action; publish `std::clamp(value, 0.0, 1.0)` for finite values. Test negative, above-one, endpoints, ordinary values, NaN, and infinities through the bridge.
- [x] Reproduce review 3957560698 with unrelated regular filenames of lengths 5, 6, and 7 in the archive-index cache. Replace the unsafe subtraction/comparison with a length-safe suffix predicate such as `fileName.ends_with(".idx.tmp")`. Preserve valid index/temp cleanup and unrelated files.
- [x] Run focused tests, report each RED/GREEN result and exact changed files, then freeze for independent review and controller commits.

### Task 2: Selector pause and gameplay launch lifecycle

**Files:** `src/scene/MusicSelectScene.cpp`, its header as needed, and existing `tests/music_select_scene_*_fixture.cpp` / `tests/music_select_error_flow_contract_tests.py`.

**Interfaces:** Reuse the existing `folderStatusLoader_->cancel()` and revision/retry reset sequence, `launchThread_`, `launchCancelled_`, deferred scene transitions, and the loading overlay. Capture UI-owned selections before starting workers.

- [x] Reproduce review 3956217809: pause cancels active folder statistics and prevents stale publication; resume reloads/requests current statistics without losing normal navigation behavior.
- [x] Reproduce review 3957560690 in chart-slot, course-slot, and records replay launches. Require both successful `BackendOperationResult` and no cancellation before gameplay; failed audio leaves a recoverable selector and does not consume a transition.
- [x] Reproduce review 3957560694 with gated course parse/audio work. Course and folder-autoplay staging must run on a cancellable worker while the UI/loading overlay remains responsive. Post success/failure to the scene thread with stale-generation/lifetime checks; cancel/join on teardown, and do not read mutable UI/settings from workers.
- [x] Preserve course constraints, replay provenance, table context, return scene, options, and retry behavior. Test success, failure, cancellation, deferred completion after pause/teardown, and subsequent successful launch.
- [x] Run focused regressions and freeze for independent review and controller commits.

### Task 3: Recoverable paging and bounded search

**Files:** `src/music_select/MusicSelectSqlSongs.*`, relevant row-provider/bar-manager/directory-loader files, `src/repositories/ChartRepository.h`, `src/repositories/ChartRepositoryQueries.cpp`, `src/scene/MusicSelectScene.*`, and focused repository/provider/scene tests.

**Interfaces:** Reuse the bounded SQL provider and cancellable directory loading path. Keep repository snapshot validation, representative/visibility/filter/sort semantics, stable identities, and retained read views intact.

- [x] SEL-03-R2 P1 from `review-selector-round2.md`: while owning the same Scene files, close the remaining multiple-Lua-actions handoff path first. Stop dispatch after scene ownership is lost and reject inactive launch-work entry. Test real `consumeActions` plus real launch entry with two queued Play actions, a ready preload, and actual pause; ensure exactly one launch, no post-handoff worker/Jukebox staging, and valid launch after resume. No arbitrary event cap; preserve all Task 8 handoff/revision and Task 2 generation tests. This is a third, separate corrective commit in this wave.

- [x] Reproduce review 3955943140 on an unprimed middle page. Surface failure in the production scene, expose bounded retry/reload, and invalidate failed cached placeholders without an every-frame retry storm. Test transient recovery, repeated failure/backoff or explicit retry, snapshot invalidation, and leaving/reopening a folder.
- [x] Reproduce review 3957560705 on a large broad keyword match. Search submission uses a bounded existence query without full chart projection; opening a SearchWord uses the same bounded paged provider architecture rather than eager `QueryChartMeta()` on the UI thread.
- [x] Preserve existing keyword matching, raw search-history existence semantics, search identity, duplicate representative choice, mode/difficulty fallback, sort order, hidden rows, and cancellation/error handling. Exercise first/middle/wrapped pages and repository revision changes.
- [x] Assert query/decoded-row bounds in tests rather than fragile wall-clock thresholds. Run focused suites, then freeze for independent review and controller commits.
- [x] SEL-01 is now covered by Task 8's cohesive selector/Records fix wave; do not duplicate it in the SQL/search task.

### Task 4: Independent whole-PR reviewer–fixer loop

**Review range:** merge base `637f862464bd494b079b23ac6418ce2076e2fb4a` through the latest committed head, not only Task 1–3 diffs.

- [x] Partition all changed production/build/test paths across fresh reviewers so ownership and coverage are explicit. Review correctness, lifecycle/concurrency, compatibility, bounded resource use, platform integration, and whether tests validate real production behavior.
- [x] Require concrete findings with path/line, triggering conditions, impact, and evidence. Do not treat already-fixed GitHub comments or speculation as new findings.
- [x] Dispatch a fixer for confirmed findings, run targeted regressions, commit each logical fix, and have an independent reviewer verify the fixes and affected interactions.
- [x] Continue until the final review of the current committed tree reports zero actionable findings. Record rounds, findings, disposition, and review coverage.

### Task 5: Integration and GitHub closure

- [x] Run desktop `main`/all relevant builds and all CTests with `-j 6`; run unsigned iOS `scripts/ios_firebase_deploy.sh --build-only --skip-init` after final source changes.
- [x] Run Android `scripts/android_firebase_deploy.sh --build-only` after final source changes to compile the complete Activity/helper/JNI/NDK integration; host-JVM fixtures do not replace this gate. Do not upload or override the automatic version code.
- [x] Record the final evidence and any untested platform/device limits. Preserve a clean tracked worktree.
- [ ] Push once after the review loop converges, reply in each addressed GitHub inline thread with fix/test evidence, and verify remote head equality. Re-fetch new feedback before reporting completion.

### Task 6: Platform review findings, first fix wave

**Priority:** Execute after Task 2 and before Task 3 because PLAT-01/02 are P1 findings. Independent evidence is in `.superpowers/sdd/2026-09-08-pr103-review-cycle/review-platform-round1.md`.

**Files:** `src/ArchiveFile.cpp/.h`, `src/library/ChartLibraryPlatform.cpp`, `src/AndroidNatives.cpp/.h`, `android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowActivity.java`, `src/ChartLibraryScanner.cpp/.h`, `src/library/ChartLibraryOperations.cpp`, and focused archive/library/Android lifecycle tests; adjust only needed files.

**Interfaces:** Preserve bounded-read guarantees for every enabled backend, picker owner lifetime, Android result/permission delivery, and transactional scoped scanning. Do not edit Scene/Lua/SQL-provider files from other tasks.

- [x] PLAT-01: Reproduce an underdeclared ZIP BZIP2/method-12 entry that miniz cannot decode but libarchive can. Ensure `readFileBounded()` never runs an unbounded intermediate extraction. Test incremental stopping at the caller's budget, not only eventual failure; retain valid supported fallback formats, integrity rejection, and cancellation.
- [x] PLAT-02: Reproduce Activity destruction while the sound-folder picker waits, and destruction racing dispatch. Release/reject native folder and permission waits before SDL's UI-thread join, and wire owner cancellation where necessary. No detached threads retaining destroyed state. Preserve successful selection, user cancellation, repeated requests, and import-picker behavior. Use executable lifecycle tests, not only string assertions.
- [x] PLAT-03: Reproduce Update Folder after changing an existing ordinary chart at the same path. Reparse changed/explicitly refreshed charts within the requested scope, including relevant folder-preview metadata, without disturbing other roots. Preserve rollback/checkpoint cancellation and post-refresh identities/metadata. Verify ordinary and archived behavior remain correct.
- [x] Run focused RED/GREEN regressions and freeze. The controller verifies and commits each logical fix, then the independent platform reviewer checks all findings and affected interactions. Escalate any discovered scope conflict rather than changing unrelated subsystems.

### Task 7: Skin review findings, first fix wave

**Evidence:** `.superpowers/sdd/2026-09-08-pr103-review-cycle/review-skin-round1.md`; six findings from the full 117-path review, including one P1. Read the detailed triggers and pinned evidence there before changing code.

**Files:** `src/skin/beatoraja/SkinResourceCatalog.*`, `MusicSelectSkinSession.*`, `LuaSkinHostModules.cpp`, `Skin2DRenderer.cpp`, necessary adjacent atlas/coordinator interfaces, and their focused tests/fixtures. Keep Scene, SQL/provider, platform/library, and external `docs/reviews/` untouched. Coordinate native build ownership.

**Interfaces:** Preserve parallel decoding and compatibility policy semantics, production resource planning, LuaJ's pinned host behavior, runtime glyph virtualization without per-title texture rebuilds, timer last-definition ownership, and graph animation layout.

- [x] SKIN-1 P1: Bound outstanding/ready decoded ownership before launching the complete image corpus. Account/reserve decoded work or use a scheduling/draining window with aggregate admission; six workers alone are not backpressure. Use small fake-decoder barriers and quantitative admission/ownership assertions, not an OOM test. Preserve cancellation and cache ownership.
- [x] SKIN-2 P2: Collect distribution-graph sprite resources and regions in production `collectResourceUses`, including criticality. Exercise Lua decode through production planning/upload/lowering for graph-only sources and shared-source/different-grid sources, both standalone and nested.
- [x] SKIN-3 P2: Supply LuaJ's non-mutating `os.setlocale` behavior (`"C"`) where safe OS compatibility is enabled. Preserve the function surface and process-global mutation prohibition; assert unchanged native locale/numeric parsing, not only function presence. Test locale mutation only with proper isolation/restoration.
- [x] SKIN-4 P2: Preserve resident kerning pairs through glyph patches and learn new pairs made solely from resident glyphs without rebuilding textures on every title change. Test AV before/after adding B and reordered resident glyphs with real nonzero kerning/layout evidence; keep state bounded by existing policy.
- [x] SKIN-5 P2: Determine active custom timer IDs from final duplicate definitions, not any earlier definition. Test callback-to-passive and passive-to-callback definitions, event writes and following-frame persistence against pinned semantics.
- [x] SKIN-6 P2: Floor complete 11/28-state distribution animation frames, ignoring trailing source cells in standalone and nested lowering; reject only zero usable frames. Test 12-cell normal, 29-cell rank, and multi-frame nonmultiples.
- [x] SEL-06 P1 from `review-selector-round1.md`: extend this wave's scope to `src/scene/play/GamePlayScene.cpp` and the builtin-image batch interface/tests. Carry the active encoded-byte budget into preferred archive batching and enforce it before full buffering, preserving unrestricted compatibility and small-image success/cancellation. Reuse corrected `readFileBounded` where possible rather than broadening ArchiveFile APIs unnecessarily. Prove bounded reads/allocations with truthful oversized archive entries. This is separate from PLAT-01's underdeclared fallback defect and must have its own logical commit.
- [x] Establish regression RED/GREEN for all seven findings, run focused suites, freeze and report; prepare separate logical fixes for controller commits. Independent re-review must find all addressed and no new breakage before this slice is clean. Coordinate the gameplay-service delta with the selector reviewer too.

### Task 8: Selector lifecycle and Records review findings

**Evidence:** `.superpowers/sdd/2026-09-08-pr103-review-cycle/review-selector-round1.md`, plus `task-2-review.md` for previously approved G4/G5/G6. Six findings here; SEL-06 is assigned to Task 7 because it shares the resource-catalog budget interface.

**Files:** `src/scene/MusicSelectScene.*`, `src/music_select/MusicSelectBarManager.cpp`, `src/scene/MainMenuScene.*`, `src/scene/SettingsSceneLayout.cpp`, `src/scene/SettingsSceneSkins.cpp`, and focused existing/new Scene/modal/provider fixtures. Do not edit SQL/search/provider APIs reserved for Task 3, skin implementation reserved for Task 7, Archive/platform/library, or external `docs/reviews/`.

**Interfaces:** Preserve Task 2's generation-safe asynchronous course launch, pause cancellation, replay options/ownership, pinned sorting, retained Lua runtime contract, extracted modal ownership, and Settings view/picker lifetimes. Handle P1 findings first within the wave.

- [x] SEL-03 P1: End selector update after synchronous pending-preload gameplay handoff and prevent paused/inactive work-entry points from restarting shared Jukebox staging. Exercise actual update plus pending completion with same-tick repository revision and synchronous table/Hash restoration; assert no post-handoff preload, publication, or input consumption.
- [x] SEL-05 P1: Synchronize native MainMenu and Records modal busy flags on AutoPlay and local chart/course/remote result recall entry/success/cancel/failure and replay export entry/completion. Clear load state before successful hide, and block hide during exports. Exercise actual owner callbacks and return paths, not modal callback doubles alone.
- [x] SEL-01 P2: Records `MusicSelectScene::launchAutoPlay` must require successful audio staging and no cancellation; keep failure modal recoverable, preserve ownership/options, and test failure/cancel/retry through the actual callback/body.
- [x] SEL-02 P2: Apply pinned installed-before-unavailable and two-unavailable equality for ARTIST/BPM/LENGTH/LEVEL only. Preserve TITLE and other sorts. Test both input orders and stable authored ordering, using the pinned comparator rather than only shared eager/compact/SQL parity.
- [x] SEL-04 P2: Unchanged Settings round-trip retains the same Lua runtime and its sentinel/custom state. Compare applicable activation identity/revision/configuration before replacing; changed skin/configuration still activates correctly. Test unchanged runtime epoch alongside changed settings.
- [x] SEL-07 P2: Clear sound-set input view pointers on Settings layout teardown; late picker success updates settings without touching destroyed controls when tabs/layout change. Test another tab, same-tab rebuild, and cancellation with gated publication and live/deleted control detection.
- [x] Establish behavioral RED/GREEN, run focused tests plus main, freeze and prepare separate logical index patches for controller commits. Independent selector re-review must verify these six and Task 7's separate SEL-06 delta. G7/G8 remain Task 3 work, not defects to silently omit from final convergence.

### Task 9: Android integration correction

**Evidence:** Complete Firebase-release build-only verification fails at `AsoBMaShowActivity.java:202`: `onResume()` still calls the permission-latch helper removed by Task 6. Host fixtures did not extract that lifecycle entry point.

**Files:** Activity and `NativeFolderPickerRequests` helper, plus the existing host-JVM lifecycle fixture/tests. Do not change unrelated selector or rendering code.

- [x] Correct the actual permission-resume path, preserving grant/deny completion when no Activity result is delivered. Do not complete unlaunched, non-permission, cancelled, destroyed, or stale requests.
- [x] Add actual `onResume()` compilation/behavior to the regression fixture. Preserve existing cancellation, destruction, late-dispatch, user-cancel and successful-result coverage; do not add a no-op stub merely to compile.
- [x] Run lifecycle regressions and the full Android `--build-only` compile, freeze, commit this logical fix, and independently review the complete affected delta.

### Task 10: Commit accepted volume writes before handoff

**Evidence:** Selector round 3 approves G7/G8 and the multi-Play P1 fix, but reports SEL-R3-01: a volume action mutates settings before Play; synchronous pause then skips the deferred runtime apply/save.

- [x] Finalize accepted audio changes before a handoff-capable event, while retaining ownership guards and discarding trailing actions after pause.
- [x] Extend the real action-loop/ready-preload/pause regression with observable apply/save ordering for all three channels, unchanged/consecutive writes, trailing write/Play suppression and resumed launch.
- [x] Run regression RED/GREEN and focused verification, commit separately, then independently re-review until no actionable finding remains.

## Completion Evidence

- All eight GitHub findings are corrected: callback dispatch (`08dd9d4c`), volume validation (`532409b4`), short cache names (`ecb9c465`), pause cancellation (`132557bb`), replay audio rejection (`68ac39a0`), asynchronous course staging (`0be03a5e`), failed-page recovery (`06a7f551`), and bounded search (`f51a6a87`).
- Independent whole-PR reviews and corrective re-reviews converge to zero remaining actionable PR findings at code freeze. Coverage is exactly 413 changed paths: 117 skin/render/audio, 166 selector/repository and 130 platform/library paths, with no unassigned or overlapping paths. Unchanged paths retain their reviewed baseline; every subsequent changed path is re-reviewed.
- Additional review/integration fixes include decoded-image admission, graph resources/slicing, locale/timer compatibility, runtime kerning, bounded archive fallbacks and gameplay batches, same-path refresh, Android picker lifecycle, selector/Records ownership, Settings retention, and pre-handoff audio commits. Each logical fix is committed separately; reviewers do not perform their own fixes.
- Fresh all-target desktop build: `cmake --build cmake-build-debug -j 6` passes. Fresh unrestricted `ctest --test-dir cmake-build-debug --output-on-failure -j 6`: **335/335 pass**, 79.18 seconds.
- Unsigned iOS: `scripts/ios_firebase_deploy.sh --build-only --skip-init` passes. Android Firebase release: `scripts/android_firebase_deploy.sh --build-only` passes, including complete Java/JNI/NDK integration; final build reports 38 seconds. No distribution upload or explicit Android version-code override occurs.
- The first full CTest run was 334/335: one renderer trace-characterization test failed, then passed alone, in 20 consecutive isolated repeats and in the final full run. Triage identifies pointer-sensitive ordering already present before the PR merge base; the original log lacks differing JSON values, so exact failure attribution is not proven. No unrelated renderer code or golden data is changed, and reruns are not claimed to fix that nondeterminism.
- Source review and host fixtures do not establish physical-device/OEM lifecycle behavior, native Windows execution or sanitizer coverage. Those limits remain explicit.
