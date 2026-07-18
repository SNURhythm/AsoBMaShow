# Bokutachi Payload and Result Layering Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep the result gauge graph below modal overlays, add payload-safe Bokutachi gauge history, exclude PGREAT from submitted FAST/SLOW, and distinguish active submission from active 202 polling.

**Architecture:** Keep the graph inside the existing View tree so sequential UI submission follows View ordering. Derive detailed IR evidence from the already-durable replay, map that evidence only at the Tachi boundary, and add an in-memory active-request discriminator to UI snapshots without changing the outbox schema.

**Tech Stack:** C++23, Yoga View hierarchy, bgfx `SimpleBatchRenderer`, nlohmann/json, SQLite-backed IR outbox, CTest, Python repository audits.

## Global Constraints

- Target branch remains `feature/bokutachi-ir`; do not push or deploy.
- Bokutachi/Tachi Direct Manual remains the only write-capable IR implementation.
- Keep local FAST/SLOW totals and score database schemas unchanged.
- Keep API keys in per-profile credential storage; never place them in `IrSubmission`, payload JSON, or outbox rows.
- Keep the durable `IrOutboxState` values unchanged; no database migration.
- Preserve full gauge history whenever the complete payload fits 64 KiB.
- When downsampling is required, preserve the first and final samples and deterministic ordering.
- Implement every production behavior only after its focused regression test fails for the expected reason.

---

### Task 1: Put the live gauge graph in the View render order

**Files:**
- Modify: `scripts/check_result_visual_layout.py`
- Modify: `src/scene/ResultScene.cpp:47-112,2005-2018,2052-2070`

**Interfaces:**
- Consumes: the existing skin-owned `View` named `graph` and `ResultScene::resultState`.
- Produces: a private `ResultGaugeGraphView final : public View` whose `renderImpl(RenderContext&)` submits the existing graph batch at its position in the View tree.

- [ ] **Step 1: Extend the audit with the failing layer contract**

Add `result_scene = read("src/scene/ResultScene.cpp")` and these checks to
`scripts/check_result_visual_layout.py`:

```python
require(
    "class ResultGaugeGraphView final : public View" in result_scene,
    "the live result gauge graph must render through the View tree",
)
require(
    "graphPlaceHolder->addView(graphView);" in result_scene,
    "the live result gauge view must be attached to the skin graph placeholder",
)

render_scene_start = result_scene.find("void ResultScene::renderScene()")
cleanup_start = result_scene.find("void ResultScene::cleanupScene()", render_scene_start)
render_scene = result_scene[render_scene_start:cleanup_start]
require(
    "drawResultGaugeLineGraph" not in render_scene
    and "SimpleBatchRenderer graphBatch" not in render_scene,
    "ResultScene::renderScene must not submit the graph after modal overlays",
)
```

- [ ] **Step 2: Run the audit and verify RED**

Run:

```bash
ctest --test-dir cmake-build-debug --output-on-failure -R '^result_visual_layout_audit$'
```

Expected: FAIL with all or part of `the live result gauge graph must render through the View tree`.

- [ ] **Step 3: Add the graph View and attach it before the overlay portal**

In the anonymous namespace in `src/scene/ResultScene.cpp`, keep
`drawResultGaugeLineGraph` and add:

```cpp
class ResultGaugeGraphView final : public View {
public:
  explicit ResultGaugeGraphView(const RhythmState &state) : state(state) {
    batch.setSubmitView(rendering::ui_view);
  }

protected:
  void renderImpl(RenderContext &context) override {
    if (state.gaugeHistory.empty() || getWidth() <= 0 || getHeight() <= 0) {
      return;
    }
    rendering::setScissorUI(context.scissor.x, context.scissor.y,
                            context.scissor.width, context.scissor.height);
    batch.begin(context.getTransformMatrix());
    drawResultGaugeLineGraph(batch, state, static_cast<float>(getX()),
                             static_cast<float>(getY()),
                             static_cast<float>(getWidth()),
                             static_cast<float>(getHeight()));
    batch.end();
  }

private:
  const RhythmState &state;
  rendering::SimpleBatchRenderer batch;
};
```

Immediately after finding `graphPlaceHolder` in `ResultScene::init()`, attach
the child before the root layout is applied:

```cpp
graphPlaceHolder = rootLayout->findViewByName("graph");
if (graphPlaceHolder != nullptr) {
  auto *graphView = new ResultGaugeGraphView(resultState);
  graphView->setWidthPercent(100.0F)->setFlex(1.0F);
  graphPlaceHolder->addView(graphView);
}
```

Delete the graph batch block from `ResultScene::renderScene()`. Retain only the
portal resize block.

- [ ] **Step 4: Run the audit and compile the result scene**

Run:

```bash
ctest --test-dir cmake-build-debug --output-on-failure -R '^result_visual_layout_audit$'
cmake --build cmake-build-debug --target main -j 6
```

Expected: the audit passes and `main` links successfully.

- [ ] **Step 5: Commit the layer fix**

```bash
git add scripts/check_result_visual_layout.py src/scene/ResultScene.cpp
git commit -m "fix: render result gauge below overlays"
```

---

### Task 2: Derive gauge and PGREAT timing evidence from replay events

**Files:**
- Modify: `tests/ir_driver_tests.cpp:20-115`
- Modify: `src/ir/IrSubmission.h:5-33`
- Modify: `src/ir/IrSubmission.cpp:12-137`

**Interfaces:**
- Consumes: `ChartResultAttempt::replay.events`, aggregate score FAST/SLOW, and the existing canonical submission validation.
- Produces: `IrSubmission::gaugeHistory`, `IrSubmission::pGreatFast`, and `IrSubmission::pGreatSlow`.

- [ ] **Step 1: Add canonical evidence tests**

Add replay evidence to a copy of `validAttempt()` in
`tests/ir_driver_tests.cpp`:

```cpp
void testCanonicalSubmissionExtractsGaugeAndPGreatTiming() {
  auto attempt = validAttempt();
  attempt.replay.events = {
      {.action = ReplayEventAction::Press,
       .judgement = PGreat,
       .diffMicros = -12'000,
       .gauge = 24.0F},
      {.action = ReplayEventAction::Release,
       .judgement = PGreat,
       .diffMicros = 8'000,
       .gauge = 25.5F},
      {.action = ReplayEventAction::Press,
       .judgement = Great,
       .diffMicros = -6'000,
       .gauge = 27.0F},
      {.action = ReplayEventAction::Press,
       .judgement = None,
       .diffMicros = -1'000,
       .gauge = 99.0F},
      {.action = ReplayEventAction::Mine, .gauge = 11.0F},
  };

  const auto outcome = ir::makeIrSubmission(attempt, 1'700'000'000'123LL);
  expect(outcome.value.has_value(), "replay evidence builds");
  if (outcome.value) {
    expect(outcome.value->gaugeHistory ==
               std::vector<float>{24.0F, 25.5F, 27.0F, 11.0F},
           "only gauge-mutating replay events become gauge history");
    expect(outcome.value->pGreatFast == 1 &&
               outcome.value->pGreatSlow == 1,
           "PGREAT early and late evidence is separated");
  }
}

void testCanonicalSubmissionRejectsInvalidDetailedEvidence() {
  auto attempt = validAttempt();
  attempt.replay.events = {{.action = ReplayEventAction::Press,
                            .judgement = Great,
                            .diffMicros = -1,
                            .gauge = std::numeric_limits<float>::quiet_NaN()}};
  expect(!ir::makeIrSubmission(attempt, 1'700'000'000'123LL).value,
         "non-finite gauge history is rejected");

  attempt = validAttempt();
  attempt.score.fast = 0;
  attempt.replay.events = {{.action = ReplayEventAction::Press,
                            .judgement = PGreat,
                            .diffMicros = -1,
                            .gauge = 20.0F}};
  expect(!ir::makeIrSubmission(attempt, 1'700'000'000'123LL).value,
         "PGREAT timing cannot exceed aggregate timing");
}
```

Include `<limits>` and `<vector>`, invoke both tests from `main()`, and keep the
existing empty-replay case to prove history remains optional.

- [ ] **Step 2: Build and run the focused test to verify RED**

```bash
cmake --build cmake-build-debug --target ir_driver_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^ir_driver_tests$'
```

Expected: compilation fails because the three new `IrSubmission` members do
not exist yet. This is the intended RED failure.

- [ ] **Step 3: Add canonical fields and extraction**

Add `<vector>` to `IrSubmission.h` and add these fields after `slow`:

```cpp
int pGreatFast = 0;
int pGreatSlow = 0;
std::vector<float> gaugeHistory;
```

Add this helper in the anonymous namespace of `IrSubmission.cpp`:

```cpp
bool replayEventMutatesGauge(const ReplayEvent &event) {
  switch (event.action) {
  case ReplayEventAction::Mine:
  case ReplayEventAction::Gauge:
    return true;
  case ReplayEventAction::Press:
  case ReplayEventAction::Release:
  case ReplayEventAction::Miss:
    return event.judgement != None;
  }
  return false;
}
```

Before constructing `IrSubmission`, scan `attempt.replay.events`:

```cpp
std::vector<float> gaugeHistory;
gaugeHistory.reserve(attempt.replay.events.size());
int pGreatFast = 0;
int pGreatSlow = 0;
for (const ReplayEvent &event : attempt.replay.events) {
  if (replayEventMutatesGauge(event)) {
    if (!std::isfinite(event.gauge)) {
      return invalid("replay gauge history is not finite");
    }
    gaugeHistory.push_back(event.gauge);
  }
  if (event.judgement == PGreat && replayEventMutatesGauge(event)) {
    if (event.diffMicros < 0) {
      ++pGreatFast;
    } else if (event.diffMicros > 0) {
      ++pGreatSlow;
    }
  }
}
if (pGreatFast > score.fast || pGreatSlow > score.slow) {
  return invalid("PGREAT timing exceeds aggregate timing counts");
}
```

Move the vector into the returned submission and assign both counts.

- [ ] **Step 4: Run focused tests to verify GREEN**

```bash
cmake --build cmake-build-debug --target ir_driver_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^ir_driver_tests$'
```

Expected: PASS.

- [ ] **Step 5: Commit canonical evidence**

```bash
git add src/ir/IrSubmission.h src/ir/IrSubmission.cpp tests/ir_driver_tests.cpp
git commit -m "feat: capture Bokutachi gauge evidence"
```

---

### Task 3: Map gauge history and non-PGREAT timing into Direct Manual

**Files:**
- Modify: `tests/tachi_batch_manual_tests.cpp:30-420`
- Modify: `src/ir/tachi/TachiBatchManual.cpp:12-337`

**Interfaces:**
- Consumes: the canonical detailed evidence from Task 2.
- Produces: `optional.gaugeHistory`, adjusted `optional.fast` and `optional.slow`, and a deterministic payload no larger than `kMaximumPayloadBytes`.
- Invariant: downsample candidates are nested prefixes of a balanced refinement order, so every added sample strictly increases serialized size and the largest fitting prefix can be selected without assuming unrelated resamples have monotonic sizes.

- [ ] **Step 1: Add mapping and payload-budget tests**

Update `validSubmission()` with:

```cpp
submission.pGreatFast = 5;
submission.pGreatSlow = 7;
submission.gaugeHistory = {20.0F, 31.25F, 48.5F, 82.0F};
```

Extend `testBuildsOneScoreBatchManual()`:

```cpp
expect(score.at("optional").at("fast") == 25,
       "submitted fast excludes early PGREAT");
expect(score.at("optional").at("slow") == 33,
       "submitted slow excludes late PGREAT");
expect(score.at("optional").at("gaugeHistory") ==
           nlohmann::json::array({20.0, 31.25, 48.5, 82.0}),
       "payload includes complete fitting gauge history");
```

Add:

```cpp
void testGaugeHistoryDownsamplesWithinPayloadLimit() {
  auto submission = validSubmission();
  submission.gaugeHistory.clear();
  for (int index = 0; index < 30'000; ++index) {
    submission.gaugeHistory.push_back(
        static_cast<float>((index * 37) % 10'001) / 100.0F);
  }
  const auto outcome = ir::tachi::buildBatchManualDraft(submission);
  expect(outcome.draft.has_value(), "oversized history still builds");
  if (!outcome.draft) {
    return;
  }
  expect(outcome.draft->payloadJson.size() <= ir::tachi::kMaximumPayloadBytes,
         "downsampled payload respects provider limit");
  const auto document = nlohmann::json::parse(outcome.draft->payloadJson);
  const auto &history =
      document.at("scores").at(0).at("optional").at("gaugeHistory");
  expect(history.size() < submission.gaugeHistory.size() && history.size() >= 2,
         "oversized history is reduced but retained");
  expect(history.front() == submission.gaugeHistory.front() &&
             history.back() == submission.gaugeHistory.back(),
         "downsampling preserves endpoints");

  const auto repeated = ir::tachi::buildBatchManualDraft(submission);
  expect(repeated.draft &&
             repeated.draft->payloadJson == outcome.draft->payloadJson,
         "downsampling is deterministic");
}
```

Add invalid cases where `pGreatFast > fast`, `pGreatSlow > slow`, and one
history sample is non-finite. Invoke the new test from `main()`.

- [ ] **Step 2: Run the focused test to verify RED**

```bash
cmake --build cmake-build-debug --target tachi_batch_manual_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^tachi_batch_manual_tests$'
```

Expected: FAIL because payload FAST/SLOW are still aggregate values and
`gaugeHistory` is absent.

- [ ] **Step 3: Validate detailed evidence and build payloads from selected indices**

In `buildBatchManualDraft()`, validate:

```cpp
if (submission.pGreatFast < 0 || submission.pGreatSlow < 0 ||
    submission.pGreatFast > submission.fast ||
    submission.pGreatSlow > submission.slow) {
  return invalid("submission PGREAT timing breakdown is invalid");
}
if (std::ranges::any_of(submission.gaugeHistory,
                        [](float value) { return !std::isfinite(value); })) {
  return invalid("submission gauge history is not finite");
}
```

Extract payload construction into local lambdas. `makeDocument(indices)` must
build the existing fields, use adjusted timing, and add history only when
nonempty. Indices passed to the document builder are always sorted in replay
order:

```cpp
const int fast = submission.fast - submission.pGreatFast;
const int slow = submission.slow - submission.pGreatSlow;

const auto sampledHistory = [&](std::span<const std::size_t> indices) {
  nlohmann::json history = nlohmann::json::array();
  for (std::size_t index : indices) {
    history.push_back(
        std::clamp(submission.gaugeHistory[index], 0.0F, 100.0F));
  }
  return history;
};
```

The `optional` object uses `fast`, `slow`, and conditionally:

```cpp
if (!submission.gaugeHistory.empty()) {
  optional["gaugeHistory"] = sampledHistory(indices);
}
```

- [ ] **Step 4: Select the largest fitting nested sample set**

Serialize the full document first using every index in source order. If it
fits, use it unchanged. Otherwise, generate a balanced refinement order:

1. Add index `0`, then the final index.
2. Repeatedly split the currently largest unsampled interval at its midpoint.
3. Resolve equal-width intervals by lower starting index.

This makes every candidate a nested prefix while spreading retained samples
across the play. Build an empty-history document once, then add each sampled
float token's exact `nlohmann::json(value).dump().size()` plus its comma. Stop
before the first addition that would cross `kMaximumPayloadBytes`; because the
candidates are nested, that prefix has the largest fitting sample count.

Sort the retained indices before building the final array so gauge samples
remain in replay order. Require at least the first and final samples and apply
a final serialized-size assertion through the existing invalid outcome:

```cpp
std::vector<std::size_t> allIndices(submission.gaugeHistory.size());
std::iota(allIndices.begin(), allIndices.end(), 0);
std::vector<std::size_t> selected = allIndices;
std::string payload = makeDocument(allIndices).dump();
if (payload.size() > kMaximumPayloadBytes) {
  const auto refinement = balancedSampleOrder(submission.gaugeHistory.size());
  const std::string emptyPayload =
      makeDocument(std::span<const std::size_t>{}).dump();
  selected.clear();
  std::size_t selectedPayloadSize = emptyPayload.size();
  for (std::size_t index : refinement) {
    const std::size_t separatorSize = selected.empty() ? 0 : 1;
    const std::size_t tokenSize =
        nlohmann::json(std::clamp(submission.gaugeHistory[index],
                                  0.0F, 100.0F))
            .dump()
            .size();
    if (selectedPayloadSize + separatorSize + tokenSize >
        kMaximumPayloadBytes) {
      break;
    }
    selectedPayloadSize += separatorSize + tokenSize;
    selected.push_back(index);
  }
  std::ranges::sort(selected);
  payload = makeDocument(selected).dump();
}
if ((!submission.gaugeHistory.empty() &&
     submission.gaugeHistory.size() > 1 && selected.size() < 2) ||
    payload.size() > kMaximumPayloadBytes) {
  return invalid("submission payload exceeds the provider size limit");
}
```

The production code may structure the local variables differently, but must
retain the exact-size accounting, nested candidate property, endpoint order,
and final full serialization check. Add `<numeric>`, `<queue>`, and `<span>` as
needed.

Keep the existing proof fingerprint over the final frozen payload. Do not add
credentials or local proof fields to the JSON.

- [ ] **Step 5: Run focused Direct Manual tests to verify GREEN**

```bash
cmake --build cmake-build-debug --target tachi_batch_manual_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^tachi_batch_manual_tests$'
```

Expected: PASS.

- [ ] **Step 6: Commit the provider mapping**

```bash
git add src/ir/tachi/TachiBatchManual.cpp tests/tachi_batch_manual_tests.cpp
git commit -m "feat: submit Bokutachi gauge history"
```

---

### Task 4: Distinguish active POST and poll presentation

**Files:**
- Modify: `tests/ir_submission_service_tests.cpp:34-745`
- Modify: `tests/ir_result_presentation_tests.cpp:70-115`
- Modify: `src/ir/IrSubmissionService.h:20-39`
- Modify: `src/ir/IrSubmissionService.cpp:112-123`
- Modify: `src/ir/IrResultPresentation.h:12-28`
- Modify: `src/ir/IrResultPresentation.cpp:44-64`

**Interfaces:**
- Consumes: an already-validated claimed outbox row and its persisted remote identity.
- Produces: `IrActiveRequestKind`, `IrAttemptStatusSnapshot::activeRequest`, and `IrResultState::Polling`.

- [ ] **Step 1: Add failing snapshot and presentation tests**

Add to `tests/ir_submission_service_tests.cpp`:

```cpp
void testActiveRequestSnapshotsDistinguishSubmitAndPoll() {
  Harness submit;
  submit.setCredential("key");
  submit.repository.EnqueueReadyIrOutboxDraft(draft(20, submit.now.load()),
                                              false);
  submit.driver->blockRequestsUntilCancelled();
  submit.service->start(profile(true));
  expect(submit.driver->waitForCalls(1), "blocked submit starts");
  expect(submit.waitForSnapshot(
             attemptId(20), [](const auto &snapshot) {
               return snapshot.state == ir::IrOutboxState::Uploading &&
                      snapshot.activeRequest ==
                          ir::IrActiveRequestKind::Submit;
             }),
         "fresh claim publishes submit activity");
  submit.service->pauseAndCancel();

  Harness poll;
  poll.setCredential("key");
  makeAwaiting(poll, 21);
  poll.now += 10'000;
  poll.driver->blockRequestsUntilCancelled();
  poll.service->start(profile(true));
  expect(poll.driver->waitForCalls(1), "blocked poll starts");
  expect(poll.waitForSnapshot(
             attemptId(21), [](const auto &snapshot) {
               return snapshot.state == ir::IrOutboxState::Uploading &&
                      snapshot.activeRequest ==
                          ir::IrActiveRequestKind::Poll;
             }),
         "deferred claim publishes poll activity");
  poll.service->pauseAndCancel();
}
```

Invoke it from `main()`.

In `tests/ir_result_presentation_tests.cpp`, add an Uploading snapshot with
`.activeRequest = ir::IrActiveRequestKind::Poll` and require:

```cpp
REQUIRE(presentation.state == ir::IrResultState::Polling);
REQUIRE(presentation.statusText == "Polling Bokutachi");
REQUIRE(presentation.detailText ==
        "Checking the queued import result with Bokutachi.");
```

- [ ] **Step 2: Build focused tests and verify RED**

```bash
cmake --build cmake-build-debug --target ir_submission_service_tests ir_result_presentation_tests -j 6
```

Expected: compilation fails because `IrActiveRequestKind`, `activeRequest`, and
`IrResultState::Polling` do not exist.

- [ ] **Step 3: Add the in-memory active request discriminator**

In `IrSubmissionService.h`, add:

```cpp
enum class IrActiveRequestKind { None, Submit, Poll };
```

Add this member to `IrAttemptStatusSnapshot` after `state`:

```cpp
IrActiveRequestKind activeRequest = IrActiveRequestKind::None;
```

In `snapshotFrom()` classify only active claims:

```cpp
const IrActiveRequestKind activeRequest =
    entry.state != IrOutboxState::Uploading
        ? IrActiveRequestKind::None
        : (!entry.remoteJobId.empty() && !entry.remoteOrigin.empty()
               ? IrActiveRequestKind::Poll
               : IrActiveRequestKind::Submit);
```

Place `.activeRequest = activeRequest` in the returned snapshot. Do not change
`IrOutboxState`, SQL state values, claim logic, or stale-claim recovery.

- [ ] **Step 4: Map active polling in result presentation**

Add `Polling` between `Submitting` and `Waiting` in `IrResultState`. Change the
`Uploading` case to:

```cpp
case IrOutboxState::Uploading:
  if (input.snapshot.activeRequest == IrActiveRequestKind::Poll) {
    result.state = IrResultState::Polling;
    result.statusText = "Polling " + result.providerDisplayName;
    result.detailText =
        "Checking the queued import result with " +
        result.providerDisplayName + ".";
  } else {
    result.state = IrResultState::Submitting;
    result.statusText = "Submitting";
    result.detailText =
        "Sending this score to " + result.providerDisplayName + ".";
  }
  break;
```

Keep `AwaitingRemoteResult` as the inactive wait between poll attempts.

- [ ] **Step 5: Run focused state-machine tests to verify GREEN**

```bash
cmake --build cmake-build-debug --target ir_submission_service_tests ir_result_presentation_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(ir_submission_service_tests|ir_result_presentation_tests)$'
```

Expected: both tests pass. The service test must observe `Submit` for the first
blocked request and `Poll` for a blocked deferred request.

- [ ] **Step 6: Commit the presentation correction**

```bash
git add src/ir/IrSubmissionService.h src/ir/IrSubmissionService.cpp \
  src/ir/IrResultPresentation.h src/ir/IrResultPresentation.cpp \
  tests/ir_submission_service_tests.cpp tests/ir_result_presentation_tests.cpp
git commit -m "fix: distinguish Tachi polling state"
```

---

### Task 5: Integrated verification

**Files:**
- Verify only; modify production or tests only if a failure demonstrates a defect in Tasks 1–4.

**Interfaces:**
- Consumes: all four completed changes.
- Produces: fresh evidence that the feature branch builds and all repository tests pass.

- [ ] **Step 1: Run formatting and focused verification**

```bash
git diff --check HEAD~4..HEAD
cmake --build cmake-build-debug --target \
  ir_driver_tests tachi_batch_manual_tests ir_submission_service_tests \
  ir_result_presentation_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R \
  '^(result_visual_layout_audit|ir_driver_tests|tachi_batch_manual_tests|ir_submission_service_tests|ir_result_presentation_tests)$'
```

Expected: no whitespace errors, all requested targets build, and all five
focused tests pass.

- [ ] **Step 2: Run the complete suite**

```bash
ctest --test-dir cmake-build-debug --output-on-failure -j 6
```

Expected: 100% pass with zero failed tests.

- [ ] **Step 3: Audit credential and outbox hygiene**

```bash
rg -n 'apiKey|Authorization|Bearer' src/ir/tachi/TachiBatchManual.cpp \
  src/ir/IrSubmission.h src/ir/IrSubmission.cpp
git status --short --branch
```

Expected: no credential reference in the canonical submission or Batch Manual
mapper; the worktree is clean and the branch is only ahead of its remote.

- [ ] **Step 4: Report completion without pushing**

Report the graph render-order correction, payload evidence and downsampling,
non-PGREAT FAST/SLOW mapping, corrected submit/poll UI sequence, exact build
and test results, commit IDs, and that the branch was not pushed or deployed.
