# Bokutachi Adopted Gauge and BP Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Submit only the final GAS-adopted gauge history to Bokutachi and include KPOOR in BP.

**Architecture:** Keep replay evidence canonical and provider-neutral in `makeIrSubmission`, where the final gauge-mutating event identifies the adopted gauge. Keep Bokutachi-specific BP mapping in the Direct Manual builder. No persistence schema or replay format changes are required.

**Tech Stack:** C++20, nlohmann/json, CMake, CTest

## Global Constraints

- Validate every gauge-mutating replay sample even when it belongs to a discarded GAS transition.
- PGREAT early/late extraction continues to use every judged replay event.
- Do not change replay storage, result graph behavior, outbox schema, or credential handling.
- Preserve complete gauge history for plays whose gauge type never changes.

---

### Task 1: Select only the adopted gauge history

**Files:**
- Modify: `tests/ir_driver_tests.cpp`
- Modify: `src/ir/IrSubmission.cpp`

**Interfaces:**
- Consumes: `ReplayEvent::gaugeType`, `ReplayEvent::gauge`, and the existing `replayEventMutatesGauge(const ReplayEvent&)` predicate.
- Produces: `IrSubmission::gaugeHistory` containing only samples whose gauge type matches the final gauge-mutating replay event.

- [ ] **Step 1: Write the failing regression test**

Add a replay with Hard samples followed by Normal samples and assert that only
the Normal values are retained:

```cpp
void testCanonicalSubmissionKeepsOnlyAdoptedGaugeHistory() {
  auto attempt = validAttempt();
  attempt.replay.events = {
      {.action = ReplayEventAction::Press,
       .judgement = Great,
       .gauge = 72.0F,
       .gaugeType = GaugeType::Hard},
      {.action = ReplayEventAction::Press,
       .judgement = Great,
       .gauge = 64.0F,
       .gaugeType = GaugeType::Hard},
      {.action = ReplayEventAction::Press,
       .judgement = Great,
       .gauge = 24.0F,
       .gaugeType = GaugeType::Normal},
      {.action = ReplayEventAction::Press,
       .judgement = Great,
       .gauge = 26.0F,
       .gaugeType = GaugeType::Normal},
  };
  const auto outcome = ir::makeIrSubmission(attempt, 1'700'000'000'123LL);
  expect(outcome.value && outcome.value->gaugeHistory ==
                              std::vector<float>{24.0F, 26.0F},
         "only the adopted GAS gauge becomes upload history");
}
```

- [ ] **Step 2: Run the test and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target ir_driver_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^ir_driver_tests$'
```

Expected: FAIL because the history still contains the two Hard transition
samples.

- [ ] **Step 3: Implement adopted-gauge selection**

In `makeIrSubmission`, first validate all gauge-mutating samples and remember
the final event's gauge type. Continue deriving PGREAT timing across all replay
events. Then append only matching samples:

```cpp
std::optional<GaugeType> adoptedGaugeType;
for (const ReplayEvent &event : attempt.replay.events) {
  if (!replayEventMutatesGauge(event)) {
    continue;
  }
  if (!std::isfinite(event.gauge)) {
    return invalid("replay gauge history is not finite");
  }
  adoptedGaugeType = event.gaugeType;
  // Preserve existing PGREAT timing extraction.
}
if (adoptedGaugeType) {
  for (const ReplayEvent &event : attempt.replay.events) {
    if (replayEventMutatesGauge(event) &&
        event.gaugeType == *adoptedGaugeType) {
      gaugeHistory.push_back(event.gauge);
    }
  }
}
```

- [ ] **Step 4: Run the focused test and verify GREEN**

Run the Step 2 commands again. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/ir/IrSubmission.cpp tests/ir_driver_tests.cpp
git commit -m "fix: submit only adopted gauge history"
```

### Task 2: Include KPOOR in Bokutachi BP

**Files:**
- Modify: `tests/tachi_batch_manual_tests.cpp`
- Modify: `src/ir/tachi/TachiBatchManual.cpp`

**Interfaces:**
- Consumes: `IrSubmission::bad`, `IrSubmission::poor`, and `IrSubmission::kPoor`.
- Produces: Bokutachi Direct Manual `optional.bp` equal to `BAD + POOR + KPOOR`.

- [ ] **Step 1: Change payload and overflow expectations to fail**

For the existing fixture (`bad = 2`, `poor = 1`, `kPoor = 7`), require BP
`10`. Change the overflow fixture to overflow through KPOOR:

```cpp
expect(score.at("optional").at("bp") == 10,
       "BP includes bad, poor, and KPOOR");

submission = validSubmission();
submission.bad = std::numeric_limits<int>::max();
submission.poor = 0;
submission.kPoor = 1;
expectInvalid(submission, "BP integer overflow is invalid");
```

- [ ] **Step 2: Run the test and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target tachi_batch_manual_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^tachi_batch_manual_tests$'
```

Expected: FAIL because BP is currently `BAD + POOR` and KPOOR cannot trigger
the overflow guard.

- [ ] **Step 3: Add KPOOR to the wide BP sum**

```cpp
const long long badPoints = static_cast<long long>(submission.bad) +
                            submission.poor + submission.kPoor;
```

Keep the existing `std::numeric_limits<int>::max()` guard and serialized cast.

- [ ] **Step 4: Run the focused test and verify GREEN**

Run the Step 2 commands again. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/ir/tachi/TachiBatchManual.cpp tests/tachi_batch_manual_tests.cpp
git commit -m "fix: include KPOOR in Bokutachi BP"
```

### Task 3: Integrated verification

**Files:**
- Verify only; no production changes.

**Interfaces:**
- Consumes: committed Task 1 and Task 2 changes.
- Produces: build and test evidence for handoff.

- [ ] **Step 1: Build affected targets**

```bash
cmake --build cmake-build-debug --target ir_driver_tests tachi_batch_manual_tests main -j 6
```

Expected: exit code 0.

- [ ] **Step 2: Run focused IR tests**

```bash
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(ir_driver_tests|tachi_batch_manual_tests|ir_submission_service_tests)$'
```

Expected: all selected tests pass.

- [ ] **Step 3: Run the full suite and repository checks**

```bash
ctest --test-dir cmake-build-debug --output-on-failure -j 6
git diff --check
git status --short --branch
```

Expected: 103 tests pass, no whitespace errors, and no uncommitted changes.
