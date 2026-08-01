# Modern Course BRD Recording and Playback Implementation Plan

> **For agentic workers:** Execute this plan inline on
> `feature/file-based-replays-v2`. Use the
> `superpowers:test-driven-development` skill task by task and
> `superpowers:verification-before-completion` at the slice gate. Commit each
> independently reviewable boundary.

**Goal:** Activate complete and partial modern course result persistence,
Beatoraja-layout course BRDs, replay-independent result recall, and the Watch,
Retry Same, and video consumers through one explicit continuation contract.

**Architecture:** Add schema-v12 course result/stage/entry rows and
exclusive chart-or-course replay ownership. Introduce a pure carried-state
transition used by live completion and replay materialization, factor the
chart file lifecycle into a shared association coordinator, and expose one
strict course replay context/consumer pipeline. Keep legacy runtime adapters
temporary until Slice 7 and defer file actions/profile workflows to Slice 6.

**Tech stack:** C++23, SQLite, nlohmann JSON, gzip BRD codec, SDL
gameplay/runtime, CMake/CTest.

**Global constraints:** Never write or migrate the original
`~/Downloads/profiles`; use only generated fixtures or temporary copies. Do
not deploy. Do not reconstruct legacy detail or write new legacy replay,
event, touch, lane-cover, or course-stage rows. Do not inspect or reuse PR #82
implementation until a failing contract test requires the specific behavior.

---

### Task 1: Pin course continuation and persistence boundaries

**Files:**

- Create: `tests/course_continuation_tests.cpp`
- Create: `tests/modern_course_persistence_contract_tests.cpp`
- Modify: `tests/replay_contract_boundary_tests.cpp`
- Modify: `tests/replay_playback_tests.cpp`
- Modify: `CMakeLists.txt`

1. Add failing tests for initial state, contiguous stage advancement,
   cumulative score/max-score overflow safety, carried combo/course max
   combo, all gauge values and adopted gauge, mixed per-stage setup, complete
   and partial prefixes, repeated charts, and one-hour inclusive rest bounds.
2. Add failing source/schema tests proving a new course write cannot reach
   legacy replay/event/touch/lane-cover/course-stage storage, result recall
   cannot open BRD bytes, and file ownership is exactly one chart or course
   result.
3. Strengthen course playback validation tests for source/time-bound shape,
   empty/oversized courses, rest vector agreement, and negative or excessive
   rest.
4. Build and run the new tests to record the expected failures, then commit
   the red contracts before production code.

### Task 2: Implement the shared course-continuation model

**Files:**

- Create: `src/replay/CourseContinuation.h`
- Create: `src/replay/CourseContinuation.cpp`
- Modify: `src/CoursePlaySession.h`
- Modify: `src/replay/ReplayPlayback.cpp`
- Modify: `src/replay/ReplayLimits.h`
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`

1. Implement immutable initial-state construction and one-stage transitions
   with checked arithmetic, exact stage ordering, full gauge snapshot
   validation, normalized constraints, canonical per-stage setup, and shared
   `ReplayLimits` rest validation.
2. Replace direct `CoursePlaySession` carried gauge/combo/rest mutation with
   accessors that delegate to the continuation state. Keep legacy compatibility
   reads only where Slice 7 has not yet cut them over.
3. Make course playback validation use the same rest helper and stage-count
   authority; do not clamp invalid persisted or decoded input.
4. Run the continuation, playback, setup, limits, gameplay authority, and
   course session tests, then commit.

### Task 3: Capture canonical live course attempts

**Files:**

- Create: `src/replay/CourseReplayCapture.h`
- Create: `src/replay/CourseReplayCapture.cpp`
- Create: `tests/course_replay_capture_tests.cpp`
- Modify: `src/ModernResult.h`
- Modify: `src/ModernResult.cpp`
- Modify: `src/CoursePlaySession.h`
- Modify: `src/scene/play/GamePlayScene.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`

1. Add failing tests that capture complete, failed partial, repeated-chart,
   and mixed-setup attempts directly from stage result facts plus raw replay
   data, including exact entry facts and aggregate fingerprint.
2. Add one canonical `captureModernCourseResult` builder with checked totals,
   contiguous completed stages, merged provenance, final aggregate facts, and
   deterministic fingerprinting.
3. At each live stage completion, finalize the existing raw logical recorder,
   capture that stage's canonical setup/time bounds, and advance the shared
   continuation. Never construct replay input from judged legacy events.
4. Build a `ReplayCourseDocument` only from the accepted completed prefix and
   bounded rest values. If raw capture is invalid, retain the result capture
   and omit only the replay.
5. Run course capture, modern result, recorder, setup/provenance, and gameplay
   tests, then commit.

### Task 4: Add schema-v12 modern course storage

**Files:**

- Create: `tests/replay_repository_modern_course_tests.cpp`
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepositoryInternal.h`
- Modify: `src/repositories/ReplayRepositoryModernResults.cpp`
- Modify: `src/repositories/ReplayRepositorySchema.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`

1. Add failing repository tests for strict complete/partial rows, repeated
   stages, ordered entry facts, optional replay attachment, exact retry,
   conflicting retry, malformed durable payload rejection, transaction fault
   rollback, and zero new legacy raw rows.
2. Add strict course result, completed-stage, and entry tables. Transactionally
   rebuild `modern_replay_files` so exactly one chart or course result owns
   each file while preserving every existing chart reference.
3. Implement one course staging transaction and strict readers by attempt,
   result ID, and course-key history. Canonically deserialize and revalidate
   every row set and reject gaps, extras, duplicate stage indices, or aggregate
   disagreement.
4. Re-run schema upgrades from v10 and v11, foreign-key checks, rollback fault
   injection, modern chart repository regression tests, and the new course
   tests, then commit.

### Task 5: Share the file association state machine and persist course BRDs

**Files:**

- Create: `src/replay/ReplayFileAssociationCoordinator.h`
- Create: `src/replay/ReplayFileAssociationCoordinator.cpp`
- Create: `src/replay/CourseReplayPersistence.h`
- Create: `src/replay/CourseReplayPersistence.cpp`
- Create: `tests/replay_file_association_coordinator_tests.cpp`
- Create: `tests/course_replay_persistence_tests.cpp`
- Modify: `src/replay/ChartReplayPersistence.h`
- Modify: `src/replay/ChartReplayPersistence.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`

1. Add failing fault-injection tests shared by chart and course for encode,
   reserve, temporary write, atomic install, verification, occupied slot,
   database stage, ambiguous acknowledgement, exact retry, and exact-match
   cleanup.
2. Extract reservation/install/inspection/cleanup into one coordinator and
   route existing chart persistence through it without changing chart
   behavior.
3. Implement course persistence using `courseStem`, ordered stage identities,
   undefined-LN/constraint facts, the shared coordinator, and the course
   repository transaction.
4. Validate result/document shape, every setup and identity, result agreement,
   time bounds, rest, and installed metadata before association. Save the
   modern result without a replay reference when replay capture alone is
   unavailable.
5. Run chart/course persistence, path, codec, file-store, repository, and
   coordinator tests, then commit.

### Task 6: Route live course completion to modern persistence

**Files:**

- Modify: `src/context.h`
- Modify: `src/CoursePlaySession.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/scene/ResultScene.h`
- Modify: `src/scene/ResultScene.cpp`
- Modify: `src/repositories/ScoreRepository.h`
- Modify: `src/repositories/ScoreRepositoryQueries.cpp`
- Modify: `tests/gameplay_playback_startup_tests.cpp`
- Modify: `tests/score_provenance_db_tests.cpp`
- Modify: `tests/modern_course_persistence_contract_tests.cpp`

1. Add failing integration tests proving final and failed-partial live courses
   invoke exactly one modern persistence path, save independently recallable
   result facts, optionally attach one BRD, and add no legacy raw rows.
2. Persist the captured course attempt once at the final result and retain its
   typed receipt/state in the session. Remove the new-live call to
   `SaveCourseReplay`; keep it only for legacy/saved adapters until Slice 7.
3. Project course-score history from `ModernCourseResult` rather than from
   replay data, with exact attempt/result agreement and no BRD read.
4. Surface retryable persistence diagnostics without blocking continuation or
   result display, and make exact final-result retries idempotent.
5. Run gameplay, ResultScene, course score/provenance, repository, and
   persistence integration tests, then commit.

### Task 7: Add one verified course context and consumer pipeline

**Files:**

- Create: `src/replay/CourseReplayAgreement.h`
- Create: `src/replay/CourseReplayAgreement.cpp`
- Create: `src/replay/CourseReplayContext.h`
- Create: `src/replay/CourseReplayContext.cpp`
- Create: `src/replay/CourseReplayConsumer.h`
- Create: `src/replay/CourseReplayConsumer.cpp`
- Create: `tests/course_replay_context_tests.cpp`
- Create: `tests/course_replay_consumer_tests.cpp`
- Modify: `src/replay/ReplayPlaybackMaterializer.h`
- Modify: `src/replay/ReplayPlaybackMaterializer.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`

1. Add failing tests for complete/partial/repeated/mixed setup courses and for
   missing, corrupt, unsafe, wrong stage identity/order/LN/key mode,
   mismatched constraints/setup/result, unsupported extension, excessive
   rest, and materialized carried-state disagreement.
2. Implement strict load order: modern result, completed parsed chart prefix,
   ordered identity/time facts, file reference, verified bytes, course decode,
   path/setup/result agreement.
3. Materialize every stage through the raw playback driver and advance the
   shared continuation contract. Compare each stage and aggregate outcome with
   the saved result without deriving saved facts from replay input.
4. Return current-scene compatibility replay objects only after all checks
   succeed; keep those adapters memory-only and explicitly temporary.
5. Run context, consumer, continuation, materializer, codec, and result tests,
   then commit.

### Task 8: Activate modern course Records, recall, Watch, Retry, and video

**Files:**

- Modify: `src/ResultRecordSummary.h`
- Modify: `src/ResultRecordSummary.cpp`
- Modify: `src/ModernResultRecallBuilder.h`
- Modify: `src/ModernResultRecallBuilder.cpp`
- Modify: `src/scene/MainMenuScene.h`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `src/scene/ResultScene.h`
- Modify: `src/scene/ResultScene.cpp`
- Modify: `src/ReplayVideoExporter.h`
- Modify: `src/ReplayVideoExporter.cpp`
- Modify: `tests/result_record_summary_tests.cpp`
- Modify: `tests/result_record_list_view_tests.cpp`
- Modify: `tests/modern_result_recall_tests.cpp`
- Modify: `tests/replay_capabilities_tests.cpp`

1. Add failing capability/list tests for verified, missing, corrupt,
   mismatched, and unsupported modern course BRDs. Confirm View Result always
   survives while Watch, Retry Same, and video require verified bytes, and
   chart-only G-Battle/practice ghost remain absent.
2. Add a stable modern course record identity and list course history by
   durable course key, merged newest-first with temporary legacy rows.
3. Route modern course View Result through strict result rows only. Route
   Watch, Retry Same, and video through `CourseReplayConsumer`; remove their
   consumer-local modern file/setup/continuation logic.
4. Preserve missing/corrupt diagnostics without hiding modern result history.
   Defer share/delete buttons and profile workflows to Slice 6.
5. Run all course consumer, Records, result recall, video, capabilities, and
   chart consumer regression tests, then commit.

### Task 9: Slice review and gate

**Files:**

- Modify: `docs/replay/file-replay-contract-matrix.md`
- Modify tests at any shared boundary implicated by review findings.

1. Run every Slice 5 focused contract, repository, persistence, continuation,
   consumer, result, codec, gameplay, and video test plus desktop `main`.
2. Review `git diff origin/develop...HEAD` by course invariant: setup,
   limits/time/rest, ordered identity, result agreement, continuation, file
   ownership, and replay availability.
3. For every asymmetric producer/consumer check or duplicate authority, add a
   failing regression at the shared boundary, fix the boundary, and rerun the
   focused gate.
4. Update the contract matrix with activated modern course cells and commit
   the Slice 5 review fixes. Continue directly to Slice 6.
