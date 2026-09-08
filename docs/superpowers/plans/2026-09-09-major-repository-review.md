# Major Repository Review and Remediation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development for each fix wave and superpowers:requesting-code-review for independent review. Continue the reviewer–fixer cycle for confirmed major findings; do not manufacture a clean verdict by parking an unresolved major defect.

**Goal:** Apply the verified findings in the September 8 repository review, then independently audit the whole first-party repository for major performance, security and correctness problems and fix confirmed findings.

**Architecture:** Preserve existing gameplay, persistence, import and platform contracts. Reproduce each material defect through production behavior, make the smallest coherent correction, and independently review fixed commits and affected interactions. Frozen source review can run alongside a single source fixer; build ownership remains exclusive.

**Tech Stack:** C++23, SQLite, Lua, FFmpeg/bgfx/SDL, Android Java, CMake/CTest, existing Python/host-JVM regression fixtures.

**Spec:** The user request and `docs/reviews/2026-09-08-repository-review.md`, with its correctness, security, performance and platform detail documents. Those five original review files are retained unchanged as input evidence.

## Global Constraints

- PR #103 is merged into `develop` as `de1c8d45d4fdf50429bc2fbe40b9d990859be7b1`. Work on the explicitly requested new branch `fix/repository-major-review-2026-09-09`, starting at that merge. Do not create a worktree or another branch.
- Verify all 11 supplied findings against this merged baseline. Already-fixed findings need evidence, not duplicate production changes. The current URL opener has an HTTP/HTTPS guard; Android archive copying already has exception cleanup and cancellation, which need reconciliation with the older report.
- New findings must establish a reachable major impact: serious crash/data loss, incorrect durable results, credential or filesystem trust-boundary violation, broken primary workflows, or substantial/unbounded resource consumption. Exclude style, speculative hardening, microoptimizations and isolated test nondeterminism without demonstrated material application impact.
- Audit all first-party application/platform/build integration, not merely this branch's diff. Vendored sources, generated parser amalgamation, binary assets and historical documents are reviewed only at relevant integration/contract boundaries; do not claim a dependency-advisory or full vendored-source audit.
- Do not edit `src/bms_parser.cpp/.hpp` directly. Correct effective-mode/count ordering in application-owned preparation code; parser changes, if actually necessary, require the separate parser repository workflow.
- Use `apply_patch`; no whole-file formatting, unrelated refactors, new inline comments, real credentials in tests, uncontrolled network probes or distribution uploads.
- Use the existing `cmake-build-debug`, builds/test parallelism `-j 6`, and exactly one native/mobile build owner. Fixers do not commit or spawn agents. The controller verifies, commits logical fixes and manages final publication; do not merge the new branch without user direction.
- Inherit the session model. Use medium reasoning for bounded isolated fixes/re-reviews, high for credential/concurrency/persistence integration, and xhigh only where a broad difficult review warrants it.

## Task 1: Credential origin ownership and external URL verification

**Files:** `src/ir/tachi/TachiDriver.cpp`, `src/ir/IrSubmissionService.cpp`, relevant IR configuration/settings/credential interfaces only as needed, `tests/tachi_driver_tests.cpp`, `tests/ir_submission_service_tests.cpp`; existing `src/PlatformOpen.*`, external-action path and tests for verification.

**Interfaces:** Deferred outbox jobs retain their persisted `remoteOrigin` and job ID. Runtime credentials may be used only for their authorized current origin. Preserve cancellation, automatic/manual retry, blocked-job recovery, HTTPS-only authenticated transport and no-repost semantics.

- [x] SEC-04 RED: server A returns a deferred job; configure server B with a distinct dummy B key; make the old job due through automatic polling and manual retry. No HTTP request to A may contain B's credential. Exercise settings/service reactivation and relevant configuration/credential-change ordering, not only a fake driver returning a canned outcome.
- [x] Correct origin/credential ownership before any authenticated HTTP request. Suspend incompatible jobs without rewriting their original destination or silently reposting a score to B; returning to an authorized original configuration must have explicit recoverable behavior.
- [x] SEC-01: verify the current shared opener blocks local/UNC/non-web schemes, embedded NUL/control characters and backslashes before OS dispatch, while valid HTTP/HTTPS still works through the table action path. Add missing regression coverage only if required; do not duplicate the existing guard.
- [x] Run the focused driver/service/external-action suites, freeze, commit the security correction separately and obtain independent review.

```sh
cmake --build cmake-build-debug --target tachi_driver_tests ir_submission_service_tests music_select_external_actions_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -j 6 -R '^(tachi_driver_tests|ir_submission_service_tests|music_select_external_actions_tests)$'
```

## Task 2: Gameplay termination and abort integrity

**Files:** `src/scene/play/GamePlayScene.cpp/.h`, gameplay score/provenance/result/replay contracts only as needed, and the existing practice, Start+Select, score-state, modern-result and course persistence tests/fixtures.

**Interfaces:** Preserve realtime and legacy ownership, practice loop/non-loop completion, actual terminal chart conditions, gauge auto-shift, replay facts, durable clear lamps and course continuation. Abort is not successful stage completion.

- [x] COR-01 RED: actual legacy update with an unfinished manual practice session remains active for several in-range updates, without premature Poor judgements; configured end completes exactly once for loop and non-loop settings.
- [x] Complete practice only at a real range boundary or legitimate terminal state. Keep platform authority policy unchanged unless a separately verified defect requires it.
- [x] COR-02 RED: Start+Select before the first note and midway under Hard, ExHard, Hazard, Normal and auto-shift must not persist success/full-combo lamps or advance an unfinished course stage.
- [x] Represent the aborted outcome consistently through live score, remaining-note accounting, replay/result capture, cache and course continuation. Test persisted/reloaded facts and presentation, not only a local boolean.
- [x] Run focused regressions plus `main`, freeze and prepare separate logical COR-01/COR-02 commits; independent review must cover both terminal paths.

## Task 3: Effective long-note normalization and course facts

**Files:** `src/CoursePlaySession.h`, `src/replay/CourseContinuation.*`, `src/PlayOptionUtils.h`, `src/scene/play/GamePlayScene.*`, `src/scene/ResultScene.cpp`, `src/scene/ChartViewerScene.cpp`, application-owned policy/provenance helpers and existing course/practice/retry tests as needed.

**Interfaces:** Distinguish selected fallback LN interpretation from per-chart authored effective mode. Counts used to compile rules/gauge/provenance and to persist results must describe the prepared chart after range and lane modifiers, without rewriting unrelated library metadata.

- [x] COR-03 RED: selected LN=1, stage authored LNMODE=2 or 3, completed gauge 60 and nonzero combo; the next stage inherits exact live values. Cover first/later affected stages and replay-capture failure without discarding valid live carry.
- [x] COR-04 RED: complete and partially complete two-stage CN/HCN courses with undefined-mode long notes. Capture effective played/unplayed stage counts coherently; modern results validate, persist and reload with the same maximum scores used in play.
- [x] COR-05 RED: initial play and fresh RANDOM/new-pattern or `#RANDOM` retry agree on effective counts, compiled gauge rules, provenance and identical judgement effects. Include fresh viewer practice/autoplay and skin practice range followed by NORMAL/MIRROR/RANDOM; preserve reused-chart and in-game retry paths.
- [x] Normalize at application-owned preparation boundaries before fixed policy/provenance capture, or rebuild both coherently. Do not patch the generated parser to conceal late normalization.
- [x] Run course, policy, practice and persistence regressions plus `main`; freeze and commit each independently meaningful correction, then review cross-entrypoint invariants.

## Task 4: Bounded cancellable Find BMS extraction

**Files:** `src/bms_search/ArchiveSupport.*`, `DownloadedArchiveWorkflow.*`, `DownloadSupport.*`, extraction callback interfaces and `tests/find_bms_download_tests.cpp` with necessary registered integration fixtures.

**Interfaces:** Preserve valid ZIP/libarchive extraction, safe paths, downloaded-archive decision behavior and owned staging cleanup. Cancellation must reach member/chunk extraction, not only the workflow after expansion.

- [x] SEC-02 RED: small compressed fixtures exceed deliberately small injected per-entry/aggregate budgets; cancel during a large member for both miniz and libarchive paths. Measure bytes written and owned staging state rather than attempting disk exhaustion.
- [x] Add explicit declared and streamed expansion admission, overflow-safe aggregate accounting and cancellation checks before allocation/write. Use cancellable miniz streaming rather than its unbounded extract-to-file shortcut.
- [x] Establish documented production limits suitable for chart archives and available staging storage; keep tests independently configurable. Never remove user-owned outputs when cleaning failed staging.
- [x] Preserve valid extraction, integrity failure, unsafe-entry handling and transactional workflow behavior; build/run focused tests plus `main`, freeze and independently review both backend paths.

```sh
cmake --build cmake-build-debug --target find_bms_download_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -j 6 -R '^find_bms_download_tests$'
```

## Task 5: Android archive-copy and legacy HTTP integration

**Files:** `android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowActivity.java`, `ChartImportCopyControl.java`, `android/app/src/main/AndroidManifest.xml`, relevant existing network-policy/host-JVM fixtures and narrowly scoped instrumentation support if required.

**Interfaces:** Preserve successful chart imports, pause/cancel/destruction, permission picker fixes, both Android flavors, HTTPS-origin downgrade rejection and authenticated IR restrictions.

- [x] SEC-03: reconcile the existing exception cleanup and cancellation with the old finding using actual production copy behavior. Repeated read/write/close failures and cancellation must leave no owned partial inbox file. If the suggested copy-budget contract remains absent, enforce a documented archive-specific bounded copy without arbitrarily capping unrelated whole-library folder imports.
- [x] PLAT-01: verify official Android policy and configure the intended legacy HTTP compatibility. Do not relax application redirect or authenticated IR origin checks.
- [x] Exercise allowed HTTP table/archive and HTTP→HTTPS cases, and rejected HTTPS→HTTP, in both flavor policies. Include actual Android application/manifest execution where feasible; host URL-unit tests alone do not establish platform admission.
- [x] Run host lifecycle/copy/network regressions and complete Android `--build-only`; freeze, commit logical fixes and obtain independent platform/security review. No distribution.

## Task 6: Consistent real skin-movie clock

**Files:** `src/skin/beatoraja/SkinMovieCatalog.cpp`, application-owned video/stopwatch integration only if needed, `tests/skin_movie_catalog_types_tests.cpp` and a registered real-adapter regression.

**Interfaces:** The injected stopwatch and VideoPlayer start origin use one source-time convention. Preserve pause, cancellation, backward seeks, frame ownership and existing resource budgets.

- [x] PERF-01 RED: a short real decoded video through the production adapter progresses over at least three wraps, both immediately after loading and after delayed first presentation. Observe source/display timestamps, not only fake-device calls.
- [x] Rebase stopwatch/player origins together for initial presentation, backward seek and loop wrap; preserve forward progression and bounded media ownership.
- [x] Run movie/video/resource/session regressions plus `main`; freeze, commit and independently review the real clock interaction.

## Task 7: Whole-repository major-issue audit and convergence

- [x] Partition all first-party technical paths across independent frozen-source reviewers with explicit coverage ledgers. Include gameplay/persistence/input, security/network/storage, rendering/audio/media/resources and platform/build/UI integration; inspect supporting tests and contracts.
- [x] Supply the 11-item backlog and verified dispositions so reviewers do not rediscover already assigned issues. Review the rest of the repository as well as affected interactions; require concrete triggers, impact, caller-chain evidence and a viable regression for every accepted major finding.
- [x] Fix confirmed major findings in cohesive, single-writer waves with regression RED/GREEN and independent corrective review. Re-review every changed owned path and cross-slice interface; finish with no remaining confirmed major finding, not a claim of defect-free code.
- [x] Preserve original review input documents. Write a new major-review report containing dispositions, newly accepted findings, coverage, commits and honest execution limits.

### Accepted round-one major findings

These frozen-baseline findings meet the major-impact threshold through concrete caller chains. They remain open until bounded production regressions, corrections and independent review complete.

- [x] GAME-01: queued Start+Select Exit destroys realtime authority before the same frame dereferences its touch-router mutex. Fold into Task2 as a separate lifecycle correction and actual scene-queue regression.
- [x] GAME-02: ordinary DP-FLIP play and replay reconstruction persist the flag without swapping chart halves. Task3 follow-on separate logical option-preparation fix; exercise actual selected/preloaded/course/retry/replay/practice paths with asymmetric DP notes, LN ends, scratches, invisible notes and mines, proving exactly-once transformation and truthful durable provenance.
- [x] GAME-03: selector analysis, gameplay visual-model construction and realtime graph accumulation allocate dense arrays proportional to distant chart timing. New graph-memory wave; bounded or sparse representation with preserved ordinary semantics, actual parser/model/selector and simulation regressions, cancellation and no obsolete publication. Replace the existing 100-million-second fixture's unbounded dense allocation with safely bounded evidence; never execute multi-GB repros deliberately.
- [x] SEC-NEW-02: the default IR waiter snapshots its wake revision after inspecting work, losing an intervening enqueue/reconciliation notification indefinitely. Task1 follow-on; use the real condition-variable waiter and a deterministic work-inspection barrier, not the stronger fake waiter or periodic polling.
- [x] PUI-01: Firebase on API28/29 replaces authorized SAF access with a raw storage path without legacy storage permission. Fold into Task5 separately; retain SAF rather than invent direct access, preserve API30+ all-files behavior and Play flavor policy.
- [x] RENDER-01: coroutine resume/wrap argument copying grows the same Lua stack used as the loop bound. Actual current-build evidence establishes protected stack overflow and valid skin/callback rejection, not a demonstrated native crash/hang. Fixed argument boundaries and checked transfers pass real loader/callback resume/wrap/yield regressions with external child timeouts; independent review approved.
- [x] RENDER-02: chart audio rendering allocates dense float and PCM buffers proportional to untrusted chart duration before cancellation. New audio-memory fix wave; overflow-safe frame/byte/work admission or bounded streaming, cancellation before allocation and during work, tails/growth limits, cache cleanup and normal/adjacent-preload regressions.
- [x] SEC-NEW-01: desktop Find BMS GET/POST retains unlimited metadata-response bytes before parsing/status checks. New transport fix wave; bounded actual callbacks, overflow-safe chunk admission, cancellation, explicit errors for oversized success/error/chunked responses.
- [x] PUI-02: iOS Find BMS reads a completed on-disk archive into unrestricted NSData then copies it into a full vector. Same transport integration wave with separate logical commit; native file-preserving bounded/cancellable download API and staged publication, no whole-response buffers, preserve bounded text/binary callers and all platform builds.

### Additional reproduced startup findings

- [x] DATA-01: portable v0 score upgrades fail when consecutive migrations reattach the same chart alias inside their atomic transaction. Connection-owned attachment reuse passes actual public EnsureSchema/rollback/reopen fixtures and independent review; committed `be38026b`.
- [x] DATA-02: bundled Android SQLite has no writable default temporary directory and fresh replay-schema creation fails with SQLITE_IOERR_GETTEMPPATH. Early private-cache initialization passes actual bundled-native initializers in both flavors and permanent target-process tests, with independent review; committed `4e9111e9`. Headless post-startup visual rendering remains uncertified.

Shared interfaces: transport wave touches DownloadSupport after Task4 extraction callbacks; PUI02 native bridge must preserve URL/redirect restrictions and status/cancel reporting. Lua/audio waves are disjoint but still use exclusive native build ownership. Every additional delta requires independent affected-slice re-review; a frozen DE1 audit does not approve later changes.

## Task 8: Final integration and handoff

- [x] Final review FWR-01: close the already-assigned SEC-02 packed/extracted BMS verification boundary with bounded one-member admission, aggregate/entry limits, cancellable read/hash work and failure cleanup; preserve normal match/mismatch/hashless behavior. Passing extraction writer tests alone does not close this boundary. Run one cohesive corrective wave and independent scoped re-review before converged gates.
- [x] Rebuild all desktop targets and run full parallel CTest. Run unsigned iOS and Android compile-only gates after the last relevant source change; never use distribution actions.
- [x] Retain meaningful logs and report any unrelated pre-existing flaky test separately without weakening fixtures. No physical-device, sanitizer or benchmark claim without that execution.
- [x] Verify all major findings and independent reviews are closed, commit logical fixes/documentation, and report the new branch and evidence. Preserve the user's original untracked review documents and do not merge this new branch without further direction.

Final evidence: all-target desktop build and 352/352 CTests; 66 iOS native suites, 88 Python checks, unsigned UIKit arm64 build/artifact audit; current-native Firebase/Play release builds and real target-process platform checks in both flavors. Final whole-branch review plus the eleven-path FWR-01 re-review close all 22 findings. Corrections end at `ac9b48a9`; the new report documents coverage, limits, decisions and preserved original inputs. No new branch integration or distribution was performed.
