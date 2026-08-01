# PR 77 New Review Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Resolve the four actionable review threads added to PR #77 after commit `7a17937241` without weakening new-score ruleset validation.

**Architecture:** Close the LR2 long-note identity at the point where its head receives the batched BAD, make synthetic Auto Play state explicitly ruleset-aware, isolate the old-replay fallback at the replay-start boundary, and make foreground activation synchronously reconcile abandoned outbox claims before the worker resumes. Each subsystem gets a focused regression test before its production change.

**Tech Stack:** C++23, SQLite, CMake, CTest.

## Global Constraints

- Preserve result-screen BREAK as BAD + POOR without KPOOR.
- Preserve strict descriptor and complete policy-snapshot requirements for newly recorded replays.
- Legacy replay fallback uses Beatoraja behavior because legacy replays predate selectable rulesets.
- Do not copy API keys into outbox rows, logs, diagnostics, or caches.
- Do not reply to or resolve GitHub review threads, push, merge, deploy, or upload as part of this plan.

---

### Task 1: Consume LR2 multi-BAD long-note pairs

**Files:**
- Modify: `tests/gameplay_simulation_tests.cpp`
- Modify: `src/scene/play/GameplaySimulation.cpp`

**Interfaces:**
- Consumes: `GameplaySimulation::multiBadSourceIndices_`, `clearPairHolding(NoteId)`, and `markMissed(NoteId, std::int64_t, bool)`.
- Produces: an LR2 multi-BAD on a long-note head that records only the head BAD and resolves its paired tail without a later automatic POOR.

- [x] **Step 1: Write the failing long-note regression test**

Add `testLr2MultiBadLongHeadConsumesTail()` beside `testLr2MultiBadBatchAndFixedSelection()`. Build a classic LN from 800,000 to 1,800,000 microseconds plus a selectable normal note at 950,000, press at 1,000,000, and assert the first transaction is BAD on the head while both head and tail become played. Advance beyond the tail late-POOR boundary and assert no extra transaction or POOR count appears.

- [x] **Step 2: Run the test to verify RED**

Run: `cmake --build cmake-build-debug --target gameplay_simulation_tests -j 6 && ctest --test-dir cmake-build-debug -R '^gameplay_simulation_tests$' --output-on-failure`

Expected: FAIL because the head is consumed but its tail remains playable and later produces POOR.

- [x] **Step 3: Resolve the paired tail when applying batched BAD**

After marking a multi-BAD head played, detect `NoteKind::LongHead`, call `clearPairHolding(multiBadId)`, and, when the pair is valid, allowed, and not already played, call `markMissed(pairId, judgedTime, false)`. Do not call `commitMiss` for the tail.

- [x] **Step 4: Run the test to verify GREEN**

Run the command from Step 2.

Expected: PASS.

### Task 2: Propagate the selected ruleset through synthetic Auto Play

**Files:**
- Modify: `tests/replay_summary_list_tests.cpp`
- Modify: `src/ReplayAutoPlay.h`
- Modify: `src/scene/MainMenuScene.cpp`

**Interfaces:**
- Consumes: `GameplayRuleset`, `RulesetDescriptor::For(...)`, and the ruleset-aware `RhythmState` constructor.
- Produces: `BuildSummary(..., GameplayRuleset ruleset)` and `BuildReplayData(..., GameplayRuleset ruleset, ...)` whose gauge calculation and replay provenance use the same selected ruleset as Auto Play gameplay.

- [x] **Step 1: Write failing Auto Play ruleset tests**

Extend the existing synthetic Auto Play assertions to build LR2 and Beatoraja summaries for a chart whose authored TOTAL distinguishes the gauge algorithms. Assert the values differ as direct `RhythmState` simulations do. Build LR2 replay data and assert `provenance.ruleset == RulesetDescriptor::For(GameplayRuleset::LR2)` and its final gauge matches the LR2 summary.

- [x] **Step 2: Run the test to verify RED**

Run: `cmake --build cmake-build-debug --target replay_summary_list_tests -j 6 && ctest --test-dir cmake-build-debug -R '^replay_summary_list_tests$' --output-on-failure`

Expected: FAIL because both builders currently instantiate default Beatoraja state and replay data retains legacy ruleset provenance.

- [x] **Step 3: Make both builders ruleset-aware**

Add a `GameplayRuleset` argument to `perfectPlayGauge`, `BuildSummary`, and `BuildReplayData`; construct `RhythmState(&chart, false, ruleset)`; and set `replay.provenance.ruleset = RulesetDescriptor::For(ruleset)`. Pass `profileSelections.ruleset` from `autoPlayReplaySummary`, capture it in `startReplayVideoExport`, and pass it to `BuildReplayData`.

- [x] **Step 4: Run the test to verify GREEN**

Run the command from Step 2.

Expected: PASS.

### Task 3: Preserve legacy replay playback

**Files:**
- Modify: `tests/gameplay_ruleset_policy_tests.cpp`
- Modify: `src/scene/play/GamePlayStartOptions.h`
- Modify: `src/CoursePlaySession.h`

**Interfaces:**
- Consumes: `ScoreProvenance::Legacy()`, `RulesetDescriptor::Legacy()`, and `buildGameplayRulesetPolicyAtPlayStart(...)`.
- Produces: legacy single-chart and course replays that compile a canonical Beatoraja policy from current chart metadata without a recorded snapshot; supported modern descriptors still require a matching complete snapshot.

- [x] **Step 1: Write a failing migrated-replay test**

Create a replay with `ScoreProvenance::Legacy()`, attach it to `StartOptions`, call `applyReplayProvenanceToStartOptions`, then build the start policy. Assert the result is built with the canonical Beatoraja descriptor, including when the legacy provenance contains an otherwise valid stage snapshot. Repeat through `CoursePlaySession::snapshotRulesetFromReplay(...)`. Keep the existing incomplete LR2 replay assertion to prove modern replay validation remains strict.

- [x] **Step 2: Run the test to verify RED**

Run: `cmake --build cmake-build-debug --target gameplay_ruleset_policy_tests -j 6 && ctest --test-dir cmake-build-debug -R '^gameplay_ruleset_policy_tests$' --output-on-failure`

Expected: FAIL with unsupported ruleset or missing replay snapshot.

- [x] **Step 3: Add the narrow legacy fallback**

In `applyReplayProvenanceToStartOptions`, recognize exactly `RulesetDescriptor::Legacy()`, select `GameplayRuleset::Beatoraja`, require its canonical descriptor, and leave `replayRulesetOverride` empty. Apply the same runtime normalization when a course snapshots its replay ruleset. In the post-build snapshot gate, exempt only replay data carrying the exact legacy descriptor; modern and unknown/future descriptors continue to fail.

- [x] **Step 4: Run the test to verify GREEN**

Run the command from Step 2.

Expected: PASS.

### Task 4: Recover abandoned IR claims on foreground activation

**Files:**
- Modify: `tests/ir_submission_service_tests.cpp`
- Modify: `src/ir/IrSubmissionService.cpp`

**Interfaces:**
- Consumes: `ReplayRepository::RecoverStaleIrOutbox(...)`, `Impl::loadProfileSnapshots(...)`, and the existing worker cancellation synchronization.
- Produces: an inactive-to-active transition that waits for any cancelled request to leave the worker, recovers remaining `Uploading` rows, refreshes cached status/count snapshots without consuming credential-change detection, and only then resumes delivery. A lifecycle recheck prevents replacing a cancelled request source immediately before I/O.

- [x] **Step 1: Write a failing foreground recovery test**

Start a disabled provider profile, claim an existing pending row directly so it remains `Uploading`, background and foreground the service, then assert the repository row and service snapshot are both `Pending` with the request-attempt count preserved. Add a background credential-change case that must unblock on foreground, plus a deterministic credential-lookup barrier proving lifecycle cancellation wins before a new request starts.

- [x] **Step 2: Run the test to verify RED**

Run: `cmake --build cmake-build-debug --target ir_submission_service_tests -j 6 && ctest --test-dir cmake-build-debug -R '^ir_submission_service_tests$' --output-on-failure`

Expected: FAIL because foreground activation currently only signals the worker.

- [x] **Step 3: Reconcile before resuming the worker**

Keep the app inactive while requesting cancellation and waiting for `workerBusy == false`; recover stale rows at `safeNow`, refresh profile status/count snapshots for the same generation without replacing credential fingerprints, then set `applicationActive = true` and signal. Recheck generation, stopped/paused state, and application activity after credential lookup and again after claiming a row but before replacing the request stop source. Preserve the non-blocking background path.

- [x] **Step 4: Run the test to verify GREEN**

Run the command from Step 2.

Expected: PASS.

### Task 5: Final verification

**Files:**
- Verify all files changed by Tasks 1-4.

**Interfaces:**
- Consumes: all four fixes.
- Produces: a locally review-ready PR branch.

- [x] **Step 1: Build the desktop target**

Run: `cmake --build cmake-build-debug --target main -j 6`

Expected: successful build.

- [x] **Step 2: Run the complete test suite**

Run: `ctest --test-dir cmake-build-debug --output-on-failure -j 6`

Expected: all tests pass.

- [x] **Step 3: Inspect scope**

Run: `git diff --check && git status --short && git diff --stat`

Expected: no whitespace errors and only the plan plus intended source/test files are changed.
