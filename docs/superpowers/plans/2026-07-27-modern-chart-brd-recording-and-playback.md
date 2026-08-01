# Modern Chart BRD Recording and Playback Implementation Plan

> Execute this plan inline on `feature/file-based-replays-v2`. Apply TDD to
> every task and commit each independently reviewable boundary.

**Goal:** Activate raw logical-input BRD recording and every chart replay
consumer while persisting compact chart results, postponed IR snapshots, and
file ownership independently and atomically.

**Architecture:** Add schema v11 beside legacy schema v10, introduce a bounded
input recorder and shared chart replay context, and route chart completion
through a recoverable BRD/file/reference coordinator. Keep courses and legacy
reads unchanged until their later slices.

**Tech stack:** C++23, SQLite, nlohmann JSON, gzip BRD codec, SDL input/runtime,
CMake/CTest.

---

### Task 1: Pin the chart persistence and input contracts

**Files:**

- Create: `tests/replay_input_recorder_tests.cpp`
- Create: `tests/modern_chart_persistence_contract_tests.cpp`
- Modify: `tests/logical_gameplay_input_tests.cpp`
- Modify: `tests/replay_contract_boundary_tests.cpp`
- Modify: `CMakeLists.txt`

1. Add failing tests for monotonic signed-time capture, accepted-transition
   observation, scratch handoff fidelity, Start/Select mapping, redundant input
   suppression, hard count failure, and permanent invalidation after reversal
   or truncation.
2. Add failing source/schema contracts proving a chart write owns no legacy
   replay/event/touch/lane-cover row and result/IR reads do not access BRD
   bytes.
3. Build and run the new tests to record the expected failures.
4. Commit the red contract boundary before production implementation.

### Task 2: Implement the shared bounded input recorder

**Files:**

- Create: `src/replay/ReplayInputRecorder.h`
- Create: `src/replay/ReplayInputRecorder.cpp`
- Modify: `src/input/LogicalGameplayInputAdapter.h`
- Modify: `src/input/LogicalGameplayInputAdapter.cpp`
- Modify: `src/input/RealtimePhysicalInputRouter.h`
- Modify: `src/input/RealtimePhysicalInputRouter.cpp`
- Modify: `src/scene/play/RealtimeGameplayWorker.h`
- Modify: `src/scene/play/RealtimeGameplayWorker.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`

1. Implement action mapping and append-only recorder validation using
   `ReplayLimits` and `ReplayTimeBounds`.
2. Add an accepted-transition observer to the logical adapter and emit exact
   effective scratch ownership handoffs.
3. Route ordinary and realtime logical transitions through that observer.
4. Run recorder, logical-input, and realtime focused tests, then commit.

### Task 3: Add schema-v11 modern chart storage

**Files:**

- Create: `src/repositories/ReplayRepositoryModernResults.cpp`
- Create: `tests/replay_repository_modern_chart_tests.cpp`
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepositoryInternal.h`
- Modify: `src/repositories/ReplayRepository.cpp`
- Modify: `src/repositories/ReplayRepositorySchema.cpp`
- Modify: `src/repositories/ReplayRepositoryIrOutbox.cpp`
- Modify: `src/ir/IrReceiptModels.h`
- Modify: `src/ir/IrReceiptModels.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`

1. Add failing repository tests for strict result/snapshot/reference rows,
   exact retry idempotency, conflicting retry rejection, transaction rollback
   fault points, optional replay attachment, ready outbox activation, receipt
   ownership, and zero new legacy raw rows.
2. Add additive schema-v11 tables and constraints. Rebuild receipt ownership so
   exactly one legacy replay or modern result may own a receipt.
3. Implement one repository transaction that validates and stores a modern
   chart result, optional canonical snapshot/outbox drafts, and optional replay
   file metadata.
4. Implement strict modern result, snapshot, and reference readers plus chart
   history listing. Canonically deserialize and revalidate every durable
   payload.
5. Run repository, persistence, IR receipt/outbox, provenance, and source
   boundary tests, then commit.

### Task 4: Implement recoverable BRD chart persistence

**Files:**

- Create: `src/replay/ChartReplayPersistence.h`
- Create: `src/replay/ChartReplayPersistence.cpp`
- Create: `tests/chart_replay_persistence_tests.cpp`
- Modify: `src/ResultPersistenceCoordinator.h`
- Modify: `src/ResultPersistenceCoordinator.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`

1. Add failing fault-injection tests for encode, reserve, temporary write,
   rename, installed-byte verification, database stage, acknowledgement,
   exact retry, occupied history slot, and safe cleanup.
2. Implement deterministic stem/history reservation and the install-before-DB
   state machine using `ReplayFileStore` and schema-v11 reservations.
3. Validate canonical setup, chart identity, time bounds, result/setup shared
   facts, snapshot/result agreement, and installed metadata before association.
4. Preserve the result and eligible IR snapshot when replay capture is absent
   or invalid, and never create a replay reference in that case.
5. Run persistence and file-store focused tests, then commit.

### Task 5: Capture and persist live chart attempts

**Files:**

- Modify: `src/context.h`
- Modify: `src/scene/play/GamePlayScene.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/scene/play/RealtimeTouchInputRouter.h`
- Modify: `src/scene/play/RealtimeTouchInputRouter.cpp`
- Modify: `src/ScoreProvenance.h`
- Modify: `src/ScoreProvenance.cpp`
- Modify: `tests/gameplay_playback_startup_tests.cpp`
- Modify: `tests/realtime_touch_input_router_tests.cpp`
- Modify: `tests/score_provenance_tests.cpp`

1. Add failing tests for canonical setup capture across supported key modes,
   LN modes, random/lane options, DP FLIP, gauges/rulesets, playback rate,
   initial lane cover, touch actions, and timed lane-cover changes.
2. Add DP FLIP to the versioned provenance/setup agreement boundary with
   backward-compatible older provenance decoding.
3. Build `ReplayChartDocument`, `ModernChartResult`, and IR snapshot directly
   at chart completion; call modern BRD persistence for new chart attempts.
4. Keep course completion on its existing path for Slice 5 and assert the two
   routes explicitly.
5. Run gameplay, provenance, result, codec, and integration tests, then commit.

### Task 6: Add one verified chart replay context and raw driver

**Files:**

- Create: `src/replay/ChartReplayContext.h`
- Create: `src/replay/ChartReplayContext.cpp`
- Create: `src/replay/ReplayPlaybackDriver.h`
- Create: `src/replay/ReplayPlaybackDriver.cpp`
- Create: `src/replay/ReplayPlaybackMaterializer.h`
- Create: `src/replay/ReplayPlaybackMaterializer.cpp`
- Create: `tests/chart_replay_context_tests.cpp`
- Create: `tests/replay_playback_driver_tests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`

1. Add failing tests for verified, missing, corrupt, unsafe, wrong chart,
   wrong LN mode, wrong key mode, wrong shared result facts, future extension,
   and bounded materialization time.
2. Implement the load order: result, parsed chart identity/time bounds,
   reference, verified bytes, decode, setup validation, shared-fact agreement.
3. Implement a monotonic raw driver that feeds transitions into the logical
   gameplay adapter and preserves touch/lane-cover timing.
4. Implement judged materialization on the same driver and compare, never
   derive, saved result facts.
5. Run context, driver, codec, gameplay, and result tests, then commit.

### Task 7: Route all chart consumers through the context

**Files:**

- Modify: `src/scene/MainMenuScene.h`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `src/scene/ChartViewerScene.h`
- Modify: `src/scene/ChartViewerScene.cpp`
- Modify: `src/scene/ResultScene.h`
- Modify: `src/scene/ResultScene.cpp`
- Modify: `src/practice/PracticeLaunchRequest.h`
- Modify: `src/practice/PracticeLaunchRequest.cpp`
- Modify: `src/ReplayVideoExporter.h`
- Modify: `src/ReplayVideoExporter.cpp`
- Modify: `tests/replay_capabilities_tests.cpp`
- Modify: `tests/practice_launch_tests.cpp`
- Modify: `tests/gbattle_tests.cpp`
- Modify: `tests/result_recall_builder_tests.cpp`

1. Add failing matrix tests for View Result independence and Watch, Retry Same,
   G-Battle, practice ghost, and video availability for verified versus
   missing/corrupt/mismatched BRDs.
2. Change modern chart result recall to use strict result rows only.
3. Route every replay-dependent chart action through `ChartReplayContext`;
   remove consumer-local file loading and setup translation.
4. Keep explicit legacy chart behavior unchanged until Slice 7.
5. Run all chart consumer, result recall, practice, video, and IR tests, then
   commit.

### Task 8: Slice review and gate

**Files:**

- Modify: `docs/replay/file-replay-contract-matrix.md`
- Modify tests at any shared boundary implicated by review findings.

1. Run every Slice 4 focused test and `cmake --build cmake-build-debug --target main -j 6`.
2. Review `git diff origin/develop...HEAD` by producer/consumer invariant:
   setup, limits/time, chart identity, result agreement, file ownership, and
   replay availability.
3. For each duplicate authority or asymmetry, add a failing regression at the
   shared boundary, fix the shared implementation, and rerun focused tests.
4. Update the contract matrix with activated chart cells and commit the Slice 4
   review fixes. Continue directly to Slice 5.
