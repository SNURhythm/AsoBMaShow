# Authoritative IR Result Metrics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make completed result state the single source of truth for Bokutachi judgement timing and adopted gauge history, including classic LR2 long notes.

**Architecture:** `makeChartResultAttempt()` captures an immutable per-judgement timing snapshot beside the existing score and adopted-gauge snapshots. `makeIrSubmission()` validates and maps those snapshots without reinterpreting replay events; replay is used only upstream to rebuild historical result state and for chart identity metadata.

**Tech Stack:** C++20, existing result-persistence and IR models, CTest, CMake

## Global Constraints

- Preserve the existing convention that `diffMicros == 0` is reported on the early side.
- Exclude PGREAT fast/slow from Bokutachi aggregate FAST/SLOW exactly once.
- Do not add a score-database schema migration or alter existing outbox rows.
- Do not infer optional timing or gauge history from replay events at the IR boundary.

---

### Task 1: Capture authoritative judgement timing

**Files:**
- Modify: `src/ResultPersistenceModel.h`
- Modify: `src/ResultPersistenceModel.cpp`
- Test: `tests/result_persistence_model_tests.cpp`

**Interfaces:**
- Consumes: `RhythmState::judgementFastSlowCount`
- Produces: `result_persistence::ChartJudgementTiming` and `ChartResultAttempt::judgementTiming`

- [ ] **Step 1: Write the failing factory test**

Populate the fixture's per-judgement timing counters, call
`makeChartResultAttempt()`, and assert that `attempt->judgementTiming` contains
the exact PGREAT, GREAT, GOOD, BAD, and POOR fast/slow pairs from the state.

- [ ] **Step 2: Run the focused test to verify it fails**

Run: `cmake --build cmake-build-debug --target result_persistence_model_tests -j 6 && ctest --test-dir cmake-build-debug -R '^result_persistence_model_tests$' --output-on-failure`

Expected: compilation fails because `ChartResultAttempt::judgementTiming` does not exist.

- [ ] **Step 3: Add the snapshot model and factory capture**

Add this result-domain model to `ResultPersistenceModel.h`:

```cpp
struct ChartJudgementTiming {
  std::array<JudgementFastSlowCount, JudgementCount> byJudgement{};

  bool operator==(const ChartJudgementTiming &) const = default;
};
```

Add `std::optional<ChartJudgementTiming> judgementTiming;` to
`ChartResultAttempt`. In `makeChartResultAttempt()`, copy every judgement's
counter from the completed state into a `ChartJudgementTiming` and store it in
the returned attempt. Keep `payloadFingerprint()` unchanged because this
ephemeral snapshot is deterministically rebuilt and the frozen outbox payload
already provides crash durability.

- [ ] **Step 4: Run the focused model test**

Run: `cmake --build cmake-build-debug --target result_persistence_model_tests -j 6 && ctest --test-dir cmake-build-debug -R '^result_persistence_model_tests$' --output-on-failure`

Expected: PASS.

### Task 2: Project only captured result metrics to IR

**Files:**
- Modify: `src/ir/IrSubmission.cpp`
- Test: `tests/ir_driver_tests.cpp`

**Interfaces:**
- Consumes: `ChartResultAttempt::score`, `ChartResultAttempt::judgementTiming`, and `ChartResultAttempt::adoptedGaugeHistory`
- Produces: validated `IrSubmission` timing fields and gauge history

- [ ] **Step 1: Write the failing classic-LN authority regression**

Create an attempt whose replay contains non-scoring classic-LN GOOD and BAD
head judgements as well as their final judgements, but whose
`judgementTiming` contains only the committed result pairs. Set a different
`adoptedGaugeHistory`. Assert that all ten early/late fields and PGREAT
fast/slow match the snapshot and that gauge history matches only the adopted
history.

- [ ] **Step 2: Run the focused IR test to verify it fails**

Run: `cmake --build cmake-build-debug --target ir_driver_tests -j 6 && ctest --test-dir cmake-build-debug -R '^ir_driver_tests$' --output-on-failure`

Expected: FAIL because replay-derived GOOD/BAD timing disagrees with the saved totals.

- [ ] **Step 3: Replace replay derivation with snapshot mapping**

Remove EX-score-delta and replay-judgement timing extraction from
`makeIrSubmission()`. If `judgementTiming` is absent, leave the optional
breakdown unavailable and PGREAT fast/slow at zero. If present, validate:

```cpp
fast >= 0;
slow >= 0;
fast + slow <= savedJudgementTotal;
sumFast == score.fast;
sumSlow == score.slow;
KPOOR and NONE timing are zero;
```

Then map `late = slow`, `early = savedJudgementTotal - slow`, and copy the
PGREAT pair. Copy and finite-check only `attempt.adoptedGaugeHistory`; do not
fall back to replay gauge events.

- [ ] **Step 4: Cover missing and inconsistent snapshots**

Keep `validAttempt()` without a timing snapshot to assert that legacy/manual
attempts omit optional timing. Add cases where a timing pair exceeds its saved
judgement total and where per-judgement sums disagree with aggregate FAST/SLOW;
both must return an invalid submission.

- [ ] **Step 5: Run the focused IR test**

Run: `cmake --build cmake-build-debug --target ir_driver_tests -j 6 && ctest --test-dir cmake-build-debug -R '^ir_driver_tests$' --output-on-failure`

Expected: PASS.

### Task 3: Verify live, recalled, and serialized boundaries

**Files:**
- Test: `tests/result_recall_builder_tests.cpp`
- Test: `tests/result_persistence_integration_tests.cpp`

**Interfaces:**
- Consumes: the Task 1 attempt factory and Task 2 IR projection
- Produces: regression assurance for recalled Records uploads and durable outbox drafts

- [ ] **Step 1: Assert recalled results expose authoritative timing**

Extend the historical-result fixture assertion so the reconstructed PGREAT
exact judgement is reported as early and the breakdown is available.

- [ ] **Step 2: Run recall and persistence integration tests**

Run: `cmake --build cmake-build-debug --target result_recall_builder_tests result_persistence_integration_tests -j 6 && ctest --test-dir cmake-build-debug -R '^(result_recall_builder_tests|result_persistence_integration_tests)$' --output-on-failure`

Expected: PASS.

- [ ] **Step 3: Run full verification**

Run: `ctest --test-dir cmake-build-debug --output-on-failure`

Expected: all tests pass.

Run: `cmake --build cmake-build-debug --target main -j 6`

Expected: desktop target builds successfully.
