# Frame-Independent Practice Sessions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run session-backed manual practice and practice autoplay on the realtime gameplay authority, including count-in judgement and audio-clock practice completion.

**Architecture:** Add an explicit bounded-practice completion time to `RealtimeGameplayWorker` and let it finalize `GameplaySimulation` from the audio clock. Isolate the scene's realtime eligibility/range/count-in decisions in a small pure policy, then make `GamePlayScene` consume `PracticeComplete` exactly once and restart the worker when a loop is rebuilt on the next frame.

**Tech Stack:** C++23, SDL2, existing `GameplaySimulation`, `RealtimeGameplayWorker`, `practice::Session`, CMake/CTest, Xcode iOS build-only verification.

## Global Constraints

- The selected practice range is half-open: only notes in `[startMicros, endMicros)` are eligible.
- Practice autoplay uses the platform-neutral realtime worker on every platform.
- Manual practice uses realtime input only where a native asynchronous input source is already implemented; other platforms keep the existing fallback.
- Legacy `practiceMode` without `practice::Session` and replay playback remain excluded from the realtime authority.
- Count-in input may judge the first in-range note only when its compensated timestamp lies inside the note's early judge window.
- Normal non-practice preparation retains its activation gate and visual-only preparation events.
- The next practice loop may wait for a rendered frame, but gameplay, keysounds, misses, gauge mutation, and the completion signal inside an attempt may not.
- Input-triggered sound and gameplay state retain the worker's reserve/commit/fail-closed ordering.
- Do not deploy an iOS build; use `scripts/ios_firebase_deploy.sh --build-only --skip-init` only.
- Review the complete code delta after Task 3, matching the requested three-task review cadence.

---

## File Structure

- `src/scene/play/RealtimeGameplayWorker.h/.cpp`: own the bounded practice end on the realtime thread and publish `PracticeComplete`.
- `src/scene/play/RealtimeGameplayAuthorityPolicy.h/.cpp`: pure, testable decision boundary for authority eligibility, exact range propagation, activation gating, and count-in judgement.
- `src/scene/play/GamePlayScene.h/.cpp`: apply the policy, synchronize final practice state, and perform the next-frame loop/result transition.
- `tests/realtime_gameplay_worker_tests.cpp`: prove count-in judgement and practice completion without a frame pump.
- `tests/realtime_gameplay_authority_policy_tests.cpp`: prove session-backed manual/autoplay eligibility and preserve exclusions.
- `CMakeLists.txt` and `src/scene/play/CMakeLists.txt`: build and register the policy and its test.

---

### Task 1: Bounded practice completion in the realtime worker

**Files:**
- Modify: `src/scene/play/RealtimeGameplayWorker.h`
- Modify: `src/scene/play/RealtimeGameplayWorker.cpp`
- Modify: `tests/realtime_gameplay_worker_tests.cpp`

**Interfaces:**
- Consumes: `GameplaySimulationConfig::allowedNoteRange` and `GameplaySimulation::finalizePracticeRange(std::int64_t, std::int64_t)`.
- Produces: `RealtimeGameplayWorkerConfig::practiceCompletionSongTimeMicros` and worker snapshots whose terminal reason is `GameplayTerminalReason::PracticeComplete`.

- [ ] **Step 1: Write failing worker tests for count-in judgement and bounded completion**

Add a one-note definition and these tests to `tests/realtime_gameplay_worker_tests.cpp`:

```cpp
gameplay::GameplayDefinition makePracticeDefinition() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 2;
  chart.Meta.KeyMode = 7;
  auto *measure = new bms_parser::Measure();
  addTimeline(*measure, 1'000'000)->SetNote(1, new bms_parser::Note(31));
  addTimeline(*measure, 1'100'000)->SetNote(2, new bms_parser::Note(32));
  chart.Measures.push_back(measure);
  return gameplay::buildGameplayDefinition(chart, 0);
}

void testPracticeCountInPressJudgesFirstInRangeNote() {
  FakeClock clock;
  FakeAudio audio;
  auto config = makeConfig(clock, audio);
  config.simulation.allowedNoteRange = gameplay::GameplayTimeRange{
      .startMicros = 1'000'000, .endMicros = 1'100'000};
  config.practiceCompletionSongTimeMicros = 1'100'000;
  gameplay::RealtimeGameplayWorker worker(makePracticeDefinition(),
                                           std::move(config));
  require(worker.start(), "practice count-in worker starts");
  require(worker.enqueueInput({.epoch = 7,
                               .type = gameplay::RealtimeGameplayInputType::Press,
                               .lane = 1,
                               .compensateLane = 1,
                               .steadyTimestampMicros = 999'999}),
          "count-in press reaches the practice authority");
  require(waitUntil([&] { return audio.commitCount.load() == 1; }),
          "valid early count-in hit commits its keysound");
  auto snapshot = worker.acquireLatestSnapshot();
  require(snapshot && snapshot->noteStates[0].played &&
              snapshot->attempt.judgeCounts[PGreat] == 1,
          "valid early count-in hit judges the first in-range note");
  worker.stop();
}

void testPracticeCountInPressOutsideJudgeWindowStaysUnjudged() {
  FakeClock clock;
  FakeAudio audio;
  auto config = makeConfig(clock, audio);
  config.simulation.allowedNoteRange = gameplay::GameplayTimeRange{
      .startMicros = 1'000'000, .endMicros = 1'100'000};
  config.practiceCompletionSongTimeMicros = 1'100'000;
  gameplay::RealtimeGameplayWorker worker(makePracticeDefinition(),
                                           std::move(config));
  require(worker.start(), "early count-in rejection worker starts");
  require(worker.enqueueInput({.epoch = 7,
                               .type = gameplay::RealtimeGameplayInputType::Press,
                               .lane = 1,
                               .compensateLane = 1,
                               .steadyTimestampMicros = 500'000}),
          "far-early count-in press reaches the practice authority");
  std::this_thread::sleep_for(10ms);
  auto snapshot = worker.acquireLatestSnapshot();
  require(snapshot && !snapshot->noteStates[0].played &&
              snapshot->attempt.judgeCounts[PGreat] == 0 &&
              snapshot->attempt.judgeCounts[Poor] == 0 &&
              audio.commitCount.load() == 0,
          "count-in press outside every judge window stays unjudged");
  worker.stop();
}

void testPracticeCompletesFromAudioClockWithoutFramePump() {
  FakeClock clock;
  FakeAudio audio;
  auto config = makeConfig(clock, audio);
  config.simulation.allowedNoteRange = gameplay::GameplayTimeRange{
      .startMicros = 1'000'000, .endMicros = 1'100'000};
  config.practiceCompletionSongTimeMicros = 1'100'000;
  gameplay::RealtimeGameplayWorker worker(makePracticeDefinition(),
                                           std::move(config));
  require(worker.start(), "bounded practice worker starts");
  clock.nowMicros.store(1'100'000, std::memory_order_release);
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && snapshot->terminalReason ==
                                   gameplay::GameplayTerminalReason::PracticeComplete;
          }),
          "audio clock publishes PracticeComplete without a frame pump");
  auto snapshot = worker.acquireLatestSnapshot();
  require(snapshot && snapshot->noteStates[0].dead &&
              !snapshot->noteStates[1].played &&
              snapshot->attempt.judgeCounts[Poor] == 1 &&
              snapshot->replayEventCount == 1,
          "practice finalization misses only unresolved in-range identities");
  worker.stop();
}

void testPracticeAutoplayCompletesWithoutFramePump() {
  FakeClock clock;
  FakeAudio audio;
  auto config = makeConfig(clock, audio);
  config.simulation.allowedNoteRange = gameplay::GameplayTimeRange{
      .startMicros = 1'000'000, .endMicros = 1'100'000};
  config.simulation.attempt.autoPlay = true;
  config.practiceCompletionSongTimeMicros = 1'100'000;
  gameplay::RealtimeGameplayWorker worker(makePracticeDefinition(),
                                           std::move(config));
  require(worker.start(), "bounded practice autoplay starts");
  clock.nowMicros.store(1'000'000, std::memory_order_release);
  require(waitUntil([&] { return audio.commitCount.load() == 1; }),
          "practice autoplay commits its in-range keysound");
  clock.nowMicros.store(1'100'000, std::memory_order_release);
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && snapshot->terminalReason ==
                                   gameplay::GameplayTerminalReason::PracticeComplete;
          }),
          "practice autoplay completes from audio time");
  auto snapshot = worker.acquireLatestSnapshot();
  require(snapshot && snapshot->noteStates[0].played &&
              !snapshot->noteStates[1].played &&
              snapshot->attempt.judgeCounts[PGreat] == 1,
          "practice autoplay resolves only the selected half-open range");
  worker.stop();
}
```

Register all four functions in `main()` after the activation-gate test and before the ordinary autoplay test.

- [ ] **Step 2: Build and run the worker test to verify RED**

Run:

```bash
cmake --build cmake-build-debug --target realtime_gameplay_worker_tests -j 6
```

Expected: compilation fails because `RealtimeGameplayWorkerConfig` has no member named `practiceCompletionSongTimeMicros`.

- [ ] **Step 3: Add the worker boundary and transaction helper**

Add `<span>` to the direct standard-library includes in
`RealtimeGameplayWorker.h`, then add the completion field immediately after
`activationSongTimeMicros` in `RealtimeGameplayWorkerConfig` so later
designated initializers keep declaration order:

```cpp
std::optional<std::int64_t> practiceCompletionSongTimeMicros;
```

Add this private method declaration to `RealtimeGameplayWorker`:

```cpp
bool commitAutomaticTransactions(
    std::span<const GameplayInputResult> transactions);
```

Move the existing per-transaction reserve/record/commit loop from
`advanceAutomatic()` into the new method. Its complete behavior is:

```cpp
bool RealtimeGameplayWorker::commitAutomaticTransactions(
    std::span<const GameplayInputResult> transactions) {
  for (const auto &transaction : transactions) {
    const bool requiresSound =
        config_.inputTriggeredKeysounds &&
        transaction.soundNoteId != kInvalidNoteId;
    RealtimeGameplayAudioReservation reservation;
    if (requiresSound &&
        (config_.audio.reserve == nullptr ||
         !config_.audio.reserve(config_.audio.context,
                                transaction.soundNoteId, reservation))) {
      latchFault(RealtimeGameplayFault::AudioCapacityUnavailable);
      return false;
    }
    recordTransaction(transaction);
    if (!requiresSound) {
      continue;
    }
    if (config_.audio.commit == nullptr) {
      if (reservation.requiresCommit && config_.audio.cancel != nullptr) {
        config_.audio.cancel(config_.audio.context, reservation,
                             transaction.soundNoteId);
      }
      latchFault(RealtimeGameplayFault::AudioCommitFailed);
      return false;
    }
    if (!config_.audio.commit(config_.audio.context, reservation,
                              transaction.soundNoteId)) {
      latchFault(RealtimeGameplayFault::AudioCommitFailed);
      return false;
    }
  }
  return true;
}
```

In `advanceAutomatic()`, clamp automatic advancement to the last eligible
practice microsecond, commit that transaction span, then finalize the range:

```cpp
const auto practiceEnd = config_.practiceCompletionSongTimeMicros;
const std::int64_t advanceTime =
    practiceEnd.has_value() && *songTime >= *practiceEnd
        ? *practiceEnd - 1
        : *songTime;
const auto result = simulation_.advanceTo(advanceTime, advanceTime);
if (!commitAutomaticTransactions(result.transactions)) {
  return true;
}
if (practiceEnd.has_value() && *songTime >= *practiceEnd &&
    !simulation_.terminal()) {
  const auto finalized =
      simulation_.finalizePracticeRange(*practiceEnd - 1, *practiceEnd - 1);
  if (!commitAutomaticTransactions(finalized.transactions)) {
    return true;
  }
}
```

Keep the existing before/after snapshot comparisons so finalization publishes a new snapshot even when it emits no transaction.

- [ ] **Step 4: Run the worker test to verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target realtime_gameplay_worker_tests -j 6
./cmake-build-debug/realtime_gameplay_worker_tests
```

Expected: build succeeds and the executable exits with status 0.

- [ ] **Step 5: Commit Task 1**

```bash
git add src/scene/play/RealtimeGameplayWorker.h \
        src/scene/play/RealtimeGameplayWorker.cpp \
        tests/realtime_gameplay_worker_tests.cpp
git commit -m "fix: finalize practice on the realtime clock"
```

---

### Task 2: Pure realtime practice authority policy

**Files:**
- Create: `src/scene/play/RealtimeGameplayAuthorityPolicy.h`
- Create: `src/scene/play/RealtimeGameplayAuthorityPolicy.cpp`
- Create: `tests/realtime_gameplay_authority_policy_tests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/scene/play/CMakeLists.txt`

**Interfaces:**
- Consumes: `gameplay::GameplayTimeRange`, platform native-manual-input availability, launch flags, start position, and preparation activation time.
- Produces: `gameplay::RealtimeGameplayAuthorityPolicy makeRealtimeGameplayAuthorityPolicy(const RealtimeGameplayAuthorityPolicyInput &) noexcept`.

- [ ] **Step 1: Write the failing policy test**

Create `tests/realtime_gameplay_authority_policy_tests.cpp`:

```cpp
#include "scene/play/RealtimeGameplayAuthorityPolicy.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void testSessionBackedPracticeEligibility() {
  const gameplay::GameplayTimeRange range{.startMicros = 1'000'000,
                                          .endMicros = 2'000'000};
  const auto manual = gameplay::makeRealtimeGameplayAuthorityPolicy({
      .nativeManualInputAvailable = true,
      .autoPlay = false,
      .inputHandlerAvailable = true,
      .practiceMode = true,
      .practiceRange = range,
      .startPositionMicros = range.startMicros,
      .preparationActivationSongTimeMicros = range.startMicros,
  });
  require(manual.eligible && manual.allowedNoteRange.has_value() &&
              manual.allowedNoteRange->startMicros == range.startMicros &&
              manual.allowedNoteRange->endMicros == range.endMicros &&
              manual.practiceCompletionSongTimeMicros == range.endMicros &&
              !manual.activationSongTimeMicros.has_value(),
          "session-backed manual practice uses exact realtime boundaries");

  const auto autoplay = gameplay::makeRealtimeGameplayAuthorityPolicy({
      .nativeManualInputAvailable = false,
      .autoPlay = true,
      .inputHandlerAvailable = false,
      .practiceMode = true,
      .practiceRange = range,
      .startPositionMicros = range.startMicros,
      .preparationActivationSongTimeMicros = range.startMicros,
  });
  require(autoplay.eligible,
          "practice autoplay does not require native input or an input handler");
  require(!gameplay::preparationInputUsesVisualOnlyPath(true, true) &&
              gameplay::preparationInputUsesVisualOnlyPath(true, false),
          "only session-backed practice judges through preparation");
}

void testExistingExclusionsAndNormalActivationRemain() {
  const auto legacyPractice =
      gameplay::makeRealtimeGameplayAuthorityPolicy({
          .nativeManualInputAvailable = true,
          .autoPlay = false,
          .inputHandlerAvailable = true,
          .practiceMode = true,
      });
  require(!legacyPractice.eligible,
          "legacy practice without a session remains excluded");

  const auto replay = gameplay::makeRealtimeGameplayAuthorityPolicy({
      .nativeManualInputAvailable = true,
      .autoPlay = false,
      .inputHandlerAvailable = true,
      .replayPlayback = true,
  });
  require(!replay.eligible, "replay playback remains excluded");

  const auto normal = gameplay::makeRealtimeGameplayAuthorityPolicy({
      .nativeManualInputAvailable = true,
      .autoPlay = false,
      .inputHandlerAvailable = true,
      .startPositionMicros = 500'000,
      .preparationActivationSongTimeMicros = 500'000,
  });
  require(normal.eligible && normal.allowedNoteRange.has_value() &&
              normal.allowedNoteRange->startMicros == 500'000 &&
              normal.allowedNoteRange->endMicros ==
                  std::numeric_limits<std::int64_t>::max() &&
              normal.activationSongTimeMicros == 500'000,
          "ordinary partial starts retain open range and activation gate");
}

} // namespace

int main() {
  testSessionBackedPracticeEligibility();
  testExistingExclusionsAndNormalActivationRemain();
  return 0;
}
```

Add a `realtime_gameplay_authority_policy_tests` executable and CTest registration in root `CMakeLists.txt`, compiling the new policy source with C++23.

Use this target definition beside the other gameplay authority tests:

```cmake
add_executable(realtime_gameplay_authority_policy_tests
    tests/realtime_gameplay_authority_policy_tests.cpp
    src/scene/play/RealtimeGameplayAuthorityPolicy.cpp
)
target_include_directories(realtime_gameplay_authority_policy_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(realtime_gameplay_authority_policy_tests PRIVATE
    cxx_std_23
)
```

Register it beside `realtime_gameplay_worker_tests`:

```cmake
asobmashow_register_test(realtime_gameplay_authority_policy_tests)
```

- [ ] **Step 2: Build the policy test to verify RED**

Run:

```bash
cmake --build cmake-build-debug --target realtime_gameplay_authority_policy_tests -j 6
```

Expected: compilation fails because `RealtimeGameplayAuthorityPolicy.h` does not exist.

- [ ] **Step 3: Implement the pure policy**

Create `RealtimeGameplayAuthorityPolicy.h`:

```cpp
#pragma once

#include "GameplaySimulation.h"

#include <cstdint>
#include <optional>

namespace gameplay {

struct RealtimeGameplayAuthorityPolicyInput {
  bool nativeManualInputAvailable = false;
  bool autoPlay = false;
  bool inputHandlerAvailable = false;
  bool replayPlayback = false;
  bool practiceMode = false;
  std::optional<GameplayTimeRange> practiceRange;
  std::int64_t startPositionMicros = 0;
  std::optional<std::int64_t> preparationActivationSongTimeMicros;
};

struct RealtimeGameplayAuthorityPolicy {
  bool eligible = false;
  std::optional<GameplayTimeRange> allowedNoteRange;
  std::optional<std::int64_t> practiceCompletionSongTimeMicros;
  std::optional<std::int64_t> activationSongTimeMicros;
};

[[nodiscard]] RealtimeGameplayAuthorityPolicy
makeRealtimeGameplayAuthorityPolicy(
    const RealtimeGameplayAuthorityPolicyInput &input) noexcept;

[[nodiscard]] constexpr bool preparationInputUsesVisualOnlyPath(
    bool indicatorActive, bool sessionBackedPractice) noexcept {
  return indicatorActive && !sessionBackedPractice;
}

} // namespace gameplay
```

Create `RealtimeGameplayAuthorityPolicy.cpp`:

```cpp
#include "RealtimeGameplayAuthorityPolicy.h"

#include <limits>

namespace gameplay {

RealtimeGameplayAuthorityPolicy makeRealtimeGameplayAuthorityPolicy(
    const RealtimeGameplayAuthorityPolicyInput &input) noexcept {
  RealtimeGameplayAuthorityPolicy result;
  const bool sessionBackedPractice = input.practiceRange.has_value();
  const bool legacyPractice = input.practiceMode && !sessionBackedPractice;
  result.eligible =
      !input.replayPlayback && !legacyPractice &&
      (input.autoPlay ||
       (input.nativeManualInputAvailable && input.inputHandlerAvailable));
  if (sessionBackedPractice) {
    result.allowedNoteRange = input.practiceRange;
    result.practiceCompletionSongTimeMicros = input.practiceRange->endMicros;
  } else {
    if (input.startPositionMicros > 0) {
      result.allowedNoteRange = GameplayTimeRange{
          .startMicros = input.startPositionMicros,
          .endMicros = std::numeric_limits<std::int64_t>::max(),
      };
    }
    result.activationSongTimeMicros =
        input.preparationActivationSongTimeMicros;
  }
  return result;
}

} // namespace gameplay
```

Add `RealtimeGameplayAuthorityPolicy.cpp` to `src/scene/play/CMakeLists.txt`.

- [ ] **Step 4: Run the policy test to verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target realtime_gameplay_authority_policy_tests -j 6
./cmake-build-debug/realtime_gameplay_authority_policy_tests
```

Expected: build succeeds and the executable exits with status 0.

- [ ] **Step 5: Commit Task 2**

```bash
git add CMakeLists.txt src/scene/play/CMakeLists.txt \
        src/scene/play/RealtimeGameplayAuthorityPolicy.h \
        src/scene/play/RealtimeGameplayAuthorityPolicy.cpp \
        tests/realtime_gameplay_authority_policy_tests.cpp
git commit -m "feat: define realtime practice authority policy"
```

---

### Task 3: Apply realtime practice policy in GamePlayScene

**Files:**
- Modify: `src/scene/play/RealtimeGameplayAuthorityPolicy.h`
- Modify: `src/scene/play/RealtimeGameplayAuthorityPolicy.cpp`
- Modify: `src/scene/play/GamePlayScene.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Test: `tests/realtime_gameplay_authority_policy_tests.cpp`
- Test: `tests/realtime_gameplay_worker_tests.cpp`

**Interfaces:**
- Consumes: `makeRealtimeGameplayAuthorityPolicy()`, `RealtimeGameplayWorkerConfig::practiceCompletionSongTimeMicros`, and `GameplayTerminalReason::PracticeComplete`.
- Produces: session-backed manual/autoplay practice that starts and restarts the realtime authority, permits valid count-in judgement, and completes exactly once.

- [ ] **Step 1: Write failing policy tests for scene restart and terminal handling**

Add these assertions to `testExistingExclusionsAndNormalActivationRemain()` before the ordinary partial-start assertion:

```cpp
const auto unsupportedManual =
    gameplay::makeRealtimeGameplayAuthorityPolicy({
        .nativeManualInputAvailable = false,
        .autoPlay = false,
        .inputHandlerAvailable = true,
        .practiceMode = true,
        .practiceRange = gameplay::GameplayTimeRange{
            .startMicros = 1'000'000, .endMicros = 2'000'000},
    });
require(!unsupportedManual.eligible,
        "manual practice keeps fallback on platforms without native input");

require(gameplay::shouldAttemptRealtimeGameplayReset(true, false, true) &&
            gameplay::shouldAttemptRealtimeGameplayReset(true, true, false) &&
            !gameplay::shouldAttemptRealtimeGameplayReset(false, true, true),
        "loop reset restarts only with a controller and an input authority");

require(gameplay::classifyRealtimeGameplayTerminal(
            gameplay::GameplayTerminalReason::PracticeComplete, true) ==
            gameplay::RealtimeGameplayTerminalAction::CompletePractice &&
            gameplay::classifyRealtimeGameplayTerminal(
                gameplay::GameplayTerminalReason::PracticeComplete, false) ==
                gameplay::RealtimeGameplayTerminalAction::IntegrityFailure,
        "PracticeComplete is accepted only for a session-backed attempt");
```

Run:

```bash
cmake --build cmake-build-debug --target realtime_gameplay_authority_policy_tests -j 6
```

Expected: compilation fails because `shouldAttemptRealtimeGameplayReset`,
`classifyRealtimeGameplayTerminal`, and `RealtimeGameplayTerminalAction` do not
exist.

- [ ] **Step 2: Implement the reset and terminal policy helpers**

Add to `RealtimeGameplayAuthorityPolicy.h`:

```cpp
enum class RealtimeGameplayTerminalAction {
  Wait,
  CompleteChart,
  CompletePractice,
  SurvivalGaugeFailed,
  IntegrityFailure,
};

[[nodiscard]] constexpr bool shouldAttemptRealtimeGameplayReset(
    bool laneControllerAvailable, bool inputHandlerAvailable,
    bool autoPlay) noexcept {
  return laneControllerAvailable && (inputHandlerAvailable || autoPlay);
}

[[nodiscard]] RealtimeGameplayTerminalAction
classifyRealtimeGameplayTerminal(GameplayTerminalReason reason,
                                 bool sessionBackedPractice) noexcept;
```

Add to `RealtimeGameplayAuthorityPolicy.cpp`:

```cpp
RealtimeGameplayTerminalAction classifyRealtimeGameplayTerminal(
    GameplayTerminalReason reason, bool sessionBackedPractice) noexcept {
  switch (reason) {
  case GameplayTerminalReason::None:
    return RealtimeGameplayTerminalAction::Wait;
  case GameplayTerminalReason::ChartComplete:
    return RealtimeGameplayTerminalAction::CompleteChart;
  case GameplayTerminalReason::PracticeComplete:
    return sessionBackedPractice
               ? RealtimeGameplayTerminalAction::CompletePractice
               : RealtimeGameplayTerminalAction::IntegrityFailure;
  case GameplayTerminalReason::SurvivalGaugeFailed:
    return RealtimeGameplayTerminalAction::SurvivalGaugeFailed;
  case GameplayTerminalReason::ReplayCapacityExceeded:
  case GameplayTerminalReason::AutomaticResultCapacityExceeded:
  case GameplayTerminalReason::GaugeHistoryCapacityExceeded:
    return RealtimeGameplayTerminalAction::IntegrityFailure;
  }
  return RealtimeGameplayTerminalAction::IntegrityFailure;
}
```

Run:

```bash
cmake --build cmake-build-debug --target realtime_gameplay_authority_policy_tests -j 6
./cmake-build-debug/realtime_gameplay_authority_policy_tests
```

Expected: build succeeds and the policy test exits with status 0.

- [ ] **Step 3: Apply the policy when starting and restarting the worker**

Include `RealtimeGameplayAuthorityPolicy.h` in `GamePlayScene.cpp`.

At the start of `startRealtimeGameplayAuthority()`, convert
`practiceNoteRange()` to `std::optional<gameplay::GameplayTimeRange>`, set
`nativeManualInputAvailable` from the iOS/Windows target macros, and build the
policy:

```cpp
std::optional<gameplay::GameplayTimeRange> realtimePracticeRange;
if (const auto range = practiceNoteRange(); range.has_value()) {
  realtimePracticeRange = gameplay::GameplayTimeRange{
      .startMicros = range->startMicros,
      .endMicros = range->endMicros,
  };
}
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR || TARGET_OS_WINDOWS
constexpr bool nativeManualInputAvailable = true;
#else
constexpr bool nativeManualInputAvailable = false;
#endif
const auto policy = gameplay::makeRealtimeGameplayAuthorityPolicy({
    .nativeManualInputAvailable = nativeManualInputAvailable,
    .autoPlay = options.autoPlay,
    .inputHandlerAvailable = inputHandler != nullptr,
    .replayPlayback = isReplayPlayback(),
    .practiceMode = options.practiceMode,
    .practiceRange = realtimePracticeRange,
    .startPositionMicros = getStartPositionMicros(),
    .preparationActivationSongTimeMicros =
        preparationPlan.laneIndicator.enabled()
            ? std::optional<std::int64_t>(getGameplayTimeMicros(
                  preparationPlan.laneIndicator.endTimeMicros))
            : std::nullopt,
});
```

Remove the leading platform-only early return and replace the existing replay,
practice, and manual-input exclusions with `!policy.eligible`, while retaining
the chart/state/renderer/already-active guards. Assign
`simulationConfig.allowedNoteRange = policy.allowedNoteRange`. In the
`RealtimeGameplayWorkerConfig` designated initializer, preserve declaration
order by assigning:

```cpp
.activationSongTimeMicros = policy.activationSongTimeMicros,
.practiceCompletionSongTimeMicros =
    policy.practiceCompletionSongTimeMicros,
```

At the end of `reset()`, use the tested reset policy:

```cpp
if (gameplay::shouldAttemptRealtimeGameplayReset(
        laneInputController != nullptr, inputHandler != nullptr,
        options.autoPlay)) {
  (void)startRealtimeGameplayAuthority();
}
```

- [ ] **Step 4: Let session-backed legacy fallback judge during count-in**

In both `pressLane()` and `releaseLane()`, narrow the visual-only preparation
branch to normal/legacy launches:

```cpp
if (gameplay::preparationInputUsesVisualOnlyPath(
        preparationIndicatorActive(rawSongTimeMicros),
        options.practiceSession != nullptr)) {
```

This sends session-backed fallback input through `RhythmLaneInputController`,
whose allowed note range already excludes notes before the selected start and
at the exclusive end.

- [ ] **Step 5: Consume PracticeComplete exactly once**

Declare this private method in `GamePlayScene.h`:

```cpp
void completePracticeSection(bool realtimeRangeFinalized);
```

Implement it in `GamePlayScene.cpp`:

```cpp
void GamePlayScene::completePracticeSection(bool realtimeRangeFinalized) {
  if (!realtimeRangeFinalized) {
    finalizePracticeRangeMisses();
  }
  state->isEnding = true;
  completePracticeAttempt();
  if (options.practiceSession->shouldLoop()) {
    reset();
  } else {
    scheduleResultTransition(0);
  }
}
```

Remove the local `completePracticeSection` lambda from `update()`. Classify the
worker terminal with the tested policy. Handle
`RealtimeGameplayTerminalAction::CompletePractice` before the integrity-failure
path:

```cpp
if (gameplay::classifyRealtimeGameplayTerminal(
        terminalReason, options.practiceSession != nullptr) ==
    gameplay::RealtimeGameplayTerminalAction::CompletePractice) {
  stopRealtimeGameplayAuthority(true);
  completePracticeSection(true);
  return;
}
```

Use `completePracticeSection(false)` for the legacy
`practiceSectionComplete` and end-of-chart practice branches.

- [ ] **Step 6: Build and run the complete three-task regression set**

Run:

```bash
cmake --build cmake-build-debug --target \
  realtime_gameplay_worker_tests \
  realtime_gameplay_authority_policy_tests \
  gameplay_automatic_authority_tests \
  gameplay_simulation_tests \
  gameplay_practice_input_boundary_tests \
  gameplay_playback_startup_tests \
  main -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(realtime_gameplay_worker_tests|realtime_gameplay_authority_policy_tests|gameplay_automatic_authority_tests|gameplay_simulation_tests|gameplay_practice_input_boundary_tests|gameplay_playback_startup_tests)$'
```

Expected: all targets build and all six focused CTest entries pass.

- [ ] **Step 7: Review the complete three-task delta**

Run:

```bash
git diff 27d73c0 --check
git diff --stat 27d73c0
git diff 27d73c0 -- \
  src/scene/play/RealtimeGameplayWorker.h \
  src/scene/play/RealtimeGameplayWorker.cpp \
  src/scene/play/RealtimeGameplayAuthorityPolicy.h \
  src/scene/play/RealtimeGameplayAuthorityPolicy.cpp \
  src/scene/play/GamePlayScene.h \
  src/scene/play/GamePlayScene.cpp \
  tests/realtime_gameplay_worker_tests.cpp \
  tests/realtime_gameplay_authority_policy_tests.cpp \
  CMakeLists.txt src/scene/play/CMakeLists.txt
```

Confirm all of the following from the diff:

- normal non-practice activation gating is unchanged;
- session-backed practice carries the exact end boundary;
- autoplay loop reset does not require an input handler;
- `PracticeComplete` transfers worker state before session completion;
- legacy finalization is skipped only when the worker already finalized;
- replay playback and legacy sessionless practice remain excluded;
- no physical/touch device is claimed by autoplay.

- [ ] **Step 8: Commit Task 3**

```bash
git add src/scene/play/GamePlayScene.h \
        src/scene/play/GamePlayScene.cpp \
        src/scene/play/RealtimeGameplayAuthorityPolicy.h \
        src/scene/play/RealtimeGameplayAuthorityPolicy.cpp \
        tests/realtime_gameplay_authority_policy_tests.cpp
git commit -m "fix: run practice sessions on realtime authority"
```

- [ ] **Step 9: Run final desktop and iOS verification**

Run:

```bash
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(realtime_gameplay_worker_tests|realtime_gameplay_authority_policy_tests|gameplay_automatic_authority_tests|gameplay_simulation_tests|gameplay_practice_input_boundary_tests|gameplay_playback_startup_tests|foundation_av_audio_mix)$'
cmake --build cmake-build-debug --target main -j 6
scripts/ios_firebase_deploy.sh --build-only --skip-init
git status --short
```

Expected: seven focused tests pass, the desktop app target builds, the iOS
device build ends with `** BUILD SUCCEEDED **`, and `git status --short` is
empty. Do not upload or deploy the iOS build.
