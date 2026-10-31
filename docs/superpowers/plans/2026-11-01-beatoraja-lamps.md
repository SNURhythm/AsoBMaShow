# Beatoraja Lamps Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Support Perfect and Max across native results and proven historical records.

**Architecture:** Extend existing lamp ranks and shared promotion policy. Keep canonical facts immutable while deriving effective lamps for projections and presentation.

**Tech Stack:** C++20, SQLite, CMake/CTest.

**Spec:** `docs/superpowers/specs/2026-11-01-beatoraja-lamps-design.md`

## Global Constraints

- Perfect = 700, Max = 800; 150 remains reserved.
- Authenticated facts and fingerprints remain unchanged.
- Work in the existing checkout on `feature/more-lamps`; no deployment.

## Review Focus

- Missing historical judgments must not imply Max.
- Course stages must not borrow carried combo as judgment evidence.
- Cache refresh and recovery must retain stronger derived lamps.
- Imported aggregate rows must not be promoted from unrelated judgments.
- Provider ceilings and inclusive FC achievements must retain their meaning.

## Tasks

### 1. Native lamp policy and awards

Files: `src/scene/play/GameplayGaugeTypes.h`, `src/scene/play/GameplayScoreState.h`, `src/ResultPresentationUtils.h`, `src/ResultPersistenceModel.cpp`, `src/scene/ResultScene.cpp`, `src/scene/play/GamePlayScene.cpp`, `src/ReplayAutoPlay.h`, `src/replay/CourseReplayConsumer.cpp`.

- [x] Extend gameplay/result tests with Perfect, Max, GOOD/failed/assisted and incomplete cases; observe failure.
- [x] Add ranks, labels and shared judgment-based promotion using positive consistent totals, then connect chart/course capture paths.
- [x] Verify gameplay and result tests.

### 2. Historical projections and contracts

Files: `src/ReplayClearMarkUtils.h`, `src/ResultContracts.h`, `src/repositories/ScoreCacheQueries.h`, score/replay repository schema and queries, result recall/presentation paths.

- [x] Extend score-cache and result-summary tests for proven upgrades, missing evidence and immutable originals; observe failure.
- [x] Accept new ranks and derive effective lamps from per-attempt judgments, preserving canonical payloads. Refresh caches transactionally with an idempotent repair marker.
- [x] Verify results/replay tests, including recovery and course projections.

### 3. Presentation and provider boundaries

Files: `src/BeatorajaClearType.h`, `src/view/ClearLampColors.h`, result/select skin mappings, record and chart filters, `src/ir` adapters and rank validators.

- [x] Extend existing mapping/filter/provider tests to distinguish ranks 700/800 and IDs 9/10; observe failure.
- [x] Add distinct choices and display mappings; preserve inclusive FC achievements and map to supported provider ceiling.
- [x] Verify affected IR, music-select and skin-runtime tests.

### 4. Verification and delivery

- [x] Build affected groups and desktop `main` sequentially per build directory using `-j 6`; run broader suite for shared-contract coverage.
- [x] Obtain a fresh whole-branch review, address substantive findings and rerun affected checks.
- [ ] Commit, push and open/link the PR.

## Implementation decisions and review

- Reuse the score-summary revision marker (revision 4) instead of rewriting
  authenticated result rows. Course queries derive their lamps from available
  aggregate evidence; ambiguous historical assist setup blocks promotion.
- Re-judging an old replay retains its saved lamp representation when effective
  results agree, preserving existing result fingerprints and compatibility
  evidence. New captures store the new ranks directly.
- The independent review found missing course assist propagation and a ranking
  comparison that still used the gauge lamp. Both paths were corrected; a
  regression test covers an assisted earlier stage followed by a Max stage.
- Tachi submissions and receipt matching share its Full Combo ceiling. Original
  historical snapshot bytes and fingerprint validation remain authoritative.

## Final verification

- `cmake --build cmake-build-debug -j 6`: passed, including desktop `main`.
- `ctest --test-dir cmake-build-debug --output-on-failure -j 6`: 547/548
  passed. The only failure is the unchanged `ui_scale_tests` rotation fixture:
  its extracted event loop references `SDL_EVENT_WINDOW_METAL_VIEW_RESIZED`,
  which the fixture does not declare. Both the reference and the missing
  declaration are present at the branch base; no lamp changes touch them.
- Historical replay compatibility, original fingerprint preservation, course
  assist precedence, score-cache migration, provider snapshots, and Quick Retry
  regression checks passed in that run.
