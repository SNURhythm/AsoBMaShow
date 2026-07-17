# Gameplay Input Transaction Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and verify a parser-independent, indexed gameplay press/release simulation that preserves current judgement, long-note, replay, and keysound-selection behavior without changing the production input authority yet.

**Architecture:** Chart setup compiles immutable note definitions, stable note IDs, fixed judgement windows, and sorted per-lane indices. `GameplaySimulation` owns separate runtime note/lane state and returns one transaction result containing note ownership, judgement, replay, visual-lane, and sound intent. This plan leaves `GamePlayScene` and `RhythmLaneInputController` authoritative so parser state cannot diverge before automatic deadlines are moved in the next subproject.

**Tech Stack:** C++23, CMake/CTest, existing `bms_parser` chart model as read-only build input, existing `Judge`, `AppSettings::NotePriorityMode`, and lightweight executable tests.

## Global Constraints

- The approved design is `docs/superpowers/specs/2026-07-17-asynchronous-gameplay-input-design.md`.
- The final software target is native callback entry through committed gameplay state and queued sound below 1 ms at p99; this foundation must therefore allocate only during construction and use no general-purpose lock in transaction methods.
- Keysound selection, note claiming, and judgement remain one serialized logical transaction; never add speculative audio behavior.
- Parser chart objects are read-only inputs to `buildGameplayDefinition()`; new runtime state must not mutate `bms_parser::Note`.
- Keep SDL and the existing `GamePlayScene`/`RhythmLaneInputController` path authoritative throughout this plan. Do not connect the new simulation to live gameplay yet.
- Do not edit `src/bms_parser.hpp` or `src/bms_parser.cpp`; they are generated from `../bms-parser-cpp`.
- Add no third-party dependency and keep all new shared code portable C++23.
- The iOS synchronized Xcode source group discovers supported files under `src`; do not add normal new source files to `membershipExceptions`.
- Every task follows test-first order and ends in its own commit.
- Use `cmake --build cmake-build-debug --target main -j 6` for the final desktop compile check.

## Scope Boundary

This plan implements the input-transaction slice of `GameplaySimulation` and its parity tests. It intentionally does not implement automatic miss/landmine/hell-charge deadlines, score/gauge ownership, replay accumulation, realtime queues, audio handles, render snapshots, iOS runtime splitting, or native iOS sources. Those consumers will use the exact interfaces produced here after their own reviewer gates.

## File Map

- Create `src/scene/play/CompiledGameplayJudge.h`: fixed judgement-window contract used by the transaction hot path.
- Create `src/scene/play/CompiledGameplayJudge.cpp`: one-time conversion from `Judge` plus allocation-free judgement lookup.
- Create `src/scene/play/Judgement.h`: parser-independent judgement enum/result value types shared by `Judge` and the simulation.
- Modify `src/scene/play/Judge.h`: consume `Judgement.h` while retaining parser-aware chart judgement APIs.
- Create `src/scene/play/GameplayDefinition.h`: immutable note IDs, note metadata, long-note relationships, and per-lane indices.
- Create `src/scene/play/GameplayDefinition.cpp`: deterministic read-only chart compiler.
- Create `src/scene/play/GameplaySimulation.h`: runtime state and press/release transaction API.
- Create `src/scene/play/GameplaySimulation.cpp`: indexed candidate selection and state transaction semantics.
- Create `tests/gameplay_simulation_tests.cpp`: definition, judgement, input, rapid-press, long-note, parity, and search-bound tests.
- Modify `src/scene/play/CMakeLists.txt`: compile the new sources into `main`.
- Modify `CMakeLists.txt`: build and register `gameplay_simulation_tests`.

---

### Task 1: Compile Fixed Judgement Windows

**Files:**

- Create: `src/scene/play/CompiledGameplayJudge.h`
- Create: `src/scene/play/CompiledGameplayJudge.cpp`
- Create: `src/scene/play/Judgement.h`
- Modify: `src/scene/play/Judge.h`
- Create: `tests/gameplay_simulation_tests.cpp`
- Modify: `src/scene/play/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Consumes: `Judge::timingWindows` after play-start rank/rate/course scaling is complete.
- Produces: `CompiledGameplayJudge::from(const Judge&)`, `judgeAt(noteTimeMicros, inputTimeMicros)`, `window(Judgement)`, `latestHittableNoteTiming(inputTimeMicros)`, and `latePoorTimingMicros()`.

- [ ] **Step 1: Register the focused test target and write the failing fixed-window test**

Add this target beside `gameplay_practice_input_boundary_tests` in the root `CMakeLists.txt`:

```cmake
    add_executable(gameplay_simulation_tests
        tests/gameplay_simulation_tests.cpp
        src/scene/play/CompiledGameplayJudge.cpp
        src/scene/play/Judge.cpp
    )
    target_include_directories(gameplay_simulation_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(gameplay_simulation_tests PRIVATE cxx_std_23)
```

Add this registration beside the other gameplay registrations after
`asobmashow_register_test` is defined:

```cmake
    asobmashow_register_test(gameplay_simulation_tests)
```

Create `tests/gameplay_simulation_tests.cpp` with this initial test harness:

```cpp
#include "scene/play/CompiledGameplayJudge.h"
#include "scene/play/Judge.h"

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void testCompiledJudgePreservesResolvedWindows() {
  Judge judge(1);
  judge.applyWindowScale(50, 200);
  const auto compiled = gameplay::CompiledGameplayJudge::from(judge);

  require(compiled.judgeAt(1'000'000, 1'010'000).judgement == PGreat,
          "compiled judge preserves the resolved PGreat window");
  require(compiled.judgeAt(1'000'000, 1'030'000).judgement == Great,
          "compiled judge preserves the resolved Great window");
  require(compiled.window(Bad)->lateMicros == 420'000,
          "compiled judge exposes the Bad late edge");
  require(compiled.latestHittableNoteTiming(1'000'000) == 1'500'000,
          "future cutoff uses the earliest hittable edge");
}
} // namespace

int main() {
  testCompiledJudgePreservesResolvedWindows();
  return 0;
}
```

- [ ] **Step 2: Run the test target and verify the missing contract fails**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_simulation_tests -j 6
```

Expected: compilation fails with `scene/play/CompiledGameplayJudge.h` not found.

- [ ] **Step 3: Extract parser-independent judgement values**

Create `src/scene/play/Judgement.h`:

```cpp
#pragma once

#include <string>

enum Judgement { PGreat, Great, Good, Bad, Kpoor, Poor, None, JudgementCount };

enum class CourseJudgementConstraint { None, NoGood, NoGreat };

class JudgeResult {
public:
  JudgeResult(Judgement judgement, long long diffMicros)
      : judgement(judgement), Diff(diffMicros) {}

  Judgement judgement = None;
  long long Diff = 0;

  [[nodiscard]] bool isComboBreak() const {
    return judgement == Bad || judgement == Poor;
  }

  [[nodiscard]] bool isNotePlayed() const {
    return judgement != Kpoor && judgement != None;
  }

  [[nodiscard]] std::string toString() const {
    switch (judgement) {
    case PGreat:
      return "PGREAT";
    case Great:
      return "GREAT";
    case Good:
      return "GOOD";
    case Bad:
      return "BAD";
    case Kpoor:
      return "KPOOR";
    case Poor:
      return "POOR";
    case None:
    case JudgementCount:
      return "NONE";
    }
    return "NONE";
  }
};
```

In `src/scene/play/Judge.h`, include `Judgement.h` and remove the existing
`Judgement`, `CourseJudgementConstraint`, and `JudgeResult` definitions. Leave
the parser-aware `Judge` class declaration unchanged.

- [ ] **Step 4: Add the fixed-window public contract**

Create `src/scene/play/CompiledGameplayJudge.h`:

```cpp
#pragma once

#include "Judgement.h"

#include <array>
#include <cstdint>
#include <optional>

class Judge;

namespace gameplay {

struct TimingWindow {
  Judgement judgement = None;
  std::int64_t earlyMicros = 0;
  std::int64_t lateMicros = 0;
};

class CompiledGameplayJudge {
public:
  static CompiledGameplayJudge from(const Judge &judge);

  [[nodiscard]] JudgeResult judgeAt(std::int64_t noteTimeMicros,
                                    std::int64_t inputTimeMicros) const noexcept;
  [[nodiscard]] std::optional<TimingWindow>
  window(Judgement judgement) const noexcept;
  [[nodiscard]] std::int64_t
  latestHittableNoteTiming(std::int64_t inputTimeMicros) const noexcept;
  [[nodiscard]] std::int64_t latePoorTimingMicros() const noexcept;

private:
  static constexpr std::array<Judgement, 5> kHittableJudgements = {
      PGreat, Great, Good, Bad, Kpoor};
  std::array<TimingWindow, kHittableJudgements.size()> windows_{};
};

} // namespace gameplay
```

- [ ] **Step 5: Implement allocation-free judgement lookup**

Create `src/scene/play/CompiledGameplayJudge.cpp`:

```cpp
#include "CompiledGameplayJudge.h"
#include "Judge.h"

#include <algorithm>
#include <limits>

namespace gameplay {

CompiledGameplayJudge CompiledGameplayJudge::from(const Judge &judge) {
  CompiledGameplayJudge compiled;
  for (std::size_t index = 0; index < kHittableJudgements.size(); ++index) {
    const Judgement judgement = kHittableJudgements[index];
    const auto source = judge.timingWindows.find(judgement);
    if (source == judge.timingWindows.end()) {
      compiled.windows_[index] = {judgement, 1, 0};
      continue;
    }
    compiled.windows_[index] = {
        judgement, source->second.first, source->second.second};
  }
  return compiled;
}

JudgeResult CompiledGameplayJudge::judgeAt(
    std::int64_t noteTimeMicros, std::int64_t inputTimeMicros) const noexcept {
  const std::int64_t diff = inputTimeMicros - noteTimeMicros;
  for (const auto &window : windows_) {
    if (window.earlyMicros <= diff && diff <= window.lateMicros) {
      return JudgeResult(window.judgement, diff);
    }
  }
  return JudgeResult(None, diff);
}

std::optional<TimingWindow>
CompiledGameplayJudge::window(Judgement judgement) const noexcept {
  const auto found = std::ranges::find_if(
      windows_, [judgement](const TimingWindow &candidate) {
        return candidate.judgement == judgement;
      });
  return found == windows_.end() ? std::nullopt
                                 : std::optional<TimingWindow>(*found);
}

std::int64_t CompiledGameplayJudge::latestHittableNoteTiming(
    std::int64_t inputTimeMicros) const noexcept {
  std::int64_t earliest = std::numeric_limits<std::int64_t>::max();
  for (const auto &window : windows_) {
    if (window.earlyMicros <= window.lateMicros) {
      earliest = std::min(earliest, window.earlyMicros);
    }
  }
  return earliest == std::numeric_limits<std::int64_t>::max()
             ? inputTimeMicros
             : inputTimeMicros - earliest;
}

std::int64_t CompiledGameplayJudge::latePoorTimingMicros() const noexcept {
  const auto bad = window(Bad);
  return bad.has_value() ? bad->lateMicros : 0;
}

} // namespace gameplay
```

Add `CompiledGameplayJudge.cpp` to `src/scene/play/CMakeLists.txt`.

- [ ] **Step 6: Run the focused test**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_simulation_tests gameplay_practice_input_boundary_tests -j 6
ctest --test-dir cmake-build-debug -R '^(gameplay_simulation_tests|gameplay_practice_input_boundary_tests)$' --output-on-failure
```

Expected: both targets build and pass, proving the judgement-value extraction
did not change the current controller tests.

- [ ] **Step 7: Commit the fixed judgement contract**

```bash
git add CMakeLists.txt src/scene/play/CMakeLists.txt src/scene/play/Judgement.h src/scene/play/Judge.h src/scene/play/CompiledGameplayJudge.h src/scene/play/CompiledGameplayJudge.cpp tests/gameplay_simulation_tests.cpp
git commit -m "feat: compile gameplay judgement windows"
```

---

### Task 2: Compile Immutable Gameplay Definitions

**Files:**

- Create: `src/scene/play/GameplayDefinition.h`
- Create: `src/scene/play/GameplayDefinition.cpp`
- Modify: `src/scene/play/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `tests/gameplay_simulation_tests.cpp`

**Interfaces:**

- Consumes: `const bms_parser::Chart&` and the effective long-note-mode override during session setup.
- Produces: `buildGameplayDefinition()`, stable `NoteId`, immutable `NoteDefinition`, and `GameplayDefinition::laneNotes()` sorted by timing.

- [ ] **Step 1: Write failing definition/index tests**

Add chart helpers and this test to `tests/gameplay_simulation_tests.cpp`:

```cpp
#include "scene/play/GameplayDefinition.h"

bms_parser::TimeLine *addTimeline(bms_parser::Measure &measure,
                                  long long timingMicros) {
  auto *timeline = new bms_parser::TimeLine(8, false);
  timeline->Timing = timingMicros;
  measure.TimeLines.push_back(timeline);
  return timeline;
}

bms_parser::LongNote *addLongNote(bms_parser::Measure &measure,
                                  long long headMicros,
                                  long long tailMicros, int lane,
                                  bms_parser::LongNoteType type) {
  auto *headTimeline = addTimeline(measure, headMicros);
  auto *tailTimeline = addTimeline(measure, tailMicros);
  auto *head = new bms_parser::LongNote(7, type);
  auto *tail = new bms_parser::LongNote(7, type);
  head->Tail = tail;
  tail->Head = head;
  headTimeline->SetNote(lane, head);
  tailTimeline->SetNote(lane, tail);
  return head;
}

void testDefinitionUsesStableIdsAndLaneIndices() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *normalTimeline = addTimeline(*measure, 500'000);
  normalTimeline->SetNote(2, new bms_parser::Note(3));
  auto *head = addLongNote(*measure, 700'000, 900'000, 1,
                           bms_parser::LongNoteType::ChargeNote);
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  require(definition.noteCount() == 3,
          "normal and both long-note identities receive stable IDs");
  const auto laneOne = definition.laneNotes(1);
  require(laneOne.size() == 2,
          "lane index contains the long-note head and tail");
  const auto &headDefinition = definition.note(laneOne[0]);
  const auto &tailDefinition = definition.note(laneOne[1]);
  require(headDefinition.kind == gameplay::NoteKind::LongHead &&
              tailDefinition.kind == gameplay::NoteKind::LongTail,
          "long-note identities retain head and tail roles");
  require(headDefinition.pairId == tailDefinition.id &&
              tailDefinition.pairId == headDefinition.id,
          "long-note identities point to each other by stable ID");
  require(headDefinition.longNoteRule == gameplay::LongNoteRule::Charge,
          "effective long-note behavior is compiled once");
  require(definition.note(laneOne[0]).timingMicros ==
              head->Timeline->Timing,
          "definition copies timing without retaining mutable note state");
  require(definition.laneNotes(99).empty(),
          "unknown lanes return an empty span without allocation");
}
```

Call `testDefinitionUsesStableIdsAndLaneIndices()` from `main()`.

- [ ] **Step 2: Run the test and verify the missing definition fails**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_simulation_tests -j 6
```

Expected: compilation fails with `scene/play/GameplayDefinition.h` not found.

- [ ] **Step 3: Add immutable definition types**

Create `src/scene/play/GameplayDefinition.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace bms_parser {
class Chart;
}

namespace gameplay {

using NoteId = std::uint32_t;
inline constexpr NoteId kInvalidNoteId =
    std::numeric_limits<NoteId>::max();

enum class NoteKind { Normal, LongHead, LongTail, Landmine };
enum class LongNoteRule { None, Classic, Charge, HellCharge };

struct NoteDefinition {
  NoteId id = kInvalidNoteId;
  int lane = -1;
  std::int64_t timingMicros = 0;
  int wav = 0;
  NoteKind kind = NoteKind::Normal;
  LongNoteRule longNoteRule = LongNoteRule::None;
  NoteId pairId = kInvalidNoteId;
  bool scratchLane = false;
  float mineDamage = 0.0F;
};

struct LaneDefinition {
  int lane = -1;
  std::vector<NoteId> noteIds;
};

class GameplayDefinition {
public:
  [[nodiscard]] std::size_t noteCount() const noexcept;
  [[nodiscard]] const NoteDefinition &note(NoteId id) const;
  [[nodiscard]] std::span<const NoteId> laneNotes(int lane) const noexcept;
  [[nodiscard]] std::span<const LaneDefinition> lanes() const noexcept;

private:
  friend GameplayDefinition buildGameplayDefinition(
      const bms_parser::Chart &, int);
  std::vector<NoteDefinition> notes_;
  std::vector<LaneDefinition> lanes_;
};

GameplayDefinition buildGameplayDefinition(const bms_parser::Chart &chart,
                                           int longNoteModeOverride);

} // namespace gameplay
```

- [ ] **Step 4: Implement deterministic chart compilation**

Create `src/scene/play/GameplayDefinition.cpp`. The implementation must:

```cpp
#include "GameplayDefinition.h"

#include "../../CoursePlaySession.h"
#include "../../bms_parser.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

namespace gameplay {
namespace {

LongNoteRule resolveLongNoteRule(const bms_parser::LongNote *note,
                                 const bms_parser::Chart &chart,
                                 int overrideMode) {
  if (effectiveLongNoteIsHellCharge(note, chart, overrideMode)) {
    return LongNoteRule::HellCharge;
  }
  if (effectiveLongNoteIsCharge(note, chart, overrideMode)) {
    return LongNoteRule::Charge;
  }
  return LongNoteRule::Classic;
}

NoteKind noteKind(const bms_parser::Note *note) {
  if (note->IsLandmineNote()) {
    return NoteKind::Landmine;
  }
  if (!note->IsLongNote()) {
    return NoteKind::Normal;
  }
  return static_cast<const bms_parser::LongNote *>(note)->IsTail()
             ? NoteKind::LongTail
             : NoteKind::LongHead;
}

} // namespace

std::size_t GameplayDefinition::noteCount() const noexcept {
  return notes_.size();
}

const NoteDefinition &GameplayDefinition::note(NoteId id) const {
  if (id >= notes_.size()) {
    throw std::out_of_range("gameplay note id");
  }
  return notes_[id];
}

std::span<const NoteId>
GameplayDefinition::laneNotes(int lane) const noexcept {
  const auto found = std::ranges::lower_bound(
      lanes_, lane, {}, &LaneDefinition::lane);
  return found != lanes_.end() && found->lane == lane
             ? std::span<const NoteId>(found->noteIds)
             : std::span<const NoteId>();
}

std::span<const LaneDefinition> GameplayDefinition::lanes() const noexcept {
  return lanes_;
}

GameplayDefinition buildGameplayDefinition(const bms_parser::Chart &chart,
                                           int longNoteModeOverride) {
  GameplayDefinition result;
  std::unordered_map<const bms_parser::Note *, NoteId> ids;

  const auto append = [&](const bms_parser::Note *note) {
    if (note == nullptr || ids.contains(note)) {
      return;
    }
    const NoteId id = static_cast<NoteId>(result.notes_.size());
    ids.emplace(note, id);
    const auto kind = noteKind(note);
    const auto *longNote = note->IsLongNote()
                               ? static_cast<const bms_parser::LongNote *>(note)
                               : nullptr;
    result.notes_.push_back({
        .id = id,
        .lane = note->Lane,
        .timingMicros = note->Timeline->Timing,
        .wav = note->Wav,
        .kind = kind,
        .longNoteRule = longNote == nullptr
                            ? LongNoteRule::None
                            : resolveLongNoteRule(longNote, chart,
                                                  longNoteModeOverride),
        .pairId = kInvalidNoteId,
        .scratchLane = chartLaneIsScratch(chart.Meta, note->Lane),
        .mineDamage = note->IsLandmineNote()
                          ? static_cast<const bms_parser::LandmineNote *>(note)
                                ->Damage
                          : 0.0F,
    });
  };

  for (const auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      for (const auto *note : timeline->Notes) {
        append(note);
      }
      for (const auto *note : timeline->LandmineNotes) {
        append(note);
      }
    }
  }

  for (const auto &[source, id] : ids) {
    if (!source->IsLongNote()) {
      continue;
    }
    const auto *longNote = static_cast<const bms_parser::LongNote *>(source);
    const auto *pair = longNote->IsTail() ? longNote->Head : longNote->Tail;
    const auto found = ids.find(pair);
    if (found != ids.end()) {
      result.notes_[id].pairId = found->second;
    }
  }

  for (const auto &note : result.notes_) {
    if (note.kind == NoteKind::Landmine) {
      continue;
    }
    auto lane = std::ranges::lower_bound(
        result.lanes_, note.lane, {}, &LaneDefinition::lane);
    if (lane == result.lanes_.end() || lane->lane != note.lane) {
      lane = result.lanes_.insert(lane, LaneDefinition{.lane = note.lane});
    }
    lane->noteIds.push_back(note.id);
  }
  for (auto &lane : result.lanes_) {
    std::ranges::sort(lane.noteIds, [&](NoteId left, NoteId right) {
      const auto &leftNote = result.notes_[left];
      const auto &rightNote = result.notes_[right];
      return leftNote.timingMicros == rightNote.timingMicros
                 ? left < right
                 : leftNote.timingMicros < rightNote.timingMicros;
    });
  }
  return result;
}

} // namespace gameplay
```

Add `GameplayDefinition.cpp` to `src/scene/play/CMakeLists.txt`, and add
`src/scene/play/GameplayDefinition.cpp` plus `src/bms_parser.cpp` to the focused
test target.

- [ ] **Step 5: Run definition and existing parser-sensitive tests**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_simulation_tests gameplay_practice_input_boundary_tests -j 6
ctest --test-dir cmake-build-debug -R '^(gameplay_simulation_tests|gameplay_practice_input_boundary_tests)$' --output-on-failure
```

Expected: both tests pass.

- [ ] **Step 6: Commit immutable definition compilation**

```bash
git add CMakeLists.txt src/scene/play/CMakeLists.txt src/scene/play/GameplayDefinition.h src/scene/play/GameplayDefinition.cpp tests/gameplay_simulation_tests.cpp
git commit -m "feat: compile immutable gameplay definitions"
```

---

### Task 3: Add Indexed Runtime Candidate Selection

**Files:**

- Create: `src/scene/play/GameplaySimulation.h`
- Create: `src/scene/play/GameplaySimulation.cpp`
- Modify: `src/scene/play/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `tests/gameplay_simulation_tests.cpp`

**Interfaces:**

- Consumes: immutable `GameplayDefinition`, `CompiledGameplayJudge`, practice range, note-priority mode, and timestamped `GameplayInputContext`.
- Produces: owned `NoteRuntimeState`, owned lane pressed/cursor state, `pressLane()`, `releaseLane()`, and `lastSearchStats()`.

- [ ] **Step 1: Write failing indexed-selection tests**

Add these tests:

```cpp
#include "scene/play/GameplaySimulation.h"

void testCandidateSelectionIsLaneIndexed() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  for (int index = 0; index < 1'000; ++index) {
    auto *timeline = addTimeline(*measure, index * 10'000LL);
    timeline->SetNote(2, new bms_parser::Note(1));
  }
  auto *targetTimeline = addTimeline(*measure, 5'000'000);
  targetTimeline->SetNote(1, new bms_parser::Note(9));
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1)),
       .notePriorityMode = AppSettings::NotePriorityMode::Lowest});

  const auto result = simulation.pressLane(
      1, 1, {.songTimeMicros = 5'000'000,
             .laneBeamTimeMicros = 7'000'000});
  require(result.noteId != gameplay::kInvalidNoteId,
          "target lane resolves its note");
  require(simulation.lastSearchStats().notesExamined <= 2,
          "unrelated lanes are not scanned");
}

void testCompensationAndPriorityMatchCurrentRules() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *early = addTimeline(*measure, 970'000);
  early->SetNote(1, new bms_parser::Note(1));
  auto *exact = addTimeline(*measure, 1'000'000);
  exact->SetNote(2, new bms_parser::Note(2));
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1)),
       .notePriorityMode = AppSettings::NotePriorityMode::Duration});
  const auto result = simulation.pressLane(
      1, 2, {.songTimeMicros = 1'000'000,
             .laneBeamTimeMicros = 2'000'000});
  require(definition.note(result.noteId).lane == 2,
          "duration priority selects the closer compensation-lane note");
}

void testPracticeRangeIsHalfOpenBeforeLaneMutation() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *inside = addTimeline(*measure, 999'999);
  inside->SetNote(1, new bms_parser::Note(1));
  auto *atEnd = addTimeline(*measure, 1'000'000);
  atEnd->SetNote(2, new bms_parser::Note(2));
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1)),
       .allowedNoteRange = gameplay::GameplayTimeRange{
           .startMicros = 500'000, .endMicros = 1'000'000}});
  const auto accepted = simulation.pressLane(
      1, {.songTimeMicros = 999'999, .laneBeamTimeMicros = 2'000'000});
  const auto rejected = simulation.pressLane(
      2, {.songTimeMicros = 1'000'000, .laneBeamTimeMicros = 2'000'001});
  require(accepted.noteId != gameplay::kInvalidNoteId,
          "the final microsecond inside practice remains hittable");
  require(rejected.noteId == gameplay::kInvalidNoteId &&
              !simulation.lanePressed(2),
          "the exclusive end blocks selection before lane mutation");
}

void testEqualTimeLowestKeepsMainLanePrecedence() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *timeline = addTimeline(*measure, 1'000'000);
  timeline->SetNote(1, new bms_parser::Note(1));
  timeline->SetNote(2, new bms_parser::Note(2));
  chart.Measures.push_back(measure);
  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1)),
       .notePriorityMode = AppSettings::NotePriorityMode::Lowest});
  const auto result = simulation.pressLane(
      2, 1, {.songTimeMicros = 1'000'000,
             .laneBeamTimeMicros = 2'000'000});
  require(definition.note(result.noteId).lane == 2,
          "Lowest priority keeps equal-time compensation behind the main lane");
}
```

Execution correction: this test freezes equal-time main-lane precedence only
for `Lowest`. The authoritative controller still applies `shouldPreferCandidate`
to later equal-time candidates for every other priority mode; at a sufficiently
late input, `Combo` and `Score` therefore replace the main-lane candidate with
the compensation-lane candidate. The chronological merge and `shouldPrefer`
snippet below must preserve that priority-dependent behavior under Task 6's
current-controller parity goal; do not add an unconditional equal-time return.

Call all four tests from `main()`.

- [ ] **Step 2: Run the test and verify the simulation contract is missing**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_simulation_tests -j 6
```

Expected: compilation fails with `scene/play/GameplaySimulation.h` not found.

- [ ] **Step 3: Define owned runtime and transaction result types**

Create `src/scene/play/GameplaySimulation.h`:

```cpp
#pragma once

#include "CompiledGameplayJudge.h"
#include "GameplayDefinition.h"
#include "../../AppSettings.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace gameplay {

struct NoteRuntimeState {
  bool played = false;
  bool dead = false;
  bool holding = false;
  std::int64_t playedTimeMicros = 0;
  std::int64_t releaseTimeMicros = 0;
};

struct GameplayTimeRange {
  std::int64_t startMicros = 0;
  std::int64_t endMicros = 0;

  [[nodiscard]] bool contains(std::int64_t timingMicros) const noexcept {
    return startMicros <= timingMicros && timingMicros < endMicros;
  }
};

struct GameplaySimulationConfig {
  CompiledGameplayJudge judge;
  AppSettings::NotePriorityMode notePriorityMode =
      AppSettings::NotePriorityMode::Lowest;
  std::optional<GameplayTimeRange> allowedNoteRange;
};

struct GameplayInputContext {
  std::int64_t songTimeMicros = 0;
  std::int64_t laneBeamTimeMicros = 0;
  std::int64_t inputDelayMicros = 0;
};

enum class GameplayReplayAction { Press, Release, Miss, Mine, Gauge };

struct GameplayReplayEvent {
  GameplayReplayAction action = GameplayReplayAction::Press;
  NoteId noteId = kInvalidNoteId;
  int lane = -1;
  std::int64_t noteTimeMicros = -1;
  std::int64_t songTimeMicros = 0;
  std::int64_t judgeTimeMicros = 0;
  Judgement judgement = None;
  std::int64_t diffMicros = 0;
};

enum class LaneVisualAction { Press, Release };

struct LaneVisualEvent {
  LaneVisualAction action = LaneVisualAction::Press;
  int lane = -1;
  std::int64_t visualTimeMicros = 0;
  JudgeResult judge = JudgeResult(None, 0);
};

struct GameplayInputResult {
  NoteId noteId = kInvalidNoteId;
  NoteId soundNoteId = kInvalidNoteId;
  bool hasJudge = false;
  JudgeResult judge = JudgeResult(None, 0);
  bool hasReplayEvent = false;
  GameplayReplayEvent replayEvent;
  bool hasLaneVisual = false;
  LaneVisualEvent laneVisual;
};

struct GameplaySearchStats {
  std::size_t notesExamined = 0;
};

class GameplaySimulation {
public:
  GameplaySimulation(const GameplayDefinition &definition,
                     GameplaySimulationConfig config);

  GameplayInputResult pressLane(int lane,
                                const GameplayInputContext &context);
  GameplayInputResult pressLane(int mainLane, int compensateLane,
                                const GameplayInputContext &context);
  GameplayInputResult releaseLane(int lane,
                                  const GameplayInputContext &context,
                                  bool isBackSpin = false);

  [[nodiscard]] const NoteRuntimeState &noteState(NoteId id) const;
  [[nodiscard]] bool lanePressed(int lane) const noexcept;
  [[nodiscard]] GameplaySearchStats lastSearchStats() const noexcept;

private:
  struct LaneRuntimeState {
    int lane = -1;
    bool pressed = false;
    std::size_t cursor = 0;
  };

  [[nodiscard]] LaneRuntimeState *findLane(int lane) noexcept;
  [[nodiscard]] const LaneRuntimeState *findLane(int lane) const noexcept;
  [[nodiscard]] std::int64_t
  inputTime(const GameplayInputContext &context) const noexcept;
  [[nodiscard]] NoteId selectPressCandidate(int mainLane, int compensateLane,
                                            std::int64_t inputTimeMicros);
  [[nodiscard]] NoteId selectReleaseCandidate(int lane,
                                              std::int64_t inputTimeMicros);

  const GameplayDefinition &definition_;
  GameplaySimulationConfig config_;
  std::vector<NoteRuntimeState> noteStates_;
  std::vector<LaneRuntimeState> laneStates_;
  GameplaySearchStats lastSearchStats_;
};

} // namespace gameplay
```

- [ ] **Step 4: Implement owned construction and indexed lane lookup**

Create `src/scene/play/GameplaySimulation.cpp` with construction and lookup:

```cpp
GameplaySimulation::GameplaySimulation(const GameplayDefinition &definition,
                                       GameplaySimulationConfig config)
    : definition_(definition), config_(std::move(config)),
      noteStates_(definition.noteCount()) {
  laneStates_.reserve(definition.lanes().size());
  for (const auto &lane : definition.lanes()) {
    laneStates_.push_back({.lane = lane.lane});
  }
}

std::int64_t GameplaySimulation::inputTime(
    const GameplayInputContext &context) const noexcept {
  return context.songTimeMicros - context.inputDelayMicros;
}

GameplaySimulation::LaneRuntimeState *
GameplaySimulation::findLane(int lane) noexcept {
  const auto found = std::ranges::lower_bound(
      laneStates_, lane, {}, &LaneRuntimeState::lane);
  return found != laneStates_.end() && found->lane == lane ? &*found : nullptr;
}

const GameplaySimulation::LaneRuntimeState *
GameplaySimulation::findLane(int lane) const noexcept {
  const auto found = std::ranges::lower_bound(
      laneStates_, lane, {}, &LaneRuntimeState::lane);
  return found != laneStates_.end() && found->lane == lane ? &*found : nullptr;
}

bool GameplaySimulation::lanePressed(int lane) const noexcept {
  const auto *state = findLane(lane);
  return state != nullptr && state->pressed;
}

GameplaySearchStats GameplaySimulation::lastSearchStats() const noexcept {
  return lastSearchStats_;
}

const NoteRuntimeState &GameplaySimulation::noteState(NoteId id) const {
  return noteStates_.at(id);
}
```

- [ ] **Step 5: Implement the chronological two-lane press merge**

Add `<algorithm>`, `<array>`, `<cstdlib>`, and `<utility>` to
`GameplaySimulation.cpp`, then implement the press candidate merge:

```cpp
NoteId GameplaySimulation::selectPressCandidate(
    int mainLane, int compensateLane, std::int64_t inputTimeMicros) {
  lastSearchStats_ = {};
  struct LaneScan {
    std::span<const NoteId> ids;
    std::size_t index = 0;
  };
  std::array<LaneScan, 2> scans{};
  std::size_t scanCount = 0;
  const std::int64_t poorCutoff =
      inputTimeMicros - config_.judge.latePoorTimingMicros();
  const std::int64_t futureCutoff =
      config_.judge.latestHittableNoteTiming(inputTimeMicros);

  const auto addLane = [&](int lane) {
    auto *runtime = findLane(lane);
    if (runtime == nullptr || runtime->pressed) {
      return;
    }
    const auto ids = definition_.laneNotes(lane);
    while (runtime->cursor < ids.size()) {
      const NoteId id = ids[runtime->cursor];
      const auto &note = definition_.note(id);
      const auto &state = noteStates_[id];
      ++lastSearchStats_.notesExamined;
      if (!state.played && !state.dead && note.timingMicros >= poorCutoff) {
        break;
      }
      ++runtime->cursor;
    }
    scans[scanCount++] = {ids, runtime->cursor};
  };

  addLane(mainLane);
  if (compensateLane != mainLane) {
    addLane(compensateLane);
  }

  const auto noteAllowed = [&](const NoteDefinition &note) {
    return !config_.allowedNoteRange.has_value() ||
           config_.allowedNoteRange->contains(note.timingMicros);
  };
  const auto shouldPrefer = [&](NoteId current, NoteId next) {
    const auto &currentNote = definition_.note(current);
    const auto &nextNote = definition_.note(next);
    switch (config_.notePriorityMode) {
    case AppSettings::NotePriorityMode::Duration:
      return std::llabs(currentNote.timingMicros - inputTimeMicros) >
             std::llabs(nextNote.timingMicros - inputTimeMicros);
    case AppSettings::NotePriorityMode::Combo: {
      const auto window = config_.judge.window(Good);
      return window.has_value() &&
             currentNote.timingMicros <
                 inputTimeMicros - window->lateMicros &&
             nextNote.timingMicros <=
                 inputTimeMicros - window->earlyMicros;
    }
    case AppSettings::NotePriorityMode::Score: {
      const auto window = config_.judge.window(Great);
      return window.has_value() &&
             currentNote.timingMicros <
                 inputTimeMicros - window->lateMicros &&
             nextNote.timingMicros <=
                 inputTimeMicros - window->earlyMicros;
    }
    case AppSettings::NotePriorityMode::Lowest:
      return false;
    }
    return false;
  };

  NoteId selected = kInvalidNoteId;
  while (true) {
    std::size_t chosen = scanCount;
    for (std::size_t index = 0; index < scanCount; ++index) {
      const auto &scan = scans[index];
      if (scan.index >= scan.ids.size()) {
        continue;
      }
      if (chosen == scanCount) {
        chosen = index;
        continue;
      }
      const NoteId candidateId = scan.ids[scan.index];
      const NoteId chosenId = scans[chosen].ids[scans[chosen].index];
      const auto &candidate = definition_.note(candidateId);
      const auto &current = definition_.note(chosenId);
      if (candidate.timingMicros < current.timingMicros) {
        chosen = index;
      }
    }
    if (chosen == scanCount) {
      break;
    }

    auto &scan = scans[chosen];
    const NoteId id = scan.ids[scan.index++];
    const auto &note = definition_.note(id);
    ++lastSearchStats_.notesExamined;
    if (note.timingMicros > futureCutoff) {
      scan.index = scan.ids.size();
      continue;
    }
    const auto &state = noteStates_[id];
    if (state.played || state.dead || note.kind == NoteKind::Landmine ||
        !noteAllowed(note)) {
      continue;
    }
    const JudgeResult judge =
        config_.judge.judgeAt(note.timingMicros, inputTimeMicros);
    if (judge.judgement == None) {
      continue;
    }
    if (selected == kInvalidNoteId) {
      selected = id;
      if (config_.notePriorityMode ==
          AppSettings::NotePriorityMode::Lowest) {
        return selected;
      }
    } else if (shouldPrefer(selected, id)) {
      selected = id;
    }
  }
  return selected;
}
```

- [ ] **Step 6: Implement indexed release lookup**

```cpp
NoteId GameplaySimulation::selectReleaseCandidate(
    int lane, std::int64_t inputTimeMicros) {
  lastSearchStats_ = {};
  auto *runtime = findLane(lane);
  if (runtime == nullptr) {
    return kInvalidNoteId;
  }
  const auto ids = definition_.laneNotes(lane);
  const std::int64_t poorCutoff =
      inputTimeMicros - config_.judge.latePoorTimingMicros();
  for (std::size_t index = runtime->cursor; index < ids.size(); ++index) {
    const NoteId id = ids[index];
    const auto &note = definition_.note(id);
    const auto &state = noteStates_[id];
    ++lastSearchStats_.notesExamined;
    if (state.played || state.dead || note.timingMicros < poorCutoff) {
      runtime->cursor = index + 1;
      continue;
    }
    if (config_.allowedNoteRange.has_value() &&
        !config_.allowedNoteRange->contains(note.timingMicros)) {
      continue;
    }
    return id;
  }
  return kInvalidNoteId;
}
```

- [ ] **Step 7: Add the non-authoritative entrypoint bodies**

Add these complete, intentionally non-authoritative Task 3 bodies; Tasks 4 and 5
replace them under new failing tests:

```cpp
GameplayInputResult GameplaySimulation::pressLane(
    int lane, const GameplayInputContext &context) {
  return pressLane(lane, lane, context);
}

GameplayInputResult GameplaySimulation::pressLane(
    int mainLane, int compensateLane, const GameplayInputContext &context) {
  GameplayInputResult result;
  if (config_.allowedNoteRange.has_value() &&
      context.songTimeMicros >= config_.allowedNoteRange->endMicros) {
    return result;
  }
  result.noteId =
      selectPressCandidate(mainLane, compensateLane, inputTime(context));
  return result;
}

GameplayInputResult GameplaySimulation::releaseLane(
    int lane, const GameplayInputContext &context, bool) {
  GameplayInputResult result;
  if (config_.allowedNoteRange.has_value() &&
      context.songTimeMicros >= config_.allowedNoteRange->endMicros) {
    return result;
  }
  result.noteId = selectReleaseCandidate(lane, inputTime(context));
  return result;
}
```

Add `GameplaySimulation.cpp` to `src/scene/play/CMakeLists.txt` and to the
focused test target.

- [ ] **Step 8: Run the indexed-selection tests**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_simulation_tests -j 6
ctest --test-dir cmake-build-debug -R '^gameplay_simulation_tests$' --output-on-failure
```

Expected: all focused tests pass and the unrelated 1,000-note lane is not
examined.

- [ ] **Step 9: Commit indexed candidate selection**

```bash
git add CMakeLists.txt src/scene/play/CMakeLists.txt src/scene/play/GameplaySimulation.h src/scene/play/GameplaySimulation.cpp tests/gameplay_simulation_tests.cpp
git commit -m "feat: index gameplay input candidates by lane"
```

---

### Task 4: Make Press Handling One State-and-Sound Transaction

**Files:**

- Modify: `src/scene/play/GameplaySimulation.cpp`
- Modify: `tests/gameplay_simulation_tests.cpp`

**Interfaces:**

- Consumes: the candidate selected in Task 3.
- Produces: committed note/lane state plus matching `soundNoteId`, judge, replay, and lane-visual fields in one returned `GameplayInputResult`.

- [ ] **Step 1: Write failing press transaction and rapid-press tests**

Add:

```cpp
void testPressCommitsStateAndSoundTogether() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *timeline = addTimeline(*measure, 1'000'000);
  timeline->SetNote(1, new bms_parser::Note(42));
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1))});
  const auto first = simulation.pressLane(
      1, {.songTimeMicros = 1'000'000,
          .laneBeamTimeMicros = 9'000'000});

  require(first.noteId != gameplay::kInvalidNoteId &&
              first.soundNoteId == first.noteId,
          "accepted press returns matching note and sound identity");
  require(simulation.noteState(first.noteId).played,
          "accepted press commits note state before returning");
  require(first.hasJudge && first.judge.judgement == PGreat,
          "normal note commits its judgement");
  require(first.hasReplayEvent &&
              first.replayEvent.action == gameplay::GameplayReplayAction::Press,
          "accepted press commits replay intent");
  require(first.hasLaneVisual && simulation.lanePressed(1),
          "accepted press commits lane state and visual intent");

  const auto duplicate = simulation.pressLane(
      1, {.songTimeMicros = 1'000'001,
          .laneBeamTimeMicros = 9'000'001});
  require(duplicate.noteId == gameplay::kInvalidNoteId &&
              duplicate.soundNoteId == gameplay::kInvalidNoteId,
          "held-lane duplicate produces neither note nor sound");
}

void testClassicLongHeadDefersJudgeButStillCommitsSoundAndHolding() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  addLongNote(*measure, 1'000'000, 1'500'000, 1,
              bms_parser::LongNoteType::LongNote);
  chart.Measures.push_back(measure);
  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1))});

  const auto press = simulation.pressLane(
      1, {.songTimeMicros = 1'000'000,
          .laneBeamTimeMicros = 3'000'000});
  const auto &head = definition.note(press.noteId);
  require(press.soundNoteId == press.noteId && !press.hasJudge,
          "classic head sounds now and defers scoring to release");
  require(simulation.noteState(head.id).holding &&
              simulation.noteState(head.pairId).holding,
          "classic head atomically marks both identities holding");
}
```

Call both tests from `main()`.

- [ ] **Step 2: Run the test and verify transaction fields fail**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_simulation_tests -j 6
ctest --test-dir cmake-build-debug -R '^gameplay_simulation_tests$' --output-on-failure
```

Expected: the new assertions fail because Task 3 did not commit state or sound.

- [ ] **Step 3: Implement the complete press transaction**

Replace the temporary `pressLane()` bodies with logic that:

```cpp
GameplayInputResult GameplaySimulation::pressLane(
    int lane, const GameplayInputContext &context) {
  return pressLane(lane, lane, context);
}

GameplayInputResult GameplaySimulation::pressLane(
    int mainLane, int compensateLane, const GameplayInputContext &context) {
  GameplayInputResult result;
  if (config_.allowedNoteRange.has_value() &&
      context.songTimeMicros >= config_.allowedNoteRange->endMicros) {
    return result;
  }
  auto *mainState = findLane(mainLane);
  auto *compensateState = findLane(compensateLane);
  if ((mainState == nullptr || mainState->pressed) &&
      (compensateLane == mainLane || compensateState == nullptr ||
       compensateState->pressed)) {
    return result;
  }

  const std::int64_t judgedTime = inputTime(context);
  const NoteId selected =
      selectPressCandidate(mainLane, compensateLane, judgedTime);
  if (selected == kInvalidNoteId) {
    if (mainState != nullptr) {
      mainState->pressed = true;
    }
    result.hasReplayEvent = true;
    result.replayEvent = {
        .action = GameplayReplayAction::Press,
        .lane = mainLane,
        .songTimeMicros = judgedTime,
        .judgeTimeMicros = judgedTime,
    };
    result.hasLaneVisual = true;
    result.laneVisual = {LaneVisualAction::Press, mainLane,
                         context.laneBeamTimeMicros, JudgeResult(None, 0)};
    return result;
  }

  const auto &note = definition_.note(selected);
  auto &state = noteStates_[selected];
  const JudgeResult judge = config_.judge.judgeAt(note.timingMicros, judgedTime);
  result.noteId = selected;
  result.soundNoteId = selected;
  result.judge = judge;
  if (auto *laneState = findLane(note.lane)) {
    laneState->pressed = true;
  }
  result.hasLaneVisual = true;
  result.laneVisual = {LaneVisualAction::Press, note.lane,
                       context.laneBeamTimeMicros, judge};

  if (judge.judgement != None) {
    if (judge.isNotePlayed() && note.kind == NoteKind::LongTail) {
      return result;
    }
    if (judge.isNotePlayed()) {
      state.played = true;
      state.playedTimeMicros = judgedTime;
      if (note.kind == NoteKind::LongHead) {
        state.holding = true;
        if (note.pairId != kInvalidNoteId) {
          noteStates_[note.pairId].holding = true;
        }
        result.hasJudge = note.longNoteRule != LongNoteRule::Classic;
      } else {
        result.hasJudge = true;
      }
    } else {
      result.hasJudge = true;
    }
    result.hasReplayEvent = true;
    result.replayEvent = {
        .action = GameplayReplayAction::Press,
        .noteId = selected,
        .lane = note.lane,
        .noteTimeMicros = note.timingMicros,
        .songTimeMicros = judgedTime,
        .judgeTimeMicros = judgedTime,
        .judgement = judge.judgement,
        .diffMicros = judge.Diff,
    };
  }
  return result;
}
```

Keep `soundNoteId` as a stable note identity, not a direct `wav` call. The next
audio plan will resolve the note's copied `wav` to a realtime handle before a
session opens.

- [ ] **Step 4: Run press, definition, and practice-boundary tests**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_simulation_tests gameplay_practice_input_boundary_tests -j 6
ctest --test-dir cmake-build-debug -R '^(gameplay_simulation_tests|gameplay_practice_input_boundary_tests)$' --output-on-failure
```

Expected: both targets pass.

- [ ] **Step 5: Commit atomic press semantics**

```bash
git add src/scene/play/GameplaySimulation.cpp tests/gameplay_simulation_tests.cpp
git commit -m "feat: commit gameplay press transactions"
```

---

### Task 5: Add Long-Note Release Transactions

**Files:**

- Modify: `src/scene/play/GameplaySimulation.cpp`
- Modify: `tests/gameplay_simulation_tests.cpp`

**Interfaces:**

- Consumes: worker-owned lane/long-note state from press transactions.
- Produces: deterministic classic, charge, hell-charge, and scratch release judgement/replay/visual results with no keysound intent.

- [ ] **Step 1: Write failing release and scratch-backspin tests**

Add:

```cpp
void testClassicReleaseCommitsOneJudgeAndNoSound() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  addLongNote(*measure, 1'000'000, 1'500'000, 1,
              bms_parser::LongNoteType::LongNote);
  chart.Measures.push_back(measure);
  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1))});
  const auto press = simulation.pressLane(
      1, {.songTimeMicros = 1'000'000,
          .laneBeamTimeMicros = 2'000'000});
  const auto release = simulation.releaseLane(
      1, {.songTimeMicros = 1'500'000,
          .laneBeamTimeMicros = 2'500'000});

  require(release.noteId == definition.note(press.noteId).pairId,
          "release resolves the held long-note tail");
  require(release.soundNoteId == gameplay::kInvalidNoteId,
          "release does not create an input-triggered keysound");
  require(release.hasJudge && release.judge.judgement == PGreat,
          "classic release commits its combined judgement");
  require(!simulation.noteState(press.noteId).holding &&
              !simulation.noteState(release.noteId).holding,
          "release clears both long-note holding identities");
  require(!simulation.lanePressed(1) &&
              release.replayEvent.action ==
                  gameplay::GameplayReplayAction::Release,
          "release commits lane and replay state together");
}

void testChargeScratchRequiresBackspinRelease() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  auto *measure = new bms_parser::Measure();
  addLongNote(*measure, 1'000'000, 1'500'000, 7,
              bms_parser::LongNoteType::ChargeNote);
  chart.Measures.push_back(measure);
  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1))});
  simulation.pressLane(7, {.songTimeMicros = 1'000'000});
  const auto release = simulation.releaseLane(
      7, {.songTimeMicros = 1'500'000}, false);
  require(release.hasJudge && release.judge.judgement == Poor,
          "non-backspin scratch release is Poor");
}
```

Call both tests from `main()`.

- [ ] **Step 2: Run the test and verify release semantics fail**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_simulation_tests -j 6
ctest --test-dir cmake-build-debug -R '^gameplay_simulation_tests$' --output-on-failure
```

Expected: release assertions fail because Task 3 left a temporary body.

- [ ] **Step 3: Implement classic and charge release semantics**

Add local helpers to `GameplaySimulation.cpp`:

```cpp
JudgeResult normalizeReleaseJudge(const JudgeResult &judge) {
  if (judge.judgement == None || judge.judgement == Kpoor ||
      judge.judgement == Poor) {
    return JudgeResult(Bad, judge.Diff);
  }
  return judge;
}

std::int64_t absoluteDistance(std::int64_t value) {
  return value < 0 ? -value : value;
}
```

Replace `releaseLane()` with a complete transaction:

```cpp
GameplayInputResult GameplaySimulation::releaseLane(
    int lane, const GameplayInputContext &context, bool isBackSpin) {
  GameplayInputResult result;
  if (config_.allowedNoteRange.has_value() &&
      context.songTimeMicros >= config_.allowedNoteRange->endMicros) {
    return result;
  }
  auto *laneState = findLane(lane);
  if (laneState == nullptr || !laneState->pressed) {
    return result;
  }
  laneState->pressed = false;
  const std::int64_t judgedTime = inputTime(context);
  result.hasLaneVisual = true;
  result.laneVisual = {LaneVisualAction::Release, lane,
                       context.laneBeamTimeMicros, JudgeResult(None, 0)};

  const NoteId selected = selectReleaseCandidate(lane, judgedTime);
  if (selected == kInvalidNoteId) {
    result.hasReplayEvent = true;
    result.replayEvent = {
        .action = GameplayReplayAction::Release,
        .lane = lane,
        .songTimeMicros = judgedTime,
        .judgeTimeMicros = judgedTime,
    };
    return result;
  }

  const auto &tail = definition_.note(selected);
  auto &tailState = noteStates_[selected];
  result.noteId = selected;
  if (tail.kind != NoteKind::LongTail || !tailState.holding ||
      tail.pairId == kInvalidNoteId) {
    result.hasReplayEvent = true;
    result.replayEvent = {
        .action = GameplayReplayAction::Release,
        .lane = lane,
        .songTimeMicros = judgedTime,
        .judgeTimeMicros = judgedTime,
    };
    return result;
  }

  auto &headState = noteStates_[tail.pairId];
  tailState.played = true;
  tailState.playedTimeMicros = judgedTime;
  tailState.releaseTimeMicros = judgedTime;
  tailState.holding = false;
  headState.holding = false;

  const JudgeResult tailJudge =
      config_.judge.judgeAt(tail.timingMicros, judgedTime);
  JudgeResult applied = tailJudge;
  if (tail.longNoteRule == LongNoteRule::Classic) {
    const auto &head = definition_.note(tail.pairId);
    const JudgeResult headJudge =
        config_.judge.judgeAt(head.timingMicros, headState.playedTimeMicros);
    applied = normalizeReleaseJudge(
        absoluteDistance(tailJudge.Diff) > absoluteDistance(headJudge.Diff)
            ? tailJudge
            : headJudge);
  } else if (tail.scratchLane && !isBackSpin) {
    applied = JudgeResult(Poor, judgedTime - tail.timingMicros);
  } else {
    applied = normalizeReleaseJudge(tailJudge);
  }

  result.hasJudge = true;
  result.judge = applied;
  result.hasReplayEvent = true;
  result.replayEvent = {
      .action = GameplayReplayAction::Release,
      .noteId = selected,
      .lane = lane,
      .noteTimeMicros = tail.timingMicros,
      .songTimeMicros = judgedTime,
      .judgeTimeMicros = judgedTime,
      .judgement = applied.judgement,
      .diffMicros = applied.Diff,
  };
  return result;
}
```

- [ ] **Step 4: Run all focused transaction tests**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_simulation_tests gameplay_practice_input_boundary_tests logical_gameplay_input_tests -j 6
ctest --test-dir cmake-build-debug -R '^(gameplay_simulation_tests|gameplay_practice_input_boundary_tests|foundation_input_gameplay)$' --output-on-failure
```

Expected: all three tests pass.

- [ ] **Step 5: Commit long-note release transactions**

```bash
git add src/scene/play/GameplaySimulation.cpp tests/gameplay_simulation_tests.cpp
git commit -m "feat: commit gameplay release transactions"
```

---

### Task 6: Prove Current-Controller Parity and Freeze the Boundary

**Files:**

- Modify: `CMakeLists.txt`
- Modify: `tests/gameplay_simulation_tests.cpp`

**Interfaces:**

- Consumes: the completed simulation API and current `RhythmLaneInputController` as the parity oracle.
- Produces: regression coverage showing equivalent press/release outcomes and a frozen handoff boundary for automatic-deadline extraction.

- [ ] **Step 1: Link the existing controller into the focused parity target**

Add `src/scene/play/RhythmLaneInputController.cpp` to
`gameplay_simulation_tests`. Add the two renderer stubs at file scope in the
test, matching the existing practice-boundary test:

```cpp
#include "scene/play/BMSRenderer.h"
#include "scene/play/RhythmLaneInputController.h"

#include <unordered_map>

void BMSRenderer::onLanePressed(int, const JudgeResult, long long) {}
void BMSRenderer::onLaneReleased(int, long long) {}
```

- [ ] **Step 2: Write a failing parity matrix**

Add a helper that constructs two identical single-note charts and compares the
old and new outcomes:

```cpp
struct PressSummary {
  bool selected = false;
  bool sound = false;
  bool judged = false;
  Judgement judgement = None;
  bool replayed = false;
};

PressSummary oldPress(long long diffMicros,
                      AppSettings::NotePriorityMode priority) {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *timeline = addTimeline(*measure, 1'000'000);
  timeline->SetNote(1, new bms_parser::Note(5));
  chart.Measures.push_back(measure);
  std::unordered_map<int, bool> lanes{{1, false}};
  RhythmLaneInputController controller(&chart, nullptr, lanes, Judge(1));
  const auto result = controller.pressLane(
      1, {.songTimeMicros = 1'000'000 + diffMicros,
          .laneBeamTimeMicros = 2'000'000,
          .notePriorityMode = priority});
  return {result.note != nullptr, result.keySoundNote != nullptr,
          result.hasJudge, result.judge.judgement,
          result.hasReplayEvent};
}

PressSummary newPress(long long diffMicros,
                      AppSettings::NotePriorityMode priority) {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *timeline = addTimeline(*measure, 1'000'000);
  timeline->SetNote(1, new bms_parser::Note(5));
  chart.Measures.push_back(measure);
  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1)),
       .notePriorityMode = priority});
  const auto result = simulation.pressLane(
      1, {.songTimeMicros = 1'000'000 + diffMicros,
          .laneBeamTimeMicros = 2'000'000});
  return {result.noteId != gameplay::kInvalidNoteId,
          result.soundNoteId != gameplay::kInvalidNoteId,
          result.hasJudge, result.judge.judgement,
          result.hasReplayEvent};
}

void testCurrentPressParityMatrix() {
  for (const auto priority : {
           AppSettings::NotePriorityMode::Lowest,
           AppSettings::NotePriorityMode::Duration,
           AppSettings::NotePriorityMode::Combo,
           AppSettings::NotePriorityMode::Score}) {
    for (const long long diff : {-500'001LL, -500'000LL, -30'000LL, 0LL,
                                 30'000LL, 420'000LL, 420'001LL}) {
      const auto oldResult = oldPress(diff, priority);
      const auto newResult = newPress(diff, priority);
      require(oldResult.selected == newResult.selected &&
                  oldResult.sound == newResult.sound &&
                  oldResult.judged == newResult.judged &&
                  oldResult.judgement == newResult.judgement &&
                  oldResult.replayed == newResult.replayed,
              "new press transaction matches current controller outcome");
    }
  }
}

struct ReleaseSummary {
  bool selected = false;
  bool sound = false;
  bool judged = false;
  Judgement judgement = None;
  bool replayed = false;
  bool headHolding = false;
  bool tailHolding = false;
};

ReleaseSummary oldRelease(bms_parser::LongNoteType type, int lane,
                          bool isBackSpin, long long diffMicros) {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  auto *measure = new bms_parser::Measure();
  auto *head = addLongNote(*measure, 1'000'000, 1'500'000, lane, type);
  chart.Measures.push_back(measure);
  std::unordered_map<int, bool> lanes;
  RhythmLaneInputController controller(&chart, nullptr, lanes, Judge(1));
  head->Press(1'000'000);
  lanes[lane] = true;
  const auto result = controller.releaseLane(
      lane, {.songTimeMicros = 1'500'000 + diffMicros,
             .laneBeamTimeMicros = 2'000'000},
      isBackSpin);
  return {result.note != nullptr, result.keySoundNote != nullptr,
          result.hasJudge, result.judge.judgement,
          result.hasReplayEvent, head->IsHolding, head->Tail->IsHolding};
}

ReleaseSummary newRelease(bms_parser::LongNoteType type, int lane,
                          bool isBackSpin, long long diffMicros) {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  auto *measure = new bms_parser::Measure();
  addLongNote(*measure, 1'000'000, 1'500'000, lane, type);
  chart.Measures.push_back(measure);
  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1))});
  const auto press = simulation.pressLane(
      lane, {.songTimeMicros = 1'000'000,
             .laneBeamTimeMicros = 1'500'000});
  const auto tailId = definition.note(press.noteId).pairId;
  const auto result = simulation.releaseLane(
      lane, {.songTimeMicros = 1'500'000 + diffMicros,
             .laneBeamTimeMicros = 2'000'000},
      isBackSpin);
  return {result.noteId != gameplay::kInvalidNoteId,
          result.soundNoteId != gameplay::kInvalidNoteId,
          result.hasJudge, result.judge.judgement,
          result.hasReplayEvent, simulation.noteState(press.noteId).holding,
          simulation.noteState(tailId).holding};
}

void testCurrentReleaseParityMatrix() {
  struct ReleaseCase {
    bms_parser::LongNoteType type;
    int lane;
    bool isBackSpin;
    long long diffMicros;
  };
  for (const auto &entry : {
           ReleaseCase{bms_parser::LongNoteType::LongNote, 1, false, -30'000},
           ReleaseCase{bms_parser::LongNoteType::LongNote, 1, false, 0},
           ReleaseCase{bms_parser::LongNoteType::ChargeNote, 1, false, 30'000},
           ReleaseCase{bms_parser::LongNoteType::HellChargeNote, 1, false, 0},
           ReleaseCase{bms_parser::LongNoteType::ChargeNote, 7, false, 0},
           ReleaseCase{bms_parser::LongNoteType::ChargeNote, 7, true, 0}}) {
    const auto oldResult =
        oldRelease(entry.type, entry.lane, entry.isBackSpin, entry.diffMicros);
    const auto newResult =
        newRelease(entry.type, entry.lane, entry.isBackSpin, entry.diffMicros);
    require(oldResult.selected == newResult.selected &&
                oldResult.sound == newResult.sound &&
                oldResult.judged == newResult.judged &&
                oldResult.judgement == newResult.judgement &&
                oldResult.replayed == newResult.replayed &&
                oldResult.headHolding == newResult.headHolding &&
                oldResult.tailHolding == newResult.tailHolding,
            "new release transaction matches current controller outcome");
  }
}
```

Call both parity tests from `main()`:

```cpp
  testCurrentPressParityMatrix();
  testCurrentReleaseParityMatrix();
```

- [ ] **Step 3: Run the complete parity matrix**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_simulation_tests -j 6
ctest --test-dir cmake-build-debug -R '^gameplay_simulation_tests$' --output-on-failure
```

Expected: PASS. A failure blocks the task because the new simulation does not
match the current controller at a captured boundary; keep the failing row as a
regression and diagnose it with the systematic-debugging workflow before
continuing.

- [ ] **Step 4: Run the complete verification gate**

Run:

```bash
ctest --test-dir cmake-build-debug --output-on-failure -j 6
cmake --build cmake-build-debug --target main -j 6
git diff --check
```

Expected: all CTest tests pass, `main` builds, and `git diff --check` produces no
output.

- [ ] **Step 5: Commit the parity boundary**

```bash
git add CMakeLists.txt src/scene/play/CompiledGameplayJudge.cpp src/scene/play/GameplaySimulation.cpp tests/gameplay_simulation_tests.cpp
git commit -m "test: prove gameplay input transaction parity"
```

## Specification Coverage for This Subproject

- Stable note IDs, immutable copied definitions, and sorted per-lane indices:
  Task 2.
- Fixed judgement windows and allocation-free hot lookup: Task 1.
- Parser-independent runtime state and bounded two-lane candidate search:
  Task 3.
- Atomic press state plus matching sound identity, including rapid duplicate
  rejection: Task 4.
- Classic, charge, Hell Charge, and scratch long-note release state: Task 5.
- Current input semantic parity, practice boundaries, and full regression gate:
  Task 6.
- Every remaining approved-spec section is explicitly excluded by this plan's
  scope boundary and begins only after this API passes its completion gate.

## Completion Gate

This subproject is complete only when:

- `GameplayDefinition` contains no mutable parser-note pointer.
- `GameplaySimulation::pressLane()` and `releaseLane()` do not allocate, lock,
  call SDL, call bgfx, call `Jukebox`, or mutate `bms_parser` objects.
- Stable note IDs and per-lane indices replace chart-wide input scans in the new
  simulation.
- A returned `soundNoteId` always has its corresponding runtime state already
  committed; rejected/duplicate presses never return a sound identity.
- Normal, classic LN, charge/Hell Charge, scratch release, compensation lane,
  practice boundary, and priority behavior are covered.
- Current production gameplay behavior is unchanged because the new simulation
  is not yet connected as an authority.
- The full CTest suite and desktop `main` build pass.

## Next Planning Boundary

After this plan lands, the next independently reviewed plan moves automatic
misses, long-note deadlines, landmines, Hell Charge gauge ticks, score/gauge,
and replay accumulation into this runtime state. Only after that state is the
single synchronous authority will the worker/audio/snapshot plan connect it to
live gameplay.
