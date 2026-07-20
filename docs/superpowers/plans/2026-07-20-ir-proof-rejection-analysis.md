# IR Proof Rejection Analysis Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Explain both the precise safe cause and consequence whenever a saved result cannot reproduce its historical IR proof.

**Architecture:** `ResultRecallBuilder` will own historical-proof analysis and carry a diagnostic beside a successfully reconstructed chart result when only IR proof reconstruction fails. `IrUploadsScene` will forward that diagnostic into the existing session-only preparation failure pipeline, whose row already appends `Failed: ...` to the attempt detail line.

**Tech Stack:** C++23, existing result persistence and IR submission models, Python source-flow audit, CMake/CTest.

## Global Constraints

- Preserve the existing fail-closed verification decision.
- Show both the safe cause and the consequence/remediation.
- Do not include chart titles, paths, hashes, UUIDs, replay payloads, credentials, API keys, or exception text.
- Keep immediate verification diagnostics session-only.
- Do not change batch request behavior, outbox persistence, candidate eligibility, or imported-score persistence.
- Do not push or deploy.

---

## File Structure

- Modify `src/ResultRecallBuilder.h`: carry a historical IR proof diagnostic on `ChartResult`.
- Modify `src/ResultRecallBuilder.cpp`: analyze each historical-proof rejection stage and produce deterministic safe copy.
- Modify `src/scene/IrUploadsScene.cpp`: forward the builder analysis through `VerificationOutcome`.
- Modify `tests/result_recall_builder_tests.cpp`: verify every diagnostic category and successful empty state.
- Modify `scripts/check_ir_uploads_flow.py`: require diagnostic forwarding and reject the obsolete generic message.

---

### Task 1: Analyze Historical IR Proof Reconstruction

**Files:**
- Modify: `src/ResultRecallBuilder.h:20-38`
- Modify: `src/ResultRecallBuilder.cpp:20-105`
- Test: `tests/result_recall_builder_tests.cpp:1-220`

**Interfaces:**
- Consumes: `result_persistence::makeChartResultAttempt` diagnostics, stored attempt metadata, fingerprint comparison, and `ir::makeIrSubmission` diagnostics.
- Produces: `result_recall::ChartResult::historicalIrDiagnostic` as a safe non-empty string exactly when `historicalIr` is absent after a successful chart reconstruction.

- [ ] **Step 1: Write failing diagnostic assertions**

Write tests against this target interface; do not add the production field until Step 3:

```cpp
struct ChartResult {
  std::unique_ptr<bms_parser::Chart> chart;
  ReplayData replay;
  RhythmState state;
  std::optional<HistoricalIrContext> historicalIr;
  std::string historicalIrDiagnostic;
};
```

Add `<array>` to the test includes. Assert the matching case is empty:

```cpp
assert(outcome.value->historicalIr.has_value());
assert(outcome.value->historicalIrDiagnostic.empty());
```

Require these exact diagnostics for missing metadata:

```cpp
constexpr std::array expected{
    "IR verification failed because the saved result has no attempt identity. "
    "This score cannot be uploaded safely.",
    "IR verification failed because the saved result has no integrity fingerprint. "
    "This score cannot be uploaded safely.",
    "IR verification failed because the saved result has no integrity fingerprint. "
    "This score cannot be uploaded safely.",
    "IR verification failed because the saved result has no play completion time. "
    "This score cannot be uploaded safely.",
};
```

The four variants reset the attempt ID, reset the fingerprint, set the fingerprint to an empty string, and set the timestamp to zero.

Add focused test functions for reconstruction, fingerprint, and submission validation:

```cpp
void testAttemptReconstructionExplainsInvariantFailure() {
  auto record = validRecord();
  record.replay.finalGauge = 99.0F;
  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildChartResult(
      std::move(record), cancelled, chartLoader());
  assert(outcome.value->historicalIrDiagnostic ==
         "IR verification failed: final gauge mismatch. The saved replay no "
         "longer reproduces the original score, so it cannot be uploaded safely.");
}

void testFingerprintMismatchExplainsLikelyCause() {
  auto record = validRecord();
  record.attemptFingerprint = std::string(64, 'f');
  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildChartResult(
      std::move(record), cancelled, chartLoader());
  assert(outcome.value->historicalIrDiagnostic ==
         "IR verification failed because the stored fingerprint differs from "
         "the reconstructed score. The chart or replay metadata may have "
         "changed since the score was saved, so it cannot be uploaded safely.");
}

void testSubmissionValidationExplainsInvariantFailure() {
  auto record = validRecord();
  record.replay.chartMeta.KeyMode = 0;
  bms_parser::Chart chart;
  chart.Meta = record.replay.chartMeta;
  RhythmState state = replay_result::BuildResultState(chart, record.replay);
  std::string diagnostic;
  auto attempt = result_persistence::makeChartResultAttempt(
      *record.attemptId, chart.Meta, state, record.replay.provenance,
      record.replay.chartMeta.LnMode, record.replay, diagnostic);
  assert(attempt.has_value());
  record.attemptFingerprint = attempt->payloadFingerprint;
  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildChartResult(
      std::move(record), cancelled, chartLoader());
  assert(outcome.value->historicalIrDiagnostic ==
         "IR submission validation failed: chart key mode must be positive. "
         "This score cannot be uploaded safely.");
}
```

- [ ] **Step 2: Build and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target result_recall_builder_tests -j 6
```

Expected: compilation fails because `ChartResult::historicalIrDiagnostic` does not exist.

- [ ] **Step 3: Implement structured internal proof outcomes**

In `ResultRecallBuilder.cpp`, replace the optional-only helper with an internal outcome:

```cpp
struct HistoricalIrBuildOutcome {
  std::optional<HistoricalIrContext> value;
  std::string diagnostic;
};
```

Return the exact missing-metadata messages before attempt reconstruction. When `makeChartResultAttempt` fails, use its safe diagnostic or `canonical score attempt could not be reconstructed` as a fallback:

```cpp
return {.diagnostic =
            "IR verification failed: " + safeInvariant +
            ". The saved replay no longer reproduces the original score, "
            "so it cannot be uploaded safely."};
```

Return the exact fingerprint message without including either fingerprint. When `makeIrSubmission` fails, use its safe diagnostic or `canonical submission construction failed`:

```cpp
return {.diagnostic = "IR submission validation failed: " + safeInvariant +
                      ". This score cannot be uploaded safely."};
```

On success, return the `HistoricalIrContext` with an empty diagnostic. In `BuildChartResult`, move both fields into `ChartResult`:

```cpp
auto historicalIr = historicalIrFor(record, chart->Meta, state);
return {.value = ChartResult{
            .chart = std::move(chart),
            .replay = std::move(record.replay),
            .state = std::move(state),
            .historicalIr = std::move(historicalIr.value),
            .historicalIrDiagnostic = std::move(historicalIr.diagnostic)}};
```

- [ ] **Step 4: Run the focused test and verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target result_recall_builder_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^result_recall_builder_tests$'
```

Expected: 1/1 test passes.

- [ ] **Step 5: Format, check, and commit**

Run:

```bash
git clang-format --force --quiet HEAD -- \
  src/ResultRecallBuilder.h src/ResultRecallBuilder.cpp \
  tests/result_recall_builder_tests.cpp
git diff --check
```

Commit:

```bash
git add src/ResultRecallBuilder.h src/ResultRecallBuilder.cpp \
  tests/result_recall_builder_tests.cpp
git commit -m "feat: explain historical IR proof rejection"
```

---

### Task 2: Forward the Analysis to IR Upload Rows

**Files:**
- Modify: `src/scene/IrUploadsScene.cpp:575-605`
- Modify: `scripts/check_ir_uploads_flow.py:1-85`

**Interfaces:**
- Consumes: `ChartResult::historicalIrDiagnostic` from Task 1.
- Produces: `VerificationOutcome::diagnostic`, which the existing controller sanitizes, retains for the current page session, and appends to the row attempt-detail line.

- [ ] **Step 1: Write a failing flow-audit assertion**

Add these checks to `scripts/check_ir_uploads_flow.py`:

```python
require(
    "historicalIrDiagnostic" in uploads_scene,
    "IR Uploads forwards historical proof rejection analysis",
)
reject(
    "This saved result has no verifiable IR proof." in uploads_scene,
    "IR Uploads must not replace proof analysis with a generic rejection",
)
```

- [ ] **Step 2: Run the audit and verify RED**

Run:

```bash
ctest --test-dir cmake-build-debug --output-on-failure -R '^ir_uploads_flow_audit$'
```

Expected: the audit fails because the scene does not reference `historicalIrDiagnostic` and still contains the generic literal.

- [ ] **Step 3: Forward the builder diagnostic**

Replace the scene's generic no-proof result with:

```cpp
if (!recalled.value->historicalIr ||
    !recalled.value->historicalIr->submission) {
  return ir_uploads::VerificationOutcome{
      .diagnostic = recalled.value->historicalIrDiagnostic.empty()
                        ? "IR verification failed because historical proof "
                          "reconstruction returned no analysis. This score "
                          "cannot be uploaded safely."
                        : ir::sanitizeDiagnostic(
                              recalled.value->historicalIrDiagnostic)};
}
```

- [ ] **Step 4: Run focused tests and verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target \
  result_recall_builder_tests ir_uploads_controller_tests \
  ir_upload_candidate_list_view_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(result_recall_builder_tests|ir_uploads_flow_audit|ir_uploads_controller_tests|ir_upload_candidate_list_view_tests)$'
```

Expected: 4/4 tests pass, including the flow audit.

- [ ] **Step 5: Build and run the full suite**

Run:

```bash
cmake --build cmake-build-debug --target main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -j 6
git diff --check
git status --short
```

Expected: `main` links, all configured tests pass, and only the two Task 2 files are modified.

- [ ] **Step 6: Commit the forwarding change**

```bash
git add src/scene/IrUploadsScene.cpp scripts/check_ir_uploads_flow.py
git commit -m "feat: show IR proof rejection analysis"
```

- [ ] **Step 7: Confirm local-only delivery**

Run:

```bash
git status --short
git log -5 --oneline
```

Expected: the working tree is clean. Do not push, resolve GitHub reviews, or deploy.
