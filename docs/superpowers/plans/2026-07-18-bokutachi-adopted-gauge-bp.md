# GAS Gauge History and Bokutachi BP Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Capture selectable per-gauge GAS histories, submit only the final adopted gauge history to Bokutachi, and include KPOOR in BP.

**Architecture:** `GameplayScoreState` owns parallel per-gauge histories alongside its compatibility active history. Result presentation selects among relevant series, while `ChartResultAttempt` snapshots the final series for provider-neutral IR construction. Bokutachi-specific BP mapping remains in the Direct Manual builder.

**Tech Stack:** C++20, SDL2, Yoga UI, bgfx, nlohmann/json, CMake, CTest

## Global Constraints

- The initial result series is the final active/adopted gauge.
- Each graph tap cycles to the next relevant nonempty GAS series and wraps.
- The graph label uses the selected gauge's clear-lamp color.
- Non-GAS result and upload behavior remains a single complete series.
- Realtime gameplay allocates all history storage before the worker starts.
- Do not change replay serialization, stored score columns, outbox schema, or credential handling.

---

### Task 1: Capture per-gauge history in score state

**Files:**
- Modify: `tests/gameplay_score_state_tests.cpp`
- Modify: `src/scene/play/GameplayScoreState.h`

**Interfaces:**
- Produces: `GameplayScoreState::gaugeHistories` and `gaugeHistoryFor(GaugeType)`.
- Preserves: `GameplayScoreState::gaugeHistory` and `gaugeHistoryOverflowed()`.

- [ ] **Step 1: Write failing tests**

Configure Best Clear, apply two different judgements, and require equal-length
histories for every admitted gauge with values matching `gaugeValues`. Also
require that a bounded capacity of one overflows on the second mutation without
growing any series beyond one sample.

- [ ] **Step 2: Verify RED**

```bash
cmake --build cmake-build-debug --target gameplay_score_state_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^gameplay_score_state_tests$'
```

Expected: compilation fails because per-gauge history access does not exist.

- [ ] **Step 3: Implement minimal per-gauge capture**

Add:

```cpp
using GaugeHistoryCollection =
    std::array<std::vector<float>, kGaugeTypeCount>;

GaugeHistoryCollection gaugeHistories;

const std::vector<float> &gaugeHistoryFor(GaugeType type) const {
  return gaugeHistories[gaugeTypeIndex(type)];
}
```

Reserve and clear every series with the existing active history. In
`recordGaugeHistory`, append the active value to `gaugeHistory` and append each
tracked gauge's current `gaugeValues` entry to its own series within the same
logical capacity check.

- [ ] **Step 4: Verify GREEN and commit**

Run the Step 2 commands, then:

```bash
git add src/scene/play/GameplayScoreState.h tests/gameplay_score_state_tests.cpp
git commit -m "feat: capture per-gauge GAS history"
```

### Task 2: Preserve histories through replay and realtime authority

**Files:**
- Modify: `tests/replay_summary_list_tests.cpp`
- Modify: `tests/realtime_gameplay_worker_tests.cpp`
- Modify: `tests/gameplay_simulation_tests.cpp`
- Modify: `src/ReplayResultStateBuilder.cpp`
- Modify: `src/scene/play/RealtimeGameplayWorker.h`
- Modify: `src/scene/play/RealtimeGameplayWorker.cpp`
- Modify: `src/scene/play/GamePlayScene.cpp`

**Interfaces:**
- Consumes: `GaugeHistoryCollection` from Task 1.
- Produces: `RealtimeGameplayWorker::copyGaugeHistoriesAfterStop()` and bit-consistent replay result histories.

- [ ] **Step 1: Write failing transfer and replay tests**

Require a stopped realtime worker to expose the per-gauge collection. Extend
score-state equivalence to compare `gaugeHistories`. In replay result tests,
configure GAS and require the rebuilt final gauge series to contain every
mutation.

- [ ] **Step 2: Verify RED**

```bash
cmake --build cmake-build-debug --target realtime_gameplay_worker_tests gameplay_simulation_tests replay_summary_list_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(realtime_gameplay_worker_tests|gameplay_simulation_tests|replay_summary_list_tests)$'
```

Expected: FAIL because the worker and replay synchronizer do not transfer the
new collection.

- [ ] **Step 3: Implement transfer and replay synchronization**

Return the stopped simulation's collection from the worker. When
`GamePlayScene` restores a realtime snapshot, preserve both history forms; when
the worker stops, copy both forms. In replay reconstruction, update the last
sample of the event's selected gauge after applying the recorded snapshot so
the active series stays bit-exact.

- [ ] **Step 4: Verify GREEN and commit**

Run the Step 2 commands, then:

```bash
git add src/ReplayResultStateBuilder.cpp src/scene/play/RealtimeGameplayWorker.h src/scene/play/RealtimeGameplayWorker.cpp src/scene/play/GamePlayScene.cpp tests/replay_summary_list_tests.cpp tests/realtime_gameplay_worker_tests.cpp tests/gameplay_simulation_tests.cpp
git commit -m "fix: preserve GAS histories across playback"
```

### Task 3: Select and cycle result gauge series

**Files:**
- Create: `src/scene/ResultGaugeHistory.h`
- Create: `src/scene/ResultGaugeHistory.cpp`
- Create: `tests/result_gauge_history_tests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/scene/ResultScene.cpp`
- Modify: `src/skin/DefaultSkin.cpp`

**Interfaces:**
- Produces: `result_gauge_history::availableTypes(const RhythmState&)`, `initialType(const RhythmState&)`, and `nextType(const RhythmState&, GaugeType)`.
- Consumes: per-gauge histories from Task 1.

- [ ] **Step 1: Write failing presentation tests**

Test that Best Clear returns the final active gauge first followed by other
admitted nonempty series, that Survival-to-Groove exposes only selected and
Normal gauges, that `nextType` wraps, and that non-GAS returns one type.

- [ ] **Step 2: Verify RED**

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target result_gauge_history_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^result_gauge_history_tests$'
```

Expected: compilation fails because the presentation model is absent.

- [ ] **Step 3: Implement the presentation model and graph interaction**

Build relevant nonempty gauge types in strongest-to-weakest order, rotate the
final `state.gaugeType` to the front, and wrap in `nextType`. Make the graph host
a `Button`. Update `ResultGaugeGraphView` to render a selected history and gauge
maximum. Add an absolute label using `gaugeTypeToLabel(type)` and
`clearLampColorForRank(gaugeTypeToClearRank(type))`. Each host click updates the
selected type and label.

- [ ] **Step 4: Verify GREEN and commit**

Run the Step 2 commands, then:

```bash
git add CMakeLists.txt src/scene/ResultGaugeHistory.h src/scene/ResultGaugeHistory.cpp src/scene/ResultScene.cpp src/skin/DefaultSkin.cpp tests/result_gauge_history_tests.cpp
git commit -m "feat: cycle GAS histories on results"
```

### Task 4: Feed the adopted series into IR construction

**Files:**
- Modify: `tests/result_persistence_model_tests.cpp`
- Modify: `tests/ir_driver_tests.cpp`
- Modify: `src/ResultPersistenceModel.h`
- Modify: `src/ResultPersistenceModel.cpp`
- Modify: `src/ir/IrSubmission.cpp`

**Interfaces:**
- Produces: `ChartResultAttempt::adoptedGaugeHistory`.
- Consumes: `GameplayScoreState::gaugeHistoryFor(state.gaugeType)`.

- [ ] **Step 1: Write failing state-derived and fallback tests**

Require `makeChartResultAttempt` to copy the complete final gauge series. Then
require `makeIrSubmission` to prefer that series. For a synthetic attempt with
no state-derived series, require replay fallback to retain only events matching
the final gauge-mutating event's type while still rejecting a non-finite sample
from a discarded type.

- [ ] **Step 2: Verify RED**

```bash
cmake --build cmake-build-debug --target result_persistence_model_tests ir_driver_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(result_persistence_model_tests|ir_driver_tests)$'
```

Expected: FAIL because the attempt has no adopted history and replay fallback
mixes gauge types.

- [ ] **Step 3: Implement attempt snapshot and fallback**

Add a transient `std::vector<float> adoptedGaugeHistory` to
`ChartResultAttempt`, populated from the final state. In IR construction,
validate and use it when present. Otherwise validate every gauge-mutating
replay event, identify the final event's gauge type, and collect only matching
samples. Keep PGREAT timing extraction across all events.

- [ ] **Step 4: Verify GREEN and commit**

Run the Step 2 commands, then:

```bash
git add src/ResultPersistenceModel.h src/ResultPersistenceModel.cpp src/ir/IrSubmission.cpp tests/result_persistence_model_tests.cpp tests/ir_driver_tests.cpp
git commit -m "fix: submit the adopted gauge series"
```

### Task 5: Include KPOOR in Bokutachi BP

**Files:**
- Modify: `tests/tachi_batch_manual_tests.cpp`
- Modify: `src/ir/tachi/TachiBatchManual.cpp`

**Interfaces:**
- Produces: Direct Manual `optional.bp = BAD + POOR + KPOOR`.

- [ ] **Step 1: Change BP and overflow expectations to fail**

The fixture has `bad = 2`, `poor = 1`, and `kPoor = 7`; require BP `10`.
Require `INT_MAX + 0 + 1 KPOOR` to be rejected.

- [ ] **Step 2: Verify RED**

```bash
cmake --build cmake-build-debug --target tachi_batch_manual_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^tachi_batch_manual_tests$'
```

Expected: FAIL because the current sum omits KPOOR.

- [ ] **Step 3: Add KPOOR to the wide sum**

```cpp
const long long badPoints = static_cast<long long>(submission.bad) +
                            submission.poor + submission.kPoor;
```

Keep the existing integer-range guard.

- [ ] **Step 4: Verify GREEN and commit**

Run the Step 2 commands, then:

```bash
git add src/ir/tachi/TachiBatchManual.cpp tests/tachi_batch_manual_tests.cpp
git commit -m "fix: include KPOOR in Bokutachi BP"
```

### Task 6: Integrated verification

**Files:**
- Verify only.

- [ ] **Step 1: Build affected targets**

```bash
cmake --build cmake-build-debug --target gameplay_score_state_tests realtime_gameplay_worker_tests gameplay_simulation_tests replay_summary_list_tests result_gauge_history_tests result_persistence_model_tests ir_driver_tests tachi_batch_manual_tests main -j 6
```

- [ ] **Step 2: Run focused and full suites**

```bash
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(gameplay_score_state_tests|realtime_gameplay_worker_tests|gameplay_simulation_tests|replay_summary_list_tests|result_gauge_history_tests|result_persistence_model_tests|ir_driver_tests|tachi_batch_manual_tests)$'
ctest --test-dir cmake-build-debug --output-on-failure -j 6
git diff --check
git status --short --branch
```

Expected: all tests pass, the main target builds, and the worktree is clean.
