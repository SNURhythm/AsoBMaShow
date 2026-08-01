# Gameplay Automatic Authority Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `GameplaySimulation` a complete parser-independent synchronous authority for judgement totals, score, combo, gauge, replay events, automatic note deadlines, landmines, Hell Charge ticks, practice finalization, and terminal outcome without connecting it to live gameplay yet.

**Architecture:** Chart construction compiles pointer-free metadata plus one globally chronological stable-note index. `GameplaySimulation` owns the existing gauge/scoring algorithm through a parser-free shared state type, advances automatic deadlines on the same serialized timeline as input, and records complete post-transaction replay events in preallocated storage. The current `GamePlayScene` path remains authoritative until a later worker/audio/snapshot plan performs one explicit cutover.

**Tech Stack:** C++23, CMake/CTest, existing `GameplayDefinition`, `GameplaySimulation`, `CompiledGameplayJudge`, `RhythmState` gauge algorithms, parser chart as construction-only input.

## Global Constraints

- The approved design is `docs/superpowers/specs/2026-07-17-asynchronous-gameplay-input-design.md`.
- The gameplay worker will be the only writer of timing-sensitive gameplay state; this subproject must leave one complete synchronous state owner ready for that worker.
- Input and automatic transactions must use one monotonic song-time ordering. Before applying an input at time `T`, automatic work through `T` is committed using the prior lane state.
- Parser chart objects are read-only construction inputs. New runtime state contains no parser pointer and never mutates `bms_parser::Note`.
- Transaction and `advanceTo()` methods allocate only during construction, take no general-purpose lock, and call no SDL, bgfx, renderer, `Jukebox`, or audio API.
- Replay events contain post-transaction gauge, gauge type, combo, and score values.
- `GameplaySimulation` remains non-authoritative in production throughout this plan. Do not connect it to `GamePlayScene` or native input yet.
- Do not edit `src/bms_parser.hpp` or `src/bms_parser.cpp`; they are generated from `../bms-parser-cpp`.
- Add no third-party dependency and keep shared runtime code portable C++23.
- Every task follows test-first order and ends in its own commit.
- Final verification is full CTest, `cmake --build cmake-build-debug --target main -j 6`, and `scripts/ios_firebase_deploy.sh --build-only`; none of these commands authorize deployment.

## Scope Boundary

This plan completes the synchronous deterministic authority required by migration step 1. It does not add worker threads, native callbacks, realtime audio handles, fixed ingress/audio queues, clock anchors, read-model triple buffering, UIKit/bgfx thread separation, or live authority selection. Those begin only after this state passes the completion gate below.

Touch samples and lane-cover replay events remain UI/coordinator-owned because they do not decide note ownership, judgement, sound, score, gauge, or outcome.

Replay-file playback ingestion is outside this subproject. Until the later production cutover, replay attempts continue using the legacy authority; the cutover must choose exactly one authority for each attempt and must never feed one replay input to both.

## File Map

- Create `src/scene/play/GameplayScoreState.h`: parser-free score, combo, gauge, clear-state, and fast/slow implementation shared by live `RhythmState` and `GameplaySimulation`.
- Modify `src/scene/play/RhythmState.h`: thin parser adapter deriving from `GameplayScoreState`; existing callers retain the `RhythmState` API.
- Modify `src/scene/play/GameplayDefinition.{h,cpp}`: pointer-free chart metadata and globally chronological stable note IDs.
- Modify `src/scene/play/GameplaySimulation.{h,cpp}`: attempt state, replay accumulation, automatic deadlines, HCN balances, practice finalization, terminal snapshot.
- Modify `tests/gameplay_simulation_tests.cpp`: preserve direct-input and current-controller parity coverage with post-state assertions.
- Create `tests/gameplay_automatic_authority_tests.cpp`: automatic deadline, mine, HCN, practice, score/gauge/replay, scheduling-chunk, and terminal tests.
- Modify `CMakeLists.txt` and `src/scene/play/CMakeLists.txt`: build the new focused test and shared sources.

---

### Task 1: Extract Parser-Free Score and Gauge State

**Files:**

- Create: `src/scene/play/GameplayScoreState.h`
- Modify: `src/scene/play/RhythmState.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Create: `tests/gameplay_score_state_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Consumes: existing gauge constants/functions and `RhythmState` behavior.
- Produces: `GameplayScoreConfig`, `GameplayScoreState`, and the unchanged derived `RhythmState` API.

- [ ] **Step 1: Write the failing parser-independence and parity test**

Create `tests/gameplay_score_state_tests.cpp`:

```cpp
#include "scene/play/GameplayScoreState.h"
#include "scene/play/RhythmState.h"

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void requireSame(const GameplayScoreState &left,
                 const GameplayScoreState &right) {
  require(left.judgeCount == right.judgeCount, "judge counts match");
  require(left.judgementFastSlowCount == right.judgementFastSlowCount,
          "fast/slow counts match");
  require(left.combo == right.combo && left.maxCombo == right.maxCombo &&
              left.comboBreak == right.comboBreak,
          "combo state matches");
  require(left.gaugeType == right.gaugeType &&
              left.gaugeValues == right.gaugeValues &&
              left.gaugeSurvivalFailed == right.gaugeSurvivalFailed &&
              std::bit_cast<std::uint32_t>(left.currentGauge) ==
                  std::bit_cast<std::uint32_t>(right.currentGauge),
          "gauge state is bit-identical");
}
} // namespace

int main() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 432;
  chart.Meta.KeyMode = 7;
  chart.Meta.HasTotal = true;
  chart.Meta.Total = 280.0;

  RhythmState legacy(&chart, false);
  GameplayScoreState runtime({.totalNotes = 432,
                              .keyMode = 7,
                              .gaugeTotal = 280.0});
  legacy.configureGauge(GaugeType::Hard, GaugeAutoShiftMode::BestClear);
  runtime.configureGauge(GaugeType::Hard, GaugeAutoShiftMode::BestClear);
  legacy.configureBoundedGaugeHistory(64);
  runtime.configureBoundedGaugeHistory(64);

  for (const auto judge : {PGreat, Great, Good, Bad, Poor, Kpoor}) {
    legacy.commitJudge(JudgeResult(judge, judge == PGreat ? -10 : 10));
    runtime.commitJudge(JudgeResult(judge, judge == PGreat ? -10 : 10));
  }
  legacy.applyGaugeJudgementRate(Great, 0.5F);
  runtime.applyGaugeJudgementRate(Great, 0.5F);
  legacy.applyGaugeDelta(-3.25F);
  runtime.applyGaugeDelta(-3.25F);
  requireSame(legacy, runtime);
  require(legacy.getScore() == runtime.getScore(), "EX score matches");
  require(legacy.getClearTypeRank() == runtime.getClearTypeRank(),
          "clear state matches");
  return 0;
}
```

- [ ] **Step 2: Register and run the RED build**

Add a `gameplay_score_state_tests` executable to root `CMakeLists.txt` with `tests/gameplay_score_state_tests.cpp` and `src/bms_parser.cpp`, project include paths, and C++23, then run:

```bash
cmake --build cmake-build-debug --target gameplay_score_state_tests -j 6
```

Expected: compilation fails because `GameplayScoreState.h`, `configureBoundedGaugeHistory()`, and `commitJudge()` do not exist.

- [ ] **Step 3: Mechanically move the shared implementation**

Copy the current contents of `src/scene/play/RhythmState.h` to `src/scene/play/GameplayScoreState.h`, then make these exact structural changes:

```cpp
// GameplayScoreState.h includes
#pragma once
#include "../../audio/PlaybackRate.h"
#include "Judgement.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

struct GameplayScoreConfig {
  int totalNotes = 0;
  int keyMode = 7;
  double gaugeTotal = 100.0;
};

class GameplayScoreState {
public:
  explicit GameplayScoreState(GameplayScoreConfig config)
      : gaugeTotalNotes(config.totalNotes), gaugeKeyMode(config.keyMode),
        gaugeTotal(config.gaugeTotal) {
    resetJudgeCounts();
    configureGauge(GaugeType::Normal, GaugeAutoShiftMode::None);
  }

  void configureBoundedGaugeHistory(std::size_t capacity) {
    gaugeHistory.reserve(capacity);
    boundedGaugeHistory_ = true;
  }

  void commitJudge(const JudgeResult &judgeResult) {
    ++judgeCount[judgeResult.judgement];
    if (judgeResult.isComboBreak()) {
      combo = 0;
      ++comboBreak;
    } else if (judgeResult.judgement != Kpoor) {
      ++combo;
      maxCombo = std::max(maxCombo, combo);
    }
    recordFastSlow(judgeResult);
    applyGaugeJudgement(judgeResult.judgement);
  }
};
```

Rename the original `class RhythmState` body to `class GameplayScoreState`, change `addJudgeCountFrom(const RhythmState &)` to `addJudgeCountFrom(const GameplayScoreState &)`, replace its chart constructor with the configuration constructor above, and rename its destructor.

Add `bool operator==(const JudgementFastSlowCount &) const = default;`. Route every existing `gaugeHistory.push_back(...)` through a private `recordGaugeHistory(float)` helper. In normal `RhythmState` compatibility mode it keeps the current growable behavior; after `configureBoundedGaugeHistory(capacity)` it pushes only while `size() < capacity()` and latches `gaugeHistoryOverflowed_` instead of allocating. Expose `gaugeHistoryOverflowed()` for the simulation integrity gate.

Replace `src/scene/play/RhythmState.h` with the compatibility adapter:

```cpp
#pragma once
#include "../../bms_parser.hpp"
#include "GameplayScoreState.h"

class RhythmState : public GameplayScoreState {
public:
  explicit RhythmState(const bms_parser::Chart *chart, bool addReadyMeasure)
      : GameplayScoreState(configFor(chart)) {
    (void)addReadyMeasure;
  }

private:
  static GameplayScoreConfig configFor(const bms_parser::Chart *chart) {
    if (chart == nullptr) {
      return {};
    }
    return {
        .totalNotes = chart->Meta.TotalNotes,
        .keyMode = chart->Meta.KeyMode,
        .gaugeTotal =
            chart->Meta.HasTotal
                ? chart->Meta.Total
                : beatorajaDefaultGaugeTotal(chart->Meta.KeyMode,
                                             chart->Meta.TotalNotes),
    };
  }
};
```

- [ ] **Step 4: Remove duplicated scene scoring logic**

Change only `GamePlayScene::onJudge()`'s state mutation prefix to call the shared method, retaining renderer/pacemaker side effects:

```cpp
const int previousCount = state->judgeCount[judgeResult.judgement];
state->commitJudge(judgeResult);
const int judgementCount = previousCount + 1;
```

Delete the old inline judge-count/combo/fast-slow/gauge mutations from `onJudge()` so they are not applied twice.

- [ ] **Step 5: Run focused and compatibility tests**

```bash
cmake --build cmake-build-debug --target gameplay_score_state_tests practice_rule_override_tests main -j 6
ctest --test-dir cmake-build-debug -R '^(gameplay_score_state_tests|practice_rule_override_tests)$' --output-on-failure
```

Expected: 2/2 tests pass and `main` builds.

- [ ] **Step 6: Commit parser-free scoring state**

```bash
git add CMakeLists.txt src/scene/play/GameplayScoreState.h src/scene/play/RhythmState.h src/scene/play/GamePlayScene.cpp tests/gameplay_score_state_tests.cpp
git commit -m "refactor: share parser-free gameplay score state"
```

---

### Task 2: Compile Chart Metadata and Chronological Note IDs

**Files:**

- Modify: `src/scene/play/GameplayDefinition.h`
- Modify: `src/scene/play/GameplayDefinition.cpp`
- Modify: `tests/gameplay_automatic_authority_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Consumes: construction-only `const bms_parser::Chart &`.
- Produces: `GameplayChartMetadata metadata()` and `chronologicalNotes()` sorted by `(timingMicros, NoteId)`.

- [ ] **Step 1: Write the failing definition test**

Create `tests/gameplay_automatic_authority_tests.cpp` with the existing chart helpers used by `gameplay_simulation_tests.cpp`, then add:

```cpp
void testDefinitionCompilesAutomaticMetadata() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 2;
  chart.Meta.KeyMode = 7;
  chart.Meta.HasTotal = true;
  chart.Meta.Total = 260.0;
  auto *measure = new bms_parser::Measure();
  auto *late = addTimeline(*measure, 2'000'000);
  late->SetNote(1, new bms_parser::Note(20));
  auto *early = addTimeline(*measure, 1'000'000);
  early->SetNote(2, new bms_parser::Note(10));
  early->SetLandmineNote(3, new bms_parser::LandmineNote(3.5F));
  addTimeline(*measure, 3'000'000); // empty timing still delays chart completion
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  const auto metadata = definition.metadata();
  require(metadata.totalNotes == 2 && metadata.keyMode == 7 &&
              metadata.gaugeTotal == 260.0,
          "chart gauge metadata is copied");
  const auto chronological = definition.chronologicalNotes();
  require(chronological.size() == 3, "all note identities are scheduled");
  require(definition.note(chronological[0]).timingMicros == 1'000'000 &&
              definition.note(chronological[1]).timingMicros == 1'000'000 &&
              definition.note(chronological[2]).timingMicros == 2'000'000,
          "automatic identities are chronological and stable");
  require(metadata.finalTimelineTimeMicros == 3'000'000,
          "empty final timelines remain part of completion timing");
}
```

- [ ] **Step 2: Run RED**

Register `gameplay_automatic_authority_tests` with `GameplayDefinition.cpp`, `GameplaySimulation.cpp`, `CompiledGameplayJudge.cpp`, `Judge.cpp`, and `src/bms_parser.cpp`, then run:

```bash
cmake --build cmake-build-debug --target gameplay_automatic_authority_tests -j 6
```

Expected: compilation fails because the metadata and chronological APIs are missing.

- [ ] **Step 3: Add immutable metadata/index**

Add to `GameplayDefinition.h`:

```cpp
struct GameplayChartMetadata {
  int totalNotes = 0;
  int keyMode = 7;
  double gaugeTotal = 100.0;
  std::int64_t finalNoteTimeMicros = 0;
  std::int64_t finalTimelineTimeMicros = 0;
};

[[nodiscard]] GameplayChartMetadata metadata() const noexcept;
[[nodiscard]] std::span<const NoteId> chronologicalNotes() const noexcept;
[[nodiscard]] std::span<const NoteId> hellChargeHeads() const noexcept;

GameplayChartMetadata metadata_;
std::vector<NoteId> chronologicalNoteIds_;
std::vector<NoteId> hellChargeHeadIds_;
```

In `buildGameplayDefinition()` copy `TotalNotes`, `KeyMode`, and the exact `RhythmState` gauge-total rule. After all IDs and long-note pairs are compiled, fill every ID once and sort:

```cpp
definition.chronologicalNoteIds_.resize(definition.notes_.size());
std::iota(definition.chronologicalNoteIds_.begin(),
          definition.chronologicalNoteIds_.end(), NoteId{0});
std::ranges::sort(definition.chronologicalNoteIds_,
                  [&](NoteId left, NoteId right) {
                    return std::pair{definition.notes_[left].timingMicros, left} <
                           std::pair{definition.notes_[right].timingMicros, right};
                  });
if (!definition.chronologicalNoteIds_.empty()) {
  definition.metadata_.finalNoteTimeMicros =
      definition.notes_[definition.chronologicalNoteIds_.back()].timingMicros;
}
```

While traversing timelines, update `finalTimelineTimeMicros` even when a timeline has no note. After pairing, populate `hellChargeHeadIds_` with only `LongHead` definitions whose resolved rule is `HellCharge`, in chronological-ID order. This prevents the hot HCN integrator from scanning every chart note.

- [ ] **Step 4: Run GREEN and existing definition coverage**

```bash
cmake --build cmake-build-debug --target gameplay_automatic_authority_tests gameplay_simulation_tests -j 6
ctest --test-dir cmake-build-debug -R '^(gameplay_automatic_authority_tests|gameplay_simulation_tests)$' --output-on-failure
```

Expected: 2/2 pass.

- [ ] **Step 5: Commit immutable automatic metadata**

```bash
git add CMakeLists.txt src/scene/play/GameplayDefinition.h src/scene/play/GameplayDefinition.cpp tests/gameplay_automatic_authority_tests.cpp
git commit -m "feat: compile automatic gameplay metadata"
```

---

### Task 3: Own Score, Gauge, and Complete Replay Transactions

**Files:**

- Modify: `src/scene/play/GameplaySimulation.h`
- Modify: `src/scene/play/GameplaySimulation.cpp`
- Modify: `tests/gameplay_simulation_tests.cpp`
- Modify: `tests/gameplay_automatic_authority_tests.cpp`

**Interfaces:**

- Consumes: `GameplayScoreState`, immutable metadata, direct press/release results.
- Produces: `GameplayAttemptOptions`, `GameplayAttemptSnapshot`, `scoreState()`, `replayEvents()`, and post-state replay payloads.

- [ ] **Step 1: Write failing transactional state tests**

Add tests that construct one normal note, press it at PGreat, release an empty lane, and assert:

```cpp
const auto press = simulation.pressLane(1, contextAt(1'000'000));
require(press.hasJudge && simulation.scoreState().getScore() == 2,
        "accepted press commits EX score before return");
require(simulation.scoreState().combo == 1,
        "accepted press commits combo before return");
require(press.replayEvent.score == 2 && press.replayEvent.combo == 1 &&
            press.replayEvent.gauge == simulation.scoreState().currentGauge,
        "replay payload snapshots post-transaction state");
require(simulation.replayEvents().size() == 1 &&
            simulation.replayEvents().front() == press.replayEvent,
        "simulation owns replay accumulation");
```

Add a Bad/Poor combo-break sequence and compare the full `GameplayScoreState` with a standalone state receiving the same `commitJudge()` calls.

- [ ] **Step 2: Run RED**

```bash
cmake --build cmake-build-debug --target gameplay_simulation_tests gameplay_automatic_authority_tests -j 6
```

Expected: compilation fails because attempt state/replay APIs and replay post-state fields are missing.

- [ ] **Step 3: Define attempt options and snapshots**

Add to `GameplaySimulation.h`:

```cpp
struct GameplayAttemptOptions {
  GaugeType initialGaugeType = GaugeType::Normal;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  GaugeProfile gaugeProfile = GaugeProfile::Standard;
  GaugeType gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
  std::optional<int> startingGaugePercent;
  std::optional<GaugeStateSnapshot> carriedGauge;
  int carriedCombo = 0;
  int carriedMaxCombo = 0;
  bool assistClearMark = false;
  bool autoPlay = false;
  std::size_t replayCapacity = 4096;
  std::size_t automaticResultCapacity = 4096;
};

struct GameplayAttemptSnapshot {
  std::array<int, JudgementCount> judgeCounts{};
  int combo = 0;
  int maxCombo = 0;
  int comboBreak = 0;
  int score = 0;
  float gauge = 0.0F;
  GaugeType gaugeType = GaugeType::Normal;
  int clearTypeRank = kClearTypeFailedRank;
};
```

Append `GameplayAttemptOptions attempt;` to `GameplaySimulationConfig` so existing aggregate initializers retain their defaults.

Expand `GameplayReplayEvent` with:

```cpp
float gauge = 0.0F;
GaugeType gaugeType = GaugeType::Normal;
int combo = 0;
int score = 0;
bool operator==(const GameplayReplayEvent &) const = default;
```

Add accessors:

```cpp
[[nodiscard]] const GameplayScoreState &scoreState() const noexcept;
[[nodiscard]] GameplayAttemptSnapshot snapshot() const noexcept;
[[nodiscard]] std::span<const GameplayReplayEvent> replayEvents() const noexcept;
[[nodiscard]] bool replayOverflowed() const noexcept;
```

- [ ] **Step 4: Commit score/replay inside every input transaction**

Construct `scoreState_` from `definition.metadata()`, configure/restore gauge and carried combo, then reserve both replay storage and gauge history during construction.

Add helpers:

```cpp
void GameplaySimulation::commitJudge(const JudgeResult &judge) {
  scoreState_.commitJudge(judge);
}

bool GameplaySimulation::recordReplay(GameplayReplayEvent &event) {
  event.gauge = scoreState_.currentGauge;
  event.gaugeType = scoreState_.gaugeType;
  event.combo = scoreState_.combo;
  event.score = scoreState_.getScore();
  if (replayEvents_.size() == replayEvents_.capacity()) {
    replayOverflowed_ = true;
    return false;
  }
  replayEvents_.push_back(event);
  return true;
}
```

For press/release, apply `commitJudge()` when `hasJudge` is true, then populate/record the replay event. Empty lane events are recorded without a judge and capture unchanged post-state. Do not alter the note/sound atomicity order.

- [ ] **Step 5: Run GREEN and full direct-input parity**

```bash
cmake --build cmake-build-debug --target gameplay_simulation_tests gameplay_automatic_authority_tests -j 6
ctest --test-dir cmake-build-debug -R '^(gameplay_simulation_tests|gameplay_automatic_authority_tests)$' --output-on-failure
```

Expected: 2/2 pass, including current-controller note/sound/judge/replay identity parity and new post-state assertions.

- [ ] **Step 6: Commit attempt state ownership**

```bash
git add src/scene/play/GameplaySimulation.h src/scene/play/GameplaySimulation.cpp tests/gameplay_simulation_tests.cpp tests/gameplay_automatic_authority_tests.cpp
git commit -m "feat: own gameplay score and replay transactions"
```

---

### Task 4: Advance Normal Notes, Landmines, and Autoplay Chronologically

**Files:**

- Modify: `src/scene/play/GameplaySimulation.h`
- Modify: `src/scene/play/GameplaySimulation.cpp`
- Modify: `tests/gameplay_automatic_authority_tests.cpp`

**Interfaces:**

- Consumes: chronological IDs, compiled judge, lane state, score/replay transaction helpers.
- Produces: `advanceTo(songTimeMicros, visualTimeMicros)` and `automaticResults()`.

- [ ] **Step 1: Write failing deadline/order tests**

Add tests for these exact cases:

1. A normal note is untouched at `timing + latePoor`, then becomes one Poor at `timing + latePoor + 1`; repeated advance is idempotent.
2. A landmine at time `T` detonates when its lane was pressed before `advanceTo(T)`, applies exactly `-mineDamage`, and records one Mine event; otherwise it expires without replay/gauge change.
3. An input passed through `applyPressAt(T)` first calls `advanceTo(T)`, so a mine at `T` observes the prior lane state.
4. Autoplay at timing commits the normal note, PGreat, sound identity, lane press/release visuals, score, gauge, and replay exactly once.
5. Advancing directly across 1,000 unrelated-lane notes uses the global cursor once and repeated advance examines zero completed identities.

- [ ] **Step 2: Run RED**

```bash
cmake --build cmake-build-debug --target gameplay_automatic_authority_tests -j 6
ctest --test-dir cmake-build-debug -R '^gameplay_automatic_authority_tests$' --output-on-failure
```

Expected: compilation fails because automatic advance APIs are missing.

- [ ] **Step 3: Add fixed automatic result storage and cursors**

Add:

```cpp
struct GameplayAdvanceResult {
  std::span<const GameplayInputResult> transactions;
  std::int64_t advancedToMicros = 0;
};

GameplayAdvanceResult advanceTo(std::int64_t songTimeMicros,
                                std::int64_t visualTimeMicros);
GameplayInputResult applyPressAt(int mainLane, int compensateLane,
                                 const GameplayInputContext &context);
GameplayInputResult applyReleaseAt(int lane,
                                   const GameplayInputContext &context,
                                   bool isBackSpin = false);
```

Reserve `automaticResults_` in the constructor and clear without shrinking on every `advanceTo()`. Add separate chronological cursors for at-timing and late-poor phases. The exact late deadline is:

```cpp
note.timingMicros + config_.judge.latePoorTimingMicros() + 1
```

Process the next smallest `(deadline time, phase order, NoteId)` until it exceeds the target time. At equal time, process at-timing work before late-poor work, preserving stable IDs inside each phase.

- [ ] **Step 4: Implement normal/mine/autoplay transactions**

- Mine at timing: if lane pressed, set played/dead/time, apply `-mineDamage`, record Mine post-state; otherwise set dead/time only.
- Normal late-poor: mark played/dead/time, commit `JudgeResult(Poor, targetDeadline - note.timingMicros)`, record Miss.
- Autoplay normal at timing: commit a note-specific press transaction with PGreat and sound, then a lane-only release visual without a second judgement or sound.
- Manual normal notes do nothing at timing and wait for input or late-poor.
- If automatic result capacity is exhausted, latch `automaticResultOverflowed_`; do not grow the vector in `advanceTo()`.

- [ ] **Step 5: Run GREEN and scheduling mutation checks**

```bash
cmake --build cmake-build-debug --target gameplay_automatic_authority_tests gameplay_simulation_tests -j 6
ctest --test-dir cmake-build-debug -R '^(gameplay_automatic_authority_tests|gameplay_simulation_tests)$' --output-on-failure
```

Temporarily change the strict late comparison to trigger at `+ latePoor` and confirm the boundary test fails; restore it and confirm GREEN.

- [ ] **Step 6: Commit automatic normal/mine advancement**

```bash
git add src/scene/play/GameplaySimulation.h src/scene/play/GameplaySimulation.cpp tests/gameplay_automatic_authority_tests.cpp
git commit -m "feat: advance automatic gameplay deadlines"
```

---

### Task 5: Advance Long Notes and Finalize Practice Ranges

**Files:**

- Modify: `src/scene/play/GameplaySimulation.h`
- Modify: `src/scene/play/GameplaySimulation.cpp`
- Modify: `tests/gameplay_automatic_authority_tests.cpp`
- Modify: `tests/gameplay_practice_input_boundary_tests.cpp`

**Interfaces:**

- Consumes: long-note rule/pair IDs, automatic deadline engine, allowed range.
- Produces: exact Classic/Charge/HellCharge automatic misses/releases and `finalizePracticeRange()`.

- [ ] **Step 1: Write the failing long-note matrix**

Cover and assert full note state, judgement count, combo/gauge, and replay identity:

- Unpressed Classic head late: head and in-range tail become played/dead, exactly one Poor for the head.
- Unpressed Charge and HellCharge head late: head and tail each produce one Poor; an early future tail is played but not dead.
- Held Classic tail at timing auto-releases and combines head/tail judge; manual Charge/HellCharge remains holding until release or late Poor.
- Autoplay Charge/HellCharge tail auto-releases at timing.
- `initializeAt(startMicros)` marks every identity before the attempt start as resolved without judgement/replay, preserves crossing Classic heads/tails, and positions both automatic cursors so pre-start deadlines never replay later.
- Practice end is half-open, ignores mines, preserves a Classic head whose tail crosses out of range, counts Charge identities separately, and is idempotent.

- [ ] **Step 2: Run RED**

```bash
cmake --build cmake-build-debug --target gameplay_automatic_authority_tests gameplay_practice_input_boundary_tests -j 6
ctest --test-dir cmake-build-debug -R '^(gameplay_automatic_authority_tests|gameplay_practice_input_boundary_tests)$' --output-on-failure
```

Expected: new long-note/finalization assertions fail.

- [ ] **Step 3: Implement stable-ID long-note state helpers**

Add private helpers that never follow parser pointers:

```cpp
void markMissed(NoteId id, std::int64_t judgeTimeMicros, bool dead);
void clearPairHolding(NoteId id);
GameplayInputResult commitMiss(NoteId id, std::int64_t songTimeMicros,
                               std::int64_t judgeTimeMicros);
GameplayInputResult commitAutomaticRelease(NoteId tailId,
                                            std::int64_t songTimeMicros,
                                            std::int64_t visualTimeMicros);
```

Implement the matrix exactly as `GamePlayScene::checkPassedTimeline()` and `PracticeNoteFinalizer.h` currently behave. Use `pairId`, `longNoteRule`, and `timingMicros`; do not access parser notes.

Add `void initializeAt(std::int64_t startMicros)` and call it once from construction when `allowedNoteRange.startMicros > 0`. It must resolve pre-start normal notes, mines, and both long-note identities without score/gauge/replay side effects, preserve the crossing-long-note rules exercised by the existing start-position code, and move both global deadline cursors past obsolete work.

- [ ] **Step 4: Add practice finalization**

```cpp
GameplayAdvanceResult finalizePracticeRange(std::int64_t finalizationTimeMicros,
                                            std::int64_t visualTimeMicros);
```

Reject a repeated call. Only definitions whose timing is within `[startMicros,endMicros)` participate. Mine identities never become misses. Record one complete post-state replay event per judged identity.

- [ ] **Step 5: Run GREEN and old-finalizer parity**

```bash
cmake --build cmake-build-debug --target gameplay_automatic_authority_tests gameplay_practice_input_boundary_tests -j 6
ctest --test-dir cmake-build-debug -R '^(gameplay_automatic_authority_tests|gameplay_practice_input_boundary_tests)$' --output-on-failure
```

Expected: 2/2 pass. Keep the existing parser-finalizer tests as the old-authority oracle.

- [ ] **Step 6: Commit long-note/practice authority**

```bash
git add src/scene/play/GameplaySimulation.h src/scene/play/GameplaySimulation.cpp tests/gameplay_automatic_authority_tests.cpp tests/gameplay_practice_input_boundary_tests.cpp
git commit -m "feat: own long note gameplay deadlines"
```

---

### Task 6: Integrate Hell Charge Gauge Time on the Serial Timeline

**Files:**

- Modify: `src/scene/play/GameplaySimulation.h`
- Modify: `src/scene/play/GameplaySimulation.cpp`
- Modify: `tests/gameplay_automatic_authority_tests.cpp`

**Interfaces:**

- Consumes: HellCharge head/tail definitions, lane/holding state, monotonic `advanceTo()`.
- Produces: fixed per-head balances and complete Gauge replay transactions.

- [ ] **Step 1: Write failing interval/chunking tests**

Use a HellCharge body from 1,000,000 to 2,000,000 microseconds and assert:

- Holding/gaining for exactly 200,000 microseconds emits no tick; one additional microsecond emits one half-rate Great tick.
- Released/not-held for exactly -200,000 emits no tick; one additional microsecond emits one half-rate Bad tick.
- Advance `[start,end]` in one call and in irregular chunks produces bit-identical gauge, balance, and replay events.
- A press/release at `T` advances HCN through `T` using the prior held state, then changes state for later intervals.
- Regrab via lane pressed gains even when the long-note holding flag is false.
- An early-resolved tail continues until its timing when it is played but not dead.

- [ ] **Step 2: Run RED**

```bash
cmake --build cmake-build-debug --target gameplay_automatic_authority_tests -j 6
ctest --test-dir cmake-build-debug -R '^gameplay_automatic_authority_tests$' --output-on-failure
```

Expected: HCN assertions fail because balances/ticks are not owned by the simulation.

- [ ] **Step 3: Allocate fixed balances during construction**

Add `std::vector<std::int64_t> hellChargeBalanceMicros_` sized to `definition.noteCount()`, plus `lastAdvancedMicros_`. Iterate only `definition.hellChargeHeads()`; never rescan `definition.notes_` or parser chart structures. Only those compiled LongHead identities use their indexed balance.

Before processing point deadlines in `advanceTo(target)`, integrate every HCN overlap from `lastAdvancedMicros_` to `target` in chronological segments bounded by automatic deadlines and the target. For each active head:

```cpp
const bool gaining = headState.holding || lanePressed(head.lane) ||
                     config_.attempt.autoPlay;
balance += gaining ? activeDelta : -activeDelta;
while (balance > 200'000) { balance -= 200'000; commitGaugeTick(Great); }
while (balance < -200'000) { balance += 200'000; commitGaugeTick(Bad); }
```

`commitGaugeTick()` applies `scoreState_.applyGaugeJudgementRate(judge, 0.5F)` and records a Gauge event with `lane=-1`, invalid note ID, and complete post-state. It does not change judge counts or combo.

- [ ] **Step 4: Run GREEN and mutation proof**

```bash
cmake --build cmake-build-debug --target gameplay_automatic_authority_tests -j 6
ctest --test-dir cmake-build-debug -R '^gameplay_automatic_authority_tests$' --output-on-failure
```

Temporarily change `>` to `>=` for the positive threshold and confirm the exact-boundary test fails; restore and confirm GREEN.

- [ ] **Step 5: Commit HCN serial time ownership**

```bash
git add src/scene/play/GameplaySimulation.h src/scene/play/GameplaySimulation.cpp tests/gameplay_automatic_authority_tests.cpp
git commit -m "feat: own hell charge gauge timing"
```

---

### Task 7: Freeze Terminal Outcomes and Scheduling-Independent Parity

**Files:**

- Modify: `src/scene/play/GameplaySimulation.h`
- Modify: `src/scene/play/GameplaySimulation.cpp`
- Modify: `tests/gameplay_automatic_authority_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Consumes: complete score/gauge/replay/deadline state.
- Produces: explicit `GameplayTerminalReason`, immutable terminal snapshot, and final replay summary.

- [ ] **Step 1: Write failing terminal and scheduling tests**

Add:

```cpp
enum class GameplayTerminalReason {
  None,
  ChartComplete,
  PracticeComplete,
  SurvivalGaugeFailed,
  ReplayCapacityExceeded,
  AutomaticResultCapacityExceeded,
  GaugeHistoryCapacityExceeded,
};
```

Tests must prove:

- Survival gauge reaches zero on the transaction that causes it and latches `SurvivalGaugeFailed`; later input/advance cannot mutate state.
- All chronological identities resolved past `metadata.finalTimelineTimeMicros + latePoor + 1` latch `ChartComplete` exactly once, including charts whose last timeline contains no notes.
- `finalizePracticeRange()` latches `PracticeComplete` after its final miss transaction.
- Replay/result/gauge-history capacity exhaustion latches the matching integrity terminal reason without growing storage.
- One large `advanceTo()` and a deterministic series of small advances plus identical inputs produce equal note states, lane states, score/gauge snapshot, replay events, HCN balances, and terminal reason.
- Final summary exposes score, max combo, final gauge, clear rank, and full-combo promotion inputs without accessing parser or scene objects.

- [ ] **Step 2: Run RED**

```bash
cmake --build cmake-build-debug --target gameplay_automatic_authority_tests -j 6
ctest --test-dir cmake-build-debug -R '^gameplay_automatic_authority_tests$' --output-on-failure
```

Expected: terminal APIs/assertions fail.

- [ ] **Step 3: Add immutable terminal state**

Add accessors:

```cpp
[[nodiscard]] GameplayTerminalReason terminalReason() const noexcept;
[[nodiscard]] bool terminal() const noexcept;
[[nodiscard]] GameplayAttemptSnapshot terminalSnapshot() const noexcept;
```

Latch the first terminal reason only. After latching, press/release/advance return empty results and do not change state. Store the terminal snapshot at the same transaction boundary. External effects—stopping audio, course-session writes, persistence, and scene transitions—remain consumers outside the simulation.

- [ ] **Step 4: Run complete focused gate**

```bash
cmake --build cmake-build-debug --target gameplay_score_state_tests gameplay_simulation_tests gameplay_automatic_authority_tests gameplay_practice_input_boundary_tests logical_gameplay_input_tests main -j 6
ctest --test-dir cmake-build-debug -R '^(gameplay_score_state_tests|gameplay_simulation_tests|gameplay_automatic_authority_tests|gameplay_practice_input_boundary_tests|foundation_input_gameplay)$' --output-on-failure
```

Expected: 5/5 pass and `main` builds.

- [ ] **Step 5: Run full desktop and iOS compile gates**

```bash
ctest --test-dir cmake-build-debug --output-on-failure -j 6
cmake --build cmake-build-debug --target main -j 6
scripts/ios_firebase_deploy.sh --build-only
git diff --check
```

Expected: full CTest passes, desktop `main` builds, iOS reports `BUILD SUCCEEDED`, and diff check is clean. Do not run the deploy script without `--build-only`.

- [ ] **Step 6: Audit the authority boundary**

Run:

```bash
rg -n "bms_parser|SDL|bgfx|Jukebox|AudioWrapper|mutex|lock_guard|unique_lock" \
  src/scene/play/GameplayScoreState.h \
  src/scene/play/GameplaySimulation.h \
  src/scene/play/GameplaySimulation.cpp
rg -n "GameplaySimulation|buildGameplayDefinition" src \
  --glob '!scene/play/GameplaySimulation.*' \
  --glob '!scene/play/GameplayDefinition.*'
git diff --name-only -- src/bms_parser.hpp src/bms_parser.cpp
```

Expected: no prohibited runtime dependency, no live production call site, and no generated parser diff.

- [ ] **Step 7: Commit terminal authority boundary**

```bash
git add CMakeLists.txt src/scene/play/GameplaySimulation.h src/scene/play/GameplaySimulation.cpp tests/gameplay_automatic_authority_tests.cpp
git commit -m "feat: complete synchronous gameplay authority"
```

## Completion Gate

This subproject is complete only when:

- `GameplaySimulation` owns note/lane state, judge totals, EX score, combo/max combo/combo breaks, fast/slow counts, all gauge values/survival latches, HCN balances, judgement-authoritative replay events, automatic cursors, and terminal reason.
- Direct input and every automatic deadline use one monotonic serialized timeline.
- Normal, Classic, Charge, HellCharge, landmine, autoplay, practice finalization, survival failure, and chart completion tests pass.
- Non-zero attempt start initialization resolves obsolete work once and preserves crossing long notes without retroactive score, gauge, or replay mutations.
- Replay events snapshot post-transaction gauge/gauge type/combo/score.
- Large-step and small-step scheduling produce identical state/replay/outcome.
- Runtime state contains no parser pointer and hot methods perform no allocation, general-purpose locking, renderer/UI/audio call, or chart-wide scan.
- Existing production gameplay behavior remains unchanged because live authority has not switched.
- Full desktop CTest, desktop `main`, and iOS build-only pass.

## Next Planning Boundary

After this plan passes, the next plan adds fixed ingress/control/audio/result queues, audio clock anchors, realtime sound handles, the serial gameplay worker, and immutable read snapshots. That plan temporarily routes SDL gameplay transitions through the worker and performs the single production authority cutover. The following iOS plan returns `SDL_main`, separates UIKit/render from engine/bgfx API work, installs native touch/keyboard/controller/MIDI/gyro sources, and suppresses matching SDL gameplay events.
