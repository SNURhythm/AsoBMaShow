# Result Conflict Diagnostics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give every Result-screen save conflict a non-empty, phase-specific diagnostic that identifies all differing score fields and exposes expected/actual numeric values.

**Architecture:** Add one pure score comparison formatter to `ResultPersistenceModel`, then reuse it at the coordinator and repository comparison boundaries. The coordinator remains the final defensive boundary: it adds phase context and supplies a fallback whenever an injected dependency reports an integrity conflict without details. `ResultScene` continues to render the resulting diagnostic unchanged.

**Tech Stack:** C++23, SQLite repository adapters, the existing assertion-based C++ test targets, CMake/CTest.

## Global Constraints

- Cover only conflicts that can produce `SaveState::UnstagedConflict` or `SaveState::PendingConflict` on the current Result screen.
- Do not change multi-BAD counting or score-save decisions in this pass.
- Keep public warning copy unchanged; diagnostics remain behind **Show Details** and in existing logs.
- Do not expose text identity payloads, filesystem paths, hashes, titles, artists, or provenance bodies; name those differing fields only.
- Numeric differences must include expected and actual values.
- Do not edit amalgamated `src/bms_parser.hpp` or `src/bms_parser.cpp`.

---

### Task 1: Shared score-payload difference formatter

**Files:**
- Modify: `src/ResultPersistenceModel.h`
- Test: `tests/result_persistence_model_tests.cpp`

**Interfaces:**
- Consumes: `result_persistence::ChartScoreWrite` and its default equality semantics.
- Produces: `std::string describeChartScoreDifference(const ChartScoreWrite &expected, const ChartScoreWrite &actual)`; returns an empty string for equal values and otherwise returns `score payload differs: ...`.

- [ ] **Step 1: Write the failing formatter tests**

Add a helper beside `expectScoreFingerprintChange` that changes one score field, calls the wished-for API, and checks the diagnostic. Use it from a new `testScoreDifferenceDiagnostics()` for all fields:

```cpp
template <typename Mutator>
void expectScoreDifference(const AttemptFixture &fixture, Mutator mutate,
                           std::string_view expectedFragment) {
  const auto expected = scoreFor(fixture);
  auto actual = expected;
  mutate(actual);
  const std::string diagnostic =
      result_persistence::describeChartScoreDifference(expected, actual);
  expect(diagnostic.find(expectedFragment) != std::string::npos,
         std::string("score diagnostic names ") +
             std::string(expectedFragment));
}

void testScoreDifferenceDiagnostics() {
  const AttemptFixture fixture;
  const auto score = scoreFor(fixture);
  expect(result_persistence::describeChartScoreDifference(score, score).empty(),
         "equal score payloads have no difference diagnostic");
  expectScoreDifference(fixture, [](auto &v) { v.chartPath += "!"; },
                        "chartPath");
  expectScoreDifference(fixture, [](auto &v) { v.chartMd5 += "!"; },
                        "chartMd5");
  expectScoreDifference(fixture, [](auto &v) { v.chartSha256 += "!"; },
                        "chartSha256");
  expectScoreDifference(fixture, [](auto &v) { v.chartTitle += "!"; },
                        "chartTitle");
  expectScoreDifference(fixture, [](auto &v) { v.chartArtist += "!"; },
                        "chartArtist");
  expectScoreDifference(fixture, [](auto &v) { ++v.longNoteMode; },
                        "longNoteMode expected=2 actual=3");
  expectScoreDifference(fixture, [](auto &v) { ++v.score; }, "score expected=");
  expectScoreDifference(fixture, [](auto &v) { ++v.maxScore; },
                        "maxScore expected=");
  expectScoreDifference(fixture, [](auto &v) { ++v.maxCombo; },
                        "maxCombo expected=");
  expectScoreDifference(fixture, [](auto &v) { ++v.comboBreak; },
                        "comboBreak expected=");
  expectScoreDifference(fixture, [](auto &v) { ++v.pGreat; },
                        "pGreat expected=");
  expectScoreDifference(fixture, [](auto &v) { ++v.great; },
                        "great expected=");
  expectScoreDifference(fixture, [](auto &v) { ++v.good; }, "good expected=");
  expectScoreDifference(fixture, [](auto &v) { ++v.bad; },
                        "bad expected=1 actual=2");
  expectScoreDifference(fixture, [](auto &v) { ++v.poor; }, "poor expected=");
  expectScoreDifference(fixture, [](auto &v) { ++v.kPoor; },
                        "kPoor expected=");
  expectScoreDifference(fixture, [](auto &v) { ++v.fast; }, "fast expected=");
  expectScoreDifference(fixture, [](auto &v) { ++v.slow; }, "slow expected=");
  expectScoreDifference(fixture, [](auto &v) { v.finalGauge += 0.5F; },
                        "finalGauge expected=");
  expectScoreDifference(fixture, [](auto &v) { ++v.clearType; },
                        "clearType expected=");
  expectScoreDifference(fixture, [](auto &v) { ++v.provenance.schemaVersion; },
                        "provenance");
}
```

Call `testScoreDifferenceDiagnostics()` from `main()`.

- [ ] **Step 2: Run the model test and verify RED**

Run: `cmake --build cmake-build-debug --target result_persistence_model_tests -j 6`

Expected: compilation fails because `describeChartScoreDifference` is undeclared.

- [ ] **Step 3: Implement the pure formatter**

Define the formatter inline beside `ChartScoreWrite` in
`ResultPersistenceModel.h` so the deliberately small coordinator test target
does not need to link the full model implementation. Compare every field in
struct order. Add opaque names for text/provenance fields and formatted
expected/actual values for scalar fields. Compare `finalGauge` by bit pattern
to preserve the repository's existing collision rule. Join multiple
differences with `; `:

```cpp
std::string describeChartScoreDifference(const ChartScoreWrite &expected,
                                         const ChartScoreWrite &actual) {
  std::vector<std::string> differences;
  const auto opaque = [&](std::string_view name, const auto &left,
                          const auto &right) {
    if (left != right) {
      differences.emplace_back(name);
    }
  };
  const auto scalar = [&](std::string_view name, const auto &left,
                          const auto &right) {
    if (left != right) {
      differences.push_back(std::string(name) +
                            " expected=" + std::to_string(left) +
                            " actual=" + std::to_string(right));
    }
  };

  opaque("chartPath", expected.chartPath, actual.chartPath);
  opaque("chartMd5", expected.chartMd5, actual.chartMd5);
  opaque("chartSha256", expected.chartSha256, actual.chartSha256);
  opaque("chartTitle", expected.chartTitle, actual.chartTitle);
  opaque("chartArtist", expected.chartArtist, actual.chartArtist);
  scalar("longNoteMode", expected.longNoteMode, actual.longNoteMode);
  scalar("score", expected.score, actual.score);
  scalar("maxScore", expected.maxScore, actual.maxScore);
  scalar("maxCombo", expected.maxCombo, actual.maxCombo);
  scalar("comboBreak", expected.comboBreak, actual.comboBreak);
  scalar("pGreat", expected.pGreat, actual.pGreat);
  scalar("great", expected.great, actual.great);
  scalar("good", expected.good, actual.good);
  scalar("bad", expected.bad, actual.bad);
  scalar("poor", expected.poor, actual.poor);
  scalar("kPoor", expected.kPoor, actual.kPoor);
  scalar("fast", expected.fast, actual.fast);
  scalar("slow", expected.slow, actual.slow);
  const std::uint32_t expectedGaugeBits =
      std::bit_cast<std::uint32_t>(expected.finalGauge);
  const std::uint32_t actualGaugeBits =
      std::bit_cast<std::uint32_t>(actual.finalGauge);
  if (expectedGaugeBits != actualGaugeBits) {
    differences.push_back(
        "finalGauge expected=" + std::to_string(expected.finalGauge) +
        " actual=" + std::to_string(actual.finalGauge) +
        " expectedBits=" + std::to_string(expectedGaugeBits) +
        " actualBits=" + std::to_string(actualGaugeBits));
  }
  scalar("clearType", expected.clearType, actual.clearType);
  opaque("provenance", expected.provenance, actual.provenance);

  if (differences.empty()) {
    return {};
  }
  std::string result = "score payload differs: ";
  for (std::size_t index = 0; index < differences.size(); ++index) {
    if (index != 0) {
      result += "; ";
    }
    result += differences[index];
  }
  return result;
}
```

Add direct `<bit>`, `<cstdint>`, `<vector>`, and `<string_view>` includes.

- [ ] **Step 4: Run the model test and verify GREEN**

Run: `cmake --build cmake-build-debug --target result_persistence_model_tests -j 6 && ./cmake-build-debug/result_persistence_model_tests`

Expected: `result persistence model tests passed`.

- [ ] **Step 5: Review Task 1 diff**

Run: `git diff --check -- src/ResultPersistenceModel.h tests/result_persistence_model_tests.cpp`

Expected: no output and exit code 0.

- [ ] **Step 6: Commit the shared formatter**

Run: `git add src/ResultPersistenceModel.h tests/result_persistence_model_tests.cpp && git commit -m "feat: describe score payload differences"`

Expected: one commit containing the pure formatter and complete field coverage.

---

### Task 2: Coordinator phase context and defensive fallbacks

**Files:**
- Modify: `src/ResultPersistenceCoordinator.cpp`
- Test: `tests/result_persistence_coordinator_tests.cpp`

**Interfaces:**
- Consumes: `describeChartScoreDifference(expected, actual)` from Task 1 and dependency outcome diagnostics.
- Produces: every `UnstagedConflict` and `PendingConflict` returned by `Coordinator::persist()` has a non-empty diagnostic containing its persistence phase.

- [ ] **Step 1: Write failing dependency-conflict tests**

Add a test that injects empty diagnostics at each conflict boundary and checks exact phase fallbacks. Use a fresh harness for every case:

```cpp
void testEveryPersistConflictHasPhaseDiagnostic() {
  {
    Harness harness;
    harness.stageResult = {.status = StageStatus::IntegrityConflict};
    Coordinator coordinator(harness.dependencies());
    const SaveOutcome outcome = coordinator.persist(attempt());
    assert(outcome.state == SaveState::UnstagedConflict);
    assert(outcome.diagnostic ==
           "staging: integrity conflict reported without details");
  }
  {
    Harness harness;
    harness.loadResult = {.status = PendingReadStatus::NotFound};
    Coordinator coordinator(harness.dependencies());
    const SaveOutcome outcome = coordinator.persist(attempt());
    assert(outcome.state == SaveState::PendingConflict);
    assert(outcome.diagnostic ==
           "pending score read: no pending score was found");
  }
  {
    Harness harness;
    harness.loadResult = {.status = PendingReadStatus::IntegrityConflict};
    Coordinator coordinator(harness.dependencies());
    const SaveOutcome outcome = coordinator.persist(attempt());
    assert(outcome.diagnostic ==
           "pending score read: integrity conflict reported without details");
  }
  {
    Harness harness;
    harness.projectResult = {.status = ProjectionStatus::IntegrityConflict};
    Coordinator coordinator(harness.dependencies());
    const SaveOutcome outcome = coordinator.persist(attempt());
    assert(outcome.diagnostic ==
           "score projection: integrity conflict reported without details");
  }
  {
    Harness harness;
    harness.acknowledgeResult =
        {.status = AcknowledgeStatus::IntegrityConflict};
    Coordinator coordinator(harness.dependencies());
    const SaveOutcome outcome = coordinator.persist(attempt());
    assert(outcome.diagnostic ==
           "acknowledgement: integrity conflict reported without details");
  }
}
```

Extend the existing non-empty stage, pending-read, projection, and
acknowledgement conflict tests to expect the phase prefix, for example
`staging: attempt fingerprint mismatch` and
`score projection: score payload differs`.

- [ ] **Step 2: Add a failing BAD payload diagnostic assertion**

In `testPendingPayloadMismatchStopsBeforeProjection`, add a case that increments
`bad` and expects the shared field detail:

```cpp
PendingChartScoreWrite badMismatch = pending();
badMismatch.score.bad = 2;
assertMismatch(std::move(badMismatch),
               "pending score validation: score payload differs: "
               "bad expected=0 actual=2");
```

- [ ] **Step 3: Run the coordinator test and verify RED**

Run: `cmake --build cmake-build-debug --target result_persistence_coordinator_tests -j 6 && ./cmake-build-debug/result_persistence_coordinator_tests`

Expected: assertions fail because empty dependency diagnostics remain empty and payload mismatches remain generic.

- [ ] **Step 4: Implement phase-aware conflict composition**

Add anonymous-namespace helpers that preserve supplied details and guarantee a fallback:

```cpp
std::string phaseDiagnostic(std::string_view phase,
                            std::string_view diagnostic,
                            std::string_view fallback) {
  std::string result(phase);
  result += ": ";
  result += diagnostic.empty() ? fallback : diagnostic;
  return result;
}

void appendPhaseDiagnostic(std::string &destination, std::string_view phase,
                           std::string_view diagnostic,
                           std::string_view fallback) {
  appendDiagnostic(destination,
                   phaseDiagnostic(phase, diagnostic, fallback));
}
```

Use these only for paths that return a conflict:

- Staging `IntegrityConflict`: `staging`.
- Unknown staging status: `staging` with `unknown staging status`.
- Invalid successful stage receipt: `staging receipt validation` and the
  specific invalid receipt fields.
- Pending `NotFound`: `pending score read` with `no pending score was found`.
- Pending `IntegrityConflict`: `pending score read`.
- Pending `Found` without a payload and unknown pending statuses:
  `pending score read`.
- Pending identity, timestamp, and payload invariants:
  `pending score validation`.
- Projection `IntegrityConflict`: `score projection`.
- Unknown projection status: `score projection`.
- Acknowledgement `IntegrityConflict`: `acknowledgement`.
- Unknown acknowledgement status: `acknowledgement`.

Replace the generic pending payload mismatch with:

```cpp
appendPhaseDiagnostic(
    staged.diagnostic, "pending score validation",
    describeChartScoreDifference(attempt.score, pending.score),
    "score payload differs without an identifiable field");
```

As defense against future branches, make `unstagedOutcome` and
`durableOutcome` replace an empty diagnostic with
`persistence conflict reported without details` whenever their state is a
conflict.

- [ ] **Step 5: Run the coordinator test and verify GREEN**

Run: `cmake --build cmake-build-debug --target result_persistence_coordinator_tests -j 6 && ./cmake-build-debug/result_persistence_coordinator_tests`

Expected: `result persistence coordinator tests passed`.

- [ ] **Step 6: Review Task 2 diff**

Run: `git diff --check -- src/ResultPersistenceCoordinator.cpp tests/result_persistence_coordinator_tests.cpp`

Expected: no output and exit code 0.

- [ ] **Step 7: Commit coordinator diagnostics**

Run: `git add src/ResultPersistenceCoordinator.cpp src/ResultPersistenceCoordinator.h tests/result_persistence_coordinator_tests.cpp && git commit -m "feat: describe result persistence conflicts"`

Expected: one commit containing the conflict presentation contract and all
coordinator diagnostic behavior.

---

### Task 3: Repository collision detail reaches the Result outcome

**Files:**
- Modify: `src/ResultPersistenceModel.h`
- Modify: `src/repositories/ReplayRepositoryRecords.cpp`
- Modify: `src/repositories/ScoreRepositoryQueries.cpp`
- Test: `tests/result_persistence_model_tests.cpp`
- Test: `tests/result_persistence_integration_tests.cpp`

**Interfaces:**
- Consumes: `describeChartScoreDifference(expected, actual)` from Task 1.
- Produces: repository collisions name the exact differing score fields before the coordinator adds its phase prefix.

- [ ] **Step 1: Write a failing end-to-end projection collision test**

Add this test near the other crash/retry persistence integration cases:

```cpp
void testProjectionConflictNamesDifferingBadCount() {
  TemporaryDirectory temporary("projection-bad-conflict");
  const auto replayPath = temporary.path() / "replay.db";
  const auto scorePath = temporary.path() / "score.db";
  ReplayRepository replay(replayPath);
  ScoreRepository score(scorePath);
  const ChartResultAttempt fixed =
      sampleAttempt(temporary.path(), "projection-bad-conflict", 4);

  const StageOutcome staged = replay.StageChartResult(fixed, {});
  assert(staged.status == StageStatus::Staged);
  const PendingReadOutcome loaded =
      replay.LoadPendingChartScore(fixed.attemptId);
  assert(loaded.status == PendingReadStatus::Found && loaded.value);

  PendingChartScoreWrite conflicting = *loaded.value;
  ++conflicting.score.bad;
  assert(score.SaveProjectedScore(conflicting).status ==
         ProjectionStatus::Inserted);

  Coordinator coordinator(score, replay);
  const SaveOutcome outcome = coordinator.persist(fixed);
  assert(outcome.state == SaveState::PendingConflict);
  assert(outcome.diagnostic.find("score projection: score payload differs: ") !=
         std::string::npos);
  assert(outcome.diagnostic.find("bad expected=1 actual=2") !=
         std::string::npos);
}
```

Call the test from `main()`.

- [ ] **Step 2: Run the integration test and verify RED**

Run: `cmake --build cmake-build-debug --target result_persistence_integration_tests -j 6 && ./cmake-build-debug/result_persistence_integration_tests`

Expected: the final diagnostic is generic and does not contain the BAD values.

- [ ] **Step 3: Replace repository generic score mismatch diagnostics**

In `ReplayRepository::StageChartResult`, separate pending identity, replay ID,
timestamp, and score checks. For the score check use:

```cpp
return {.status = StageStatus::IntegrityConflict,
        .diagnostic = "staged pending " +
                      describeChartScoreDifference(attempt.score,
                                                   pending.value->score)};
```

In `classifyProjectedScoreCollision`, keep the existing attempt ID and timestamp
checks explicit, then use:

```cpp
const std::string scoreDifference =
    describeChartScoreDifference(pending.score, stored);
if (!scoreDifference.empty()) {
  return {.status = ProjectionStatus::IntegrityConflict,
          .diagnostic = std::move(scoreDifference)};
}
```

Do not change collision decisions or field equality rules.

Add a model regression assertion that `+0.0F` and `-0.0F` final gauges produce
a `finalGauge` difference so the original bit-exact repository behavior cannot
silently regress.

- [ ] **Step 4: Run the integration and focused unit tests**

Run: `ctest --test-dir cmake-build-debug --output-on-failure -R '^(result_persistence_model_tests|result_persistence_coordinator_tests|result_persistence_integration_tests)$'`

Expected: 3 tests passed, 0 failed.

- [ ] **Step 5: Review Task 3 diff**

Run: `git diff --check -- src/ResultPersistenceModel.h src/repositories/ReplayRepositoryRecords.cpp src/repositories/ScoreRepositoryQueries.cpp tests/result_persistence_model_tests.cpp tests/result_persistence_integration_tests.cpp`

Expected: no output and exit code 0.

- [ ] **Step 6: Commit repository diagnostics**

Run: `git add src/ResultPersistenceModel.h src/repositories/ReplayRepositoryRecords.cpp src/repositories/ScoreRepositoryQueries.cpp tests/result_persistence_model_tests.cpp tests/result_persistence_integration_tests.cpp && git commit -m "feat: identify conflicting score fields"`

Expected: one commit containing repository field diagnostics and their
end-to-end regression test.

---

### Task 4: Full verification and handoff for UI testing

**Files:**
- Verify all files changed by Tasks 1-3 and the already implemented Result modal files.

**Interfaces:**
- Consumes: the complete modal and diagnostics implementation.
- Produces: fresh build/test evidence and a clean reviewed diff ready for manual UI testing.

- [ ] **Step 1: Run persistence and visual audits**

Run: `ctest --test-dir cmake-build-debug --output-on-failure -R '^(result_persistence_flow_audit|result_visual_layout_audit|result_persistence_model_tests|result_persistence_coordinator_tests|result_persistence_integration_tests)$'`

Expected: 5 tests passed, 0 failed.

- [ ] **Step 2: Compile the desktop application**

Run: `cmake --build cmake-build-debug --target main -j 6`

Expected: build exits 0; existing third-party bgfx variadic-macro warnings may remain.

- [ ] **Step 3: Inspect the final working tree**

Run: `git diff --check`

Expected: no output and exit code 0.

Run: `git status --short`

Expected: only the intended modal, diagnostic, test, and plan files are modified or untracked.

- [ ] **Step 4: Report the manual UI test scenario**

Tell the user to reproduce any Result save conflict, select **Show Details**, and
confirm the modal contains the phase, exact reason, attempt ID, replay ID when
available, and `bad expected=<n> actual=<n>` if BAD is the differing payload
field. Explicitly state that multi-BAD counting has not been changed.

- [ ] **Step 5: Commit the modal and implementation plan**

Run: `git add src/scene/ResultScene.cpp src/scene/ResultScene.h docs/superpowers/plans/2026-07-25-result-conflict-diagnostics.md && git commit -m "feat: show result save conflict details"`

Expected: one commit containing the blocking details modal and this execution
record.
