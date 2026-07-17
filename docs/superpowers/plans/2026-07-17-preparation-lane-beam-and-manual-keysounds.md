# Preparation Lane Beam and Manual Keysounds Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Allow normal-gameplay preparation input to show lane beams and submit manual keysounds without judgement, while making manual keysounds select the next or last pressable lane note outside judgement windows.

**Architecture:** Compile a press-only keysound index for every lane and use one allocation-free, binary-search selection policy from both `GameplaySimulation` and `RhythmLaneInputController`. Add explicit preparation press/release transactions that mutate only held-lane, visual, replay, and sound intent; the realtime worker continues to block automatic advancement until activation while reserving and committing preparation audio immediately.

**Tech Stack:** C++23, existing BMS parser model, `GameplayDefinition`, `GameplaySimulation`, `RealtimeGameplayWorker`, `RhythmLaneInputController`, SDL scene integration, CMake/CTest, Xcode iOS build-only verification.

## Global Constraints

- A judgeable press candidate always supplies the keysound before any fallback candidate is considered.
- Without a judgeable candidate, select the first normal note or long-note head at or after compensated input time across the main and compensation lanes.
- If neither candidate lane has a future pressable note, select the chronologically last eligible normal note or long-note head; played and dead state must not filter this fallback.
- Long-note tails and landmines are never manual press keysound sources.
- Practice candidates stay inside the half-open range `[startMicros, endMicros)`.
- Equal-time candidates prefer the main lane; stable chart order decides equal times within one lane.
- A selected `NoWav` note remains the selection and stays silent; do not skip forward to an audible note.
- Preserve automatic-keysound and replay-playback ownership rules so live manual submission is not duplicated.
- During the normal two-second lane indicator, input may mutate lane-held state, publish lane visuals and replay events, and submit a keysound, but may not resolve notes, judge, change score/combo/gauge, or advance automatic deadlines.
- A key held across activation must be released and pressed again before it can judge.
- Session-backed practice continues judging during count-in because it has no normal-gameplay activation gate.
- The high-frequency press path performs no allocation and no full-chart scan.
- Required audio keeps reserve-before-state and commit-after-state ordering; capacity or commit failure invalidates the realtime attempt through the existing fault path.
- Do not deploy. The iOS verification command is build-only.
- Review the combined implementation after Task 3, matching the requested three-task review cadence.

---

## File Structure

- Create `src/scene/play/ManualKeysoundSelection.h`: generic pure selection over two sorted lane spans, with no ownership or runtime-state dependency.
- Modify `src/scene/play/GameplayDefinition.h/.cpp`: retain sorted press-only `NoteId` spans beside the existing judgement/release lane spans.
- Modify `src/scene/play/GameplaySimulation.h/.cpp`: use the shared fallback and expose judgement-free preparation press/release transactions.
- Modify `src/scene/play/RhythmLaneInputController.h/.cpp`: build sorted pointer indexes once, use the shared fallback, skip long-note tails in ordinary press judgement, and expose matching preparation transactions.
- Modify `src/scene/play/RealtimeGameplayWorker.cpp`: process pre-activation input without advancing gameplay and retain existing audio fault ordering.
- Modify `src/scene/play/GamePlayScene.cpp`: route the legacy preparation branch through the controller so lane feedback and keysound ownership use the same result path.
- Modify `tests/gameplay_simulation_tests.cpp`: verify the pure selector, realtime definition index, fallback parity, dead-note behavior, excluded note kinds, and simulation preparation semantics.
- Modify `tests/gameplay_practice_input_boundary_tests.cpp`: verify legacy practice range filtering and dead long-head fallback.
- Modify `tests/realtime_gameplay_worker_tests.cpp`: verify pre-activation audio/visual transactions, deadline isolation, held-state behavior, and fail-closed reservation.

---

### Task 1: Shared selector and realtime press-only lane index

**Files:**
- Create: `src/scene/play/ManualKeysoundSelection.h`
- Modify: `src/scene/play/GameplayDefinition.h`
- Modify: `src/scene/play/GameplayDefinition.cpp`
- Test: `tests/gameplay_simulation_tests.cpp`

**Interfaces:**
- Produces: `gameplay::ManualKeysoundLane`, `gameplay::ManualKeysoundSelection`, and `gameplay::selectManualKeysound(std::span<const Entry>, std::span<const Entry>, std::int64_t, std::int64_t, std::int64_t, TimingProjection)`.
- Produces: `GameplayDefinition::laneKeysoundNotes(int) -> std::span<const NoteId>`.
- Preserves: `GameplayDefinition::laneNotes(int)` with long-note tails present for release/deadline logic.

- [ ] **Step 1: Write failing selector and definition-index tests**

Add the include and the following test fixture near the definition tests in `tests/gameplay_simulation_tests.cpp`:

```cpp
#include "scene/play/ManualKeysoundSelection.h"

struct KeysoundEntry {
  std::int64_t timingMicros = 0;
  int identity = 0;
};

void testManualKeysoundSelectionUsesFutureThenLastWithMainTies() {
  const std::array main{
      KeysoundEntry{.timingMicros = 100, .identity = 1},
      KeysoundEntry{.timingMicros = 300, .identity = 3},
  };
  const std::array compensation{
      KeysoundEntry{.timingMicros = 200, .identity = 2},
      KeysoundEntry{.timingMicros = 300, .identity = 4},
  };
  const auto timing = [](const KeysoundEntry &entry) {
    return entry.timingMicros;
  };

  const auto between = gameplay::selectManualKeysound(
      std::span<const KeysoundEntry>(main),
      std::span<const KeysoundEntry>(compensation), 150, 0, 1'000, timing);
  require(between.lane == gameplay::ManualKeysoundLane::Compensation &&
              between.index == 0,
          "the earliest future candidate wins across candidate lanes");

  const auto equalTime = gameplay::selectManualKeysound(
      std::span<const KeysoundEntry>(main),
      std::span<const KeysoundEntry>(compensation), 250, 0, 1'000, timing);
  require(equalTime.lane == gameplay::ManualKeysoundLane::Main &&
              equalTime.index == 1,
          "the main lane wins an equal-time future candidate");

  const auto afterAll = gameplay::selectManualKeysound(
      std::span<const KeysoundEntry>(main),
      std::span<const KeysoundEntry>(compensation), 400, 0, 1'000, timing);
  require(afterAll.lane == gameplay::ManualKeysoundLane::Main &&
              afterAll.index == 1,
          "the latest past candidate wins and keeps main-lane ties");

  const auto rangeFiltered = gameplay::selectManualKeysound(
      std::span<const KeysoundEntry>(main),
      std::span<const KeysoundEntry>(compensation), 0, 150, 250, timing);
  require(rangeFiltered.lane ==
                  gameplay::ManualKeysoundLane::Compensation &&
              rangeFiltered.index == 0,
          "half-open range filtering happens before future selection");

  const auto emptyRange = gameplay::selectManualKeysound(
      std::span<const KeysoundEntry>(main),
      std::span<const KeysoundEntry>(compensation), 0, 250, 250, timing);
  require(!emptyRange, "an empty half-open range has no keysound source");
}

void testDefinitionKeysoundIndexExcludesTailsAndLandmines() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *head = addLongNote(*measure, 1'000'000, 2'000'000, 1,
                           bms_parser::LongNoteType::ChargeNote);
  auto *mineTimeline = addTimeline(*measure, 2'500'000);
  mineTimeline->SetLandmineNote(1, new bms_parser::LandmineNote(5.0F));
  auto *normalTimeline = addTimeline(*measure, 3'000'000);
  auto *normal = new bms_parser::Note(9);
  normalTimeline->SetNote(1, normal);
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  const auto keysounds = definition.laneKeysoundNotes(1);
  require(keysounds.size() == 2 &&
              definition.note(keysounds[0]).kind ==
                  gameplay::NoteKind::LongHead &&
              definition.note(keysounds[1]).kind ==
                  gameplay::NoteKind::Normal &&
              definition.note(keysounds[0]).timingMicros ==
                  head->Timeline->Timing &&
              definition.note(keysounds[1]).timingMicros ==
                  normal->Timeline->Timing,
          "keysound index includes pressable notes and excludes tails and landmines");
}
```

Extend `testDefinitionUsesStableIdsAndLaneIndices()` after its existing long-note assertions:

```cpp
  const auto laneOneKeysounds = definition.laneKeysoundNotes(1);
  require(laneOneKeysounds.size() == 1 &&
              laneOneKeysounds.front() == headDefinition.id,
          "press keysound index includes the long head but excludes its tail");
  const auto laneTwoKeysounds = definition.laneKeysoundNotes(2);
  require(laneTwoKeysounds.size() == 1 &&
              definition.note(laneTwoKeysounds.front()).kind ==
                  gameplay::NoteKind::Normal,
          "press keysound index includes normal notes");
  require(definition.laneKeysoundNotes(99).empty(),
          "unknown keysound lanes return an empty span without allocation");
```

Register `testManualKeysoundSelectionUsesFutureThenLastWithMainTies()` and
`testDefinitionKeysoundIndexExcludesTailsAndLandmines()` immediately after
`testCompiledJudgePreservesResolvedWindows()` in `main()`.

- [ ] **Step 2: Build to verify RED**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_simulation_tests -j 6
```

Expected: compilation fails because `ManualKeysoundSelection.h` and `laneKeysoundNotes()` do not exist.

- [ ] **Step 3: Implement the allocation-free shared selector**

Create `src/scene/play/ManualKeysoundSelection.h` with this complete implementation:

```cpp
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <utility>

namespace gameplay {

enum class ManualKeysoundLane { None, Main, Compensation };

struct ManualKeysoundSelection {
  ManualKeysoundLane lane = ManualKeysoundLane::None;
  std::size_t index = 0;

  [[nodiscard]] explicit operator bool() const noexcept {
    return lane != ManualKeysoundLane::None;
  }
};

template <typename Entry, typename TimingProjection>
[[nodiscard]] ManualKeysoundSelection selectManualKeysound(
    std::span<const Entry> mainNotes,
    std::span<const Entry> compensationNotes,
    std::int64_t inputTimeMicros, std::int64_t rangeStartMicros,
    std::int64_t rangeEndMicros, TimingProjection timing) {
  if (rangeStartMicros >= rangeEndMicros) {
    return {};
  }

  struct Candidate {
    bool valid = false;
    std::size_t index = 0;
    std::int64_t timingMicros = 0;
  };
  const auto before = [&](const Entry &entry, std::int64_t value) {
    return timing(entry) < value;
  };
  const auto eligibleBounds = [&](std::span<const Entry> notes) {
    const auto begin = std::lower_bound(notes.begin(), notes.end(),
                                        rangeStartMicros, before);
    const auto end =
        std::lower_bound(begin, notes.end(), rangeEndMicros, before);
    return std::pair{begin, end};
  };
  const auto futureCandidate = [&](std::span<const Entry> notes) {
    const auto [begin, end] = eligibleBounds(notes);
    const auto found =
        std::lower_bound(begin, end, inputTimeMicros, before);
    return found == end
               ? Candidate{}
               : Candidate{.valid = true,
                           .index = static_cast<std::size_t>(
                               std::distance(notes.begin(), found)),
                           .timingMicros = timing(*found)};
  };
  const auto lastCandidate = [&](std::span<const Entry> notes) {
    const auto [begin, end] = eligibleBounds(notes);
    if (begin == end) {
      return Candidate{};
    }
    const auto found = std::prev(end);
    return Candidate{.valid = true,
                     .index = static_cast<std::size_t>(
                         std::distance(notes.begin(), found)),
                     .timingMicros = timing(*found)};
  };

  const Candidate mainFuture = futureCandidate(mainNotes);
  const Candidate compensationFuture = futureCandidate(compensationNotes);
  if (mainFuture.valid || compensationFuture.valid) {
    if (mainFuture.valid &&
        (!compensationFuture.valid ||
         mainFuture.timingMicros <= compensationFuture.timingMicros)) {
      return {ManualKeysoundLane::Main, mainFuture.index};
    }
    return {ManualKeysoundLane::Compensation,
            compensationFuture.index};
  }

  const Candidate mainLast = lastCandidate(mainNotes);
  const Candidate compensationLast = lastCandidate(compensationNotes);
  if (mainLast.valid &&
      (!compensationLast.valid ||
       mainLast.timingMicros >= compensationLast.timingMicros)) {
    return {ManualKeysoundLane::Main, mainLast.index};
  }
  if (compensationLast.valid) {
    return {ManualKeysoundLane::Compensation, compensationLast.index};
  }
  return {};
}

} // namespace gameplay
```

This implementation deliberately receives no played/dead predicate. Its only precondition is that each span is sorted by timing with stable chart order for ties.

- [ ] **Step 4: Add the press-only `NoteId` lane index**

Change `LaneDefinition` in `src/scene/play/GameplayDefinition.h` to:

```cpp
struct LaneDefinition {
  int lane = -1;
  std::vector<NoteId> noteIds;
  std::vector<NoteId> keysoundNoteIds;
};
```

Declare the getter directly after `laneNotes()`:

```cpp
[[nodiscard]] std::span<const NoteId>
laneKeysoundNotes(int lane) const noexcept;
```

Implement it beside `laneNotes()` in `src/scene/play/GameplayDefinition.cpp`:

```cpp
std::span<const NoteId>
GameplayDefinition::laneKeysoundNotes(int lane) const noexcept {
  const auto found =
      std::ranges::lower_bound(lanes_, lane, {}, &LaneDefinition::lane);
  return found != lanes_.end() && found->lane == lane
             ? std::span<const NoteId>(found->keysoundNoteIds)
             : std::span<const NoteId>();
}
```

Replace the final lane-index population/sort block in `buildGameplayDefinition()` with:

```cpp
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
    if (note.kind != NoteKind::LongTail) {
      lane->keysoundNoteIds.push_back(note.id);
    }
  }
  const auto noteTimingLess = [&](NoteId left, NoteId right) {
    const auto &leftNote = result.notes_[left];
    const auto &rightNote = result.notes_[right];
    return leftNote.timingMicros == rightNote.timingMicros
               ? left < right
               : leftNote.timingMicros < rightNote.timingMicros;
  };
  for (auto &lane : result.lanes_) {
    std::ranges::sort(lane.noteIds, noteTimingLess);
    std::ranges::sort(lane.keysoundNoteIds, noteTimingLess);
  }
```

- [ ] **Step 5: Run the focused test to verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_simulation_tests -j 6
./cmake-build-debug/gameplay_simulation_tests
```

Expected: build succeeds and the executable exits with status 0.

- [ ] **Step 6: Commit Task 1**

```bash
git add src/scene/play/ManualKeysoundSelection.h \
        src/scene/play/GameplayDefinition.h \
        src/scene/play/GameplayDefinition.cpp \
        tests/gameplay_simulation_tests.cpp
git commit -m "feat: index manual keysound candidates"
```

---

### Task 2: Manual fallback in realtime and legacy judgement paths

**Files:**
- Modify: `src/scene/play/GameplaySimulation.h`
- Modify: `src/scene/play/GameplaySimulation.cpp`
- Modify: `src/scene/play/RhythmLaneInputController.h`
- Modify: `src/scene/play/RhythmLaneInputController.cpp`
- Test: `tests/gameplay_simulation_tests.cpp`
- Test: `tests/gameplay_practice_input_boundary_tests.cpp`
- Test: `tests/realtime_gameplay_worker_tests.cpp`

**Interfaces:**
- Consumes: `selectManualKeysound()` and `GameplayDefinition::laneKeysoundNotes()` from Task 1.
- Produces: `GameplaySimulation::selectFallbackPressSoundNote(int, int, std::int64_t) -> NoteId` as a private allocation-free helper.
- Produces: `RhythmLaneInputController::selectFallbackKeysound(int, int, long long) -> bms_parser::Note *` as a private helper over construction-time indexes.
- Preserves: judge candidate priority, lane held-state behavior, replay payloads, and `NoWav` ownership at the audio layer.

- [ ] **Step 1: Write failing ordinary fallback/parity tests**

In the existing `testPressDoesNotClaimLongTail()` fixture in
`tests/gameplay_simulation_tests.cpp`, retain both lane identities:

```cpp
  const auto laneNotes = definition.laneNotes(1);
  const auto headId = laneNotes[0];
  const auto tailId = laneNotes[1];
```

Replace its first assertion with the new press-keysound contract:

```cpp
  require(press.noteId == gameplay::kInvalidNoteId &&
              press.soundNoteId == headId,
          "press near a long tail claims no note and reuses the pressable head keysound");
```

Add these helpers and tests in the parity section of `tests/gameplay_simulation_tests.cpp`:

```cpp
int legacyManualKeysoundAt(long long inputMicros, bool markLastDead = false) {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *firstTimeline = addTimeline(*measure, 1'000'000);
  auto *first = new bms_parser::Note(11);
  firstTimeline->SetNote(1, first);
  auto *lastTimeline = addTimeline(*measure, 2'000'000);
  auto *last = new bms_parser::Note(22);
  lastTimeline->SetNote(1, last);
  chart.Measures.push_back(measure);
  if (markLastDead) {
    last->IsPlayed = true;
    last->IsDead = true;
  }
  std::unordered_map<int, bool> lanes;
  RhythmLaneInputController controller(&chart, nullptr, lanes, Judge(1));
  const auto result = controller.pressLane(
      1, {.songTimeMicros = inputMicros,
          .laneBeamTimeMicros = inputMicros});
  return result.keySoundNote == nullptr ? bms_parser::Parser::NoWav
                                        : result.keySoundNote->Wav;
}

int simulationManualKeysoundAt(long long inputMicros,
                               bool resolveLastFirst = false) {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  addTimeline(*measure, 1'000'000)->SetNote(1, new bms_parser::Note(11));
  addTimeline(*measure, 2'000'000)->SetNote(1, new bms_parser::Note(22));
  chart.Measures.push_back(measure);
  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1))});
  if (resolveLastFirst) {
    (void)simulation.pressLane(1, {.songTimeMicros = 2'000'000});
    (void)simulation.releaseLane(1, {.songTimeMicros = 2'010'000});
  }
  const auto result = simulation.pressLane(
      1, {.songTimeMicros = inputMicros,
          .laneBeamTimeMicros = inputMicros});
  return result.soundNoteId == gameplay::kInvalidNoteId
             ? bms_parser::Parser::NoWav
             : definition.note(result.soundNoteId).wav;
}

void testManualKeysoundFallbackMatchesAcrossAuthorities() {
  for (const auto [inputMicros, expectedWav] : {
           std::pair{0LL, 11},
           std::pair{1'000'000LL, 11},
           std::pair{1'100'000LL, 11},
           std::pair{1'490'000LL, 22},
           std::pair{3'000'000LL, 22},
       }) {
    require(legacyManualKeysoundAt(inputMicros) == expectedWav &&
                simulationManualKeysoundAt(inputMicros) == expectedWav,
            "legacy and realtime select the same next-or-last keysound");
  }
  require(legacyManualKeysoundAt(3'000'000, true) == 22 &&
              simulationManualKeysoundAt(3'000'000, true) == 22,
          "the last pressed or dead note remains a fallback source");
}

void testFallbackTieAndNoWavDoNotSkipSelection() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  addTimeline(*measure, 2'000'000)
      ->SetNote(1, new bms_parser::Note(41));
  addTimeline(*measure, 2'000'000)
      ->SetNote(2, new bms_parser::Note(bms_parser::Parser::NoWav));
  addTimeline(*measure, 3'000'000)
      ->SetNote(2, new bms_parser::Note(42));
  chart.Measures.push_back(measure);

  std::unordered_map<int, bool> lanes;
  RhythmLaneInputController legacy(&chart, nullptr, lanes, Judge(1));
  const auto oldResult = legacy.pressLane(
      2, 1, {.songTimeMicros = 1'000'000,
             .laneBeamTimeMicros = 1'000'000});
  require(oldResult.keySoundNote != nullptr &&
              oldResult.keySoundNote->Lane == 2 &&
              oldResult.keySoundNote->Wav == bms_parser::Parser::NoWav,
          "main-lane equal-time NoWav is selected without skipping");

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1))});
  const auto newResult = simulation.pressLane(
      2, 1, {.songTimeMicros = 1'000'000,
             .laneBeamTimeMicros = 1'000'000});
  require(newResult.soundNoteId != gameplay::kInvalidNoteId &&
              definition.note(newResult.soundNoteId).lane == 2 &&
              definition.note(newResult.soundNoteId).wav ==
                  bms_parser::Parser::NoWav,
          "realtime keeps the same main-lane NoWav selection");
}

void testLongTailIsNotAManualPressKeysound() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *head = addLongNote(*measure, 1'000'000, 2'000'000, 1,
                           bms_parser::LongNoteType::ChargeNote);
  head->Wav = 51;
  head->Tail->Wav = 52;
  chart.Measures.push_back(measure);

  std::unordered_map<int, bool> lanes;
  RhythmLaneInputController legacy(&chart, nullptr, lanes, Judge(1));
  const auto oldResult = legacy.pressLane(
      1, {.songTimeMicros = 2'000'000,
          .laneBeamTimeMicros = 2'000'000});
  require(oldResult.note == nullptr && oldResult.keySoundNote == head,
          "legacy skips a judgeable long tail and falls back to its head");

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1))});
  const auto newResult = simulation.pressLane(
      1, {.songTimeMicros = 2'000'000,
          .laneBeamTimeMicros = 2'000'000});
  require(newResult.noteId == gameplay::kInvalidNoteId &&
              newResult.soundNoteId != gameplay::kInvalidNoteId &&
              definition.note(newResult.soundNoteId).kind ==
                  gameplay::NoteKind::LongHead,
          "realtime press fallback excludes the long tail");
}
```

Register the three test functions immediately before the existing press parity matrix in `main()`.

In `tests/realtime_gameplay_worker_tests.cpp`, replace the final wait and
assertion in `testPracticeCountInPressOutsideJudgeWindowStaysUnjudged()` with:

```cpp
  require(waitUntil([&] {
            return audio.commitCount.load(std::memory_order_acquire) == 1;
          }),
          "far-early count-in press commits the first in-range manual keysound");
  auto snapshot = worker.acquireLatestSnapshot();
  require(snapshot && !snapshot->noteStates[0].played &&
              snapshot->attempt.judgeCounts ==
                  std::array<int, JudgementCount>{} &&
              audio.commitCount.load() == 1,
          "count-in press outside every judge window sounds but stays unjudged");
  worker.stop();
```

The input timestamp remains `499'999`, one microsecond outside the earliest
`Kpoor` edge for the note at `1'000'000`.

In `tests/gameplay_practice_input_boundary_tests.cpp`, add this exact range regression and register it after `testActualInputAndJudgeRange()`:

```cpp
void testManualKeysoundFallbackRespectsPracticeRangeAndDeadHeads() {
  constexpr long long startMicros = 1'000'000;
  constexpr long long endMicros = 2'000'000;
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *before = addNote(*measure, 500'000, 1);
  before->Wav = 5;
  auto *inside = addLongNote(*measure, 1'500'000, 1'700'000, 1,
                             bms_parser::LongNoteType::ChargeNote);
  inside->Wav = 15;
  inside->Tail->Wav = 17;
  auto *atEnd = addNote(*measure, endMicros, 1);
  atEnd->Wav = 20;
  auto *mineTimeline = addTimeline(*measure, 1'800'000);
  mineTimeline->SetLandmineNote(1, new bms_parser::LandmineNote(5.0F));
  chart.Measures.push_back(measure);

  inside->IsPlayed = true;
  inside->IsDead = true;
  inside->Tail->IsPlayed = true;
  inside->Tail->IsDead = true;
  std::unordered_map<int, bool> lanePressed;
  RhythmLaneInputController controller(
      &chart, nullptr, lanePressed, Judge(chart.Meta.Rank), 0,
      NoteTimeRange{.startMicros = startMicros, .endMicros = endMicros});
  const auto result = controller.pressLane(
      1, {.songTimeMicros = endMicros - 1,
          .laneBeamTimeMicros = endMicros - 1});

  require(result.note == nullptr && result.keySoundNote == inside &&
              result.keySoundNote != before &&
              result.keySoundNote != inside->Tail &&
              result.keySoundNote != atEnd,
          "practice fallback includes the dead head but excludes outside, tail, and mine identities");
}
```

- [ ] **Step 2: Run focused tests to verify RED**

Run:

```bash
cmake --build cmake-build-debug \
  --target gameplay_simulation_tests gameplay_practice_input_boundary_tests \
           realtime_gameplay_worker_tests \
  -j 6
./cmake-build-debug/gameplay_simulation_tests
./cmake-build-debug/gameplay_practice_input_boundary_tests
./cmake-build-debug/realtime_gameplay_worker_tests
```

Expected: all three executables build, then the new assertions fail because
empty and out-of-window presses still return no keysound; the long-tail legacy
assertion also fails before the tail filter is added.

- [ ] **Step 3: Add realtime fallback selection without runtime-state filtering**

Include `ManualKeysoundSelection.h` from `src/scene/play/GameplaySimulation.cpp` and declare this private method in `GameplaySimulation.h` beside `selectPressCandidate()`:

```cpp
[[nodiscard]] NoteId
selectFallbackPressSoundNote(int mainLane, int compensateLane,
                             std::int64_t inputTimeMicros) const;
```

Implement it before `selectPressCandidate()`:

```cpp
NoteId GameplaySimulation::selectFallbackPressSoundNote(
    int mainLane, int compensateLane,
    std::int64_t inputTimeMicros) const {
  const auto *mainState = findLane(mainLane);
  const auto *compensationState = findLane(compensateLane);
  const auto mainNotes = mainState != nullptr && !mainState->pressed
                             ? definition_.laneKeysoundNotes(mainLane)
                             : std::span<const NoteId>();
  const auto compensationNotes =
      compensateLane != mainLane && compensationState != nullptr &&
              !compensationState->pressed
          ? definition_.laneKeysoundNotes(compensateLane)
          : std::span<const NoteId>();
  const std::int64_t rangeStart =
      config_.allowedNoteRange.has_value()
          ? config_.allowedNoteRange->startMicros
          : std::numeric_limits<std::int64_t>::min();
  const std::int64_t rangeEnd =
      config_.allowedNoteRange.has_value()
          ? config_.allowedNoteRange->endMicros
          : std::numeric_limits<std::int64_t>::max();
  const auto selected = selectManualKeysound(
      mainNotes, compensationNotes, inputTimeMicros, rangeStart, rangeEnd,
      [&](NoteId id) { return definition_.note(id).timingMicros; });
  switch (selected.lane) {
  case ManualKeysoundLane::Main:
    return mainNotes[selected.index];
  case ManualKeysoundLane::Compensation:
    return compensationNotes[selected.index];
  case ManualKeysoundLane::None:
    return kInvalidNoteId;
  }
  return kInvalidNoteId;
}
```

Change `previewPressSoundNote()` so its final selection is:

```cpp
  const std::int64_t judgedTime = inputTime(context);
  const NoteId judgeCandidate =
      selectPressCandidate(mainLane, compensateLane, judgedTime);
  return judgeCandidate != kInvalidNoteId
             ? judgeCandidate
             : selectFallbackPressSoundNote(mainLane, compensateLane,
                                            judgedTime);
```

In `pressLane(int, int, const GameplayInputContext &)`, keep the existing judge selection, but set fallback sound before the no-candidate transaction is recorded:

```cpp
  const NoteId selected =
      selectPressCandidate(mainLane, compensateLane, judgedTime);
  if (selected == kInvalidNoteId) {
    result.soundNoteId = selectFallbackPressSoundNote(
        mainLane, compensateLane, judgedTime);
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
    recordReplay(result.replayEvent);
    result.hasLaneVisual = true;
    result.laneVisual = {LaneVisualAction::Press, mainLane,
                         context.laneBeamTimeMicros,
                         JudgeResult(None, 0)};
    finishTransaction(judgedTime);
    return result;
  }
```

Do not add played/dead inspection to `selectFallbackPressSoundNote()`; the immutable definition index is the mechanism that keeps dead last notes eligible.

- [ ] **Step 4: Build the legacy pointer index and apply the same fallback**

Add `<span>` and `<vector>` to `RhythmLaneInputController.h`, then add these private members:

```cpp
std::unordered_map<int, std::vector<bms_parser::Note *>>
    keysoundNotesByLane;

void indexKeysoundNotes();
[[nodiscard]] bms_parser::Note *
selectFallbackKeysound(int mainLane, int compensateLane,
                       long long inputTime) const;
```

Include `ManualKeysoundSelection.h`, `<algorithm>`, `<limits>`, and `<span>` in `RhythmLaneInputController.cpp`. Call `indexKeysoundNotes();` immediately before `resetLaneStates();` in the constructor, and add these methods after the constructor:

```cpp
void RhythmLaneInputController::indexKeysoundNotes() {
  keysoundNotesByLane.clear();
  if (chart == nullptr) {
    return;
  }
  for (const auto *measure : chart->Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      for (auto *note : timeline->Notes) {
        if (note == nullptr || note->IsLandmineNote()) {
          continue;
        }
        const auto *longNote =
            dynamic_cast<const bms_parser::LongNote *>(note);
        if (longNote != nullptr && longNote->IsTail()) {
          continue;
        }
        keysoundNotesByLane[note->Lane].push_back(note);
      }
    }
  }
  for (auto &[lane, notes] : keysoundNotesByLane) {
    (void)lane;
    std::stable_sort(notes.begin(), notes.end(),
                     [](const bms_parser::Note *left,
                        const bms_parser::Note *right) {
                       return noteTimingMicros(left) <
                              noteTimingMicros(right);
                     });
  }
}

bms_parser::Note *RhythmLaneInputController::selectFallbackKeysound(
    int mainLane, int compensateLane, long long inputTime) const {
  const auto notesFor = [&](int lane, bool available) {
    if (!available) {
      return std::span<bms_parser::Note *const>();
    }
    const auto found = keysoundNotesByLane.find(lane);
    return found == keysoundNotesByLane.end()
               ? std::span<bms_parser::Note *const>()
               : std::span<bms_parser::Note *const>(found->second);
  };
  const auto mainState = lanePressed.find(mainLane);
  const auto compensationState = lanePressed.find(compensateLane);
  const auto mainNotes = notesFor(
      mainLane, mainState != lanePressed.end() && !mainState->second);
  const auto compensationNotes = notesFor(
      compensateLane,
      compensateLane != mainLane && compensationState != lanePressed.end() &&
          !compensationState->second);
  const long long rangeStart =
      allowedNoteRange.has_value()
          ? allowedNoteRange->startMicros
          : std::numeric_limits<long long>::min();
  const long long rangeEnd =
      allowedNoteRange.has_value()
          ? allowedNoteRange->endMicros
          : std::numeric_limits<long long>::max();
  const auto selected = gameplay::selectManualKeysound(
      mainNotes, compensationNotes, inputTime, rangeStart, rangeEnd,
      [](const bms_parser::Note *note) {
        return noteTimingMicros(note);
      });
  switch (selected.lane) {
  case gameplay::ManualKeysoundLane::Main:
    return mainNotes[selected.index];
  case gameplay::ManualKeysoundLane::Compensation:
    return compensationNotes[selected.index];
  case gameplay::ManualKeysoundLane::None:
    return nullptr;
  }
  return nullptr;
}
```

In the existing ordinary judgement scan, extend the skip condition so a long-note tail cannot become a press candidate:

```cpp
        auto *note = timeline->Notes[lane];
        const auto *longNote =
            dynamic_cast<const bms_parser::LongNote *>(note);
        if (note == nullptr || note->IsPlayed || note->IsLandmineNote() ||
            (longNote != nullptr && longNote->IsTail())) {
          continue;
        }
```

Immediately before the current empty-press lane mutation block, assign the fallback:

```cpp
  result.keySoundNote =
      selectFallbackKeysound(mainLane, compensateLane, inputTime);
```

The judgeable branch continues returning `pressNote()` unchanged, so its `keySoundNote` retains priority over fallback selection.

- [ ] **Step 5: Run both focused suites to verify GREEN**

Run:

```bash
cmake --build cmake-build-debug \
  --target gameplay_simulation_tests gameplay_practice_input_boundary_tests \
           realtime_gameplay_worker_tests \
  -j 6
./cmake-build-debug/gameplay_simulation_tests
./cmake-build-debug/gameplay_practice_input_boundary_tests
./cmake-build-debug/realtime_gameplay_worker_tests
```

Expected: all three executables exit with status 0, including dead-note, tail
exclusion, `NoWav`, main-lane tie, practice-range, and far-early manual
keysound regressions.

- [ ] **Step 6: Commit Task 2**

```bash
git add src/scene/play/GameplaySimulation.h \
        src/scene/play/GameplaySimulation.cpp \
        src/scene/play/RhythmLaneInputController.h \
        src/scene/play/RhythmLaneInputController.cpp \
        tests/gameplay_simulation_tests.cpp \
        tests/gameplay_practice_input_boundary_tests.cpp \
        tests/realtime_gameplay_worker_tests.cpp
git commit -m "fix: select manual keysounds outside judgement windows"
```

---

### Task 3: Judgement-free preparation transactions and immediate audio

**Files:**
- Modify: `src/scene/play/GameplaySimulation.h`
- Modify: `src/scene/play/GameplaySimulation.cpp`
- Modify: `src/scene/play/RhythmLaneInputController.h`
- Modify: `src/scene/play/RhythmLaneInputController.cpp`
- Modify: `src/scene/play/RealtimeGameplayWorker.cpp`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Test: `tests/gameplay_simulation_tests.cpp`
- Test: `tests/realtime_gameplay_worker_tests.cpp`

**Interfaces:**
- Produces in `GameplaySimulation`: `previewPreparationPressSoundNote(int, int, const GameplayInputContext &) -> NoteId`, `pressLaneForPreparation(int, int, const GameplayInputContext &) -> GameplayInputResult`, and `releaseLaneForPreparation(int, const GameplayInputContext &) -> GameplayInputResult`.
- Produces in `RhythmLaneInputController`: `pressLaneForPreparation(int, int, const InputContext &) -> Result` and `releaseLaneForPreparation(int, const InputContext &) -> Result`.
- Consumes in `RealtimeGameplayWorker`: the existing `activationSongTimeMicros`; timestamps before it choose preparation methods and skip `advanceTo()`.
- Preserves: the worker's ordinary press reservation/consistency/commit sequence and `advanceAutomatic()` activation gate.

- [ ] **Step 1: Write failing simulation preparation tests**

Add this function to `tests/gameplay_simulation_tests.cpp` and register it after the ordinary fallback tests:

```cpp
void testPreparationTransactionsOnlyChangeLaneReplayVisualAndSound() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 1;
  auto *measure = new bms_parser::Measure();
  addTimeline(*measure, 1'000'000)->SetNote(1, new bms_parser::Note(61));
  chart.Measures.push_back(measure);
  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1))});
  const auto before = simulation.snapshot();
  const gameplay::GameplayInputContext preparation{
      .songTimeMicros = 900'000,
      .laneBeamTimeMicros = 90,
  };

  const auto preview =
      simulation.previewPreparationPressSoundNote(1, 1, preparation);
  const auto press =
      simulation.pressLaneForPreparation(1, 1, preparation);
  require(preview == press.soundNoteId &&
              press.soundNoteId != gameplay::kInvalidNoteId &&
              press.noteId == gameplay::kInvalidNoteId && !press.hasJudge &&
              press.hasLaneVisual &&
              press.laneVisual.action == gameplay::LaneVisualAction::Press &&
              press.hasReplayEvent && simulation.lanePressed(1),
          "preparation press publishes sound, lane visual, replay, and held state only");
  require(!simulation.noteState(0).played &&
              !simulation.noteState(0).dead &&
              sameAttemptSnapshot(before, simulation.snapshot()),
          "preparation press does not resolve gameplay state");

  const auto duplicate =
      simulation.pressLaneForPreparation(1, 1, preparation);
  require(duplicate.soundNoteId == gameplay::kInvalidNoteId &&
              !duplicate.hasLaneVisual && !duplicate.hasReplayEvent,
          "a held preparation lane rejects duplicate presses");

  const auto activeWhileHeld =
      simulation.pressLane(1, {.songTimeMicros = 1'000'000,
                               .laneBeamTimeMicros = 100});
  require(activeWhileHeld.noteId == gameplay::kInvalidNoteId &&
              !simulation.noteState(0).played,
          "a key held through activation cannot judge automatically");

  const auto release = simulation.releaseLaneForPreparation(
      1, {.songTimeMicros = 1'010'000, .laneBeamTimeMicros = 101});
  require(release.hasLaneVisual &&
              release.laneVisual.action ==
                  gameplay::LaneVisualAction::Release &&
              release.hasReplayEvent && !release.hasJudge &&
              !simulation.lanePressed(1),
          "preparation release clears the held lane without judgement");

  const auto activeAfterRelease =
      simulation.pressLane(1, {.songTimeMicros = 1'020'000,
                               .laneBeamTimeMicros = 102});
  require(activeAfterRelease.noteId != gameplay::kInvalidNoteId &&
              activeAfterRelease.hasJudge &&
              simulation.noteState(activeAfterRelease.noteId).played,
          "release and repress after activation follows ordinary judgement");
}

void testLegacyPreparationControllerCommitsSoundAndHeldStateOnly() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *timeline = addTimeline(*measure, 1'000'000);
  auto *note = new bms_parser::Note(71);
  timeline->SetNote(1, note);
  chart.Measures.push_back(measure);
  std::unordered_map<int, bool> lanes;
  RhythmLaneInputController controller(&chart, nullptr, lanes, Judge(1));

  const RhythmLaneInputController::InputContext preparation{
      .songTimeMicros = 900'000,
      .laneBeamTimeMicros = 90,
  };
  const auto press =
      controller.pressLaneForPreparation(1, 1, preparation);
  require(press.note == nullptr && press.keySoundNote == note &&
              !press.hasJudge && press.hasReplayEvent && lanes.at(1) &&
              !note->IsPlayed && !note->IsDead,
          "legacy preparation press commits sound, replay, and held state without judgement");

  const auto release =
      controller.releaseLaneForPreparation(1, preparation);
  require(release.note == nullptr && release.keySoundNote == nullptr &&
              !release.hasJudge && release.hasReplayEvent && !lanes.at(1) &&
              !note->IsPlayed && !note->IsDead,
          "legacy preparation release clears held state without note mutation");
}
```

Register both preparation test functions immediately before the existing
release parity tests in `main()`.

- [ ] **Step 2: Replace the worker activation-drop regression with preparation behavior tests**

Extend `FakeAudio` in `tests/realtime_gameplay_worker_tests.cpp` with:

```cpp
std::atomic<gameplay::NoteId> lastCommitted{gameplay::kInvalidNoteId};
```

Add this value comparison helper after the existing `require()` helper in the
same test file:

```cpp
bool sameAttemptSnapshot(const gameplay::GameplayAttemptSnapshot &left,
                         const gameplay::GameplayAttemptSnapshot &right) {
  return left.judgeCounts == right.judgeCounts &&
         left.combo == right.combo && left.maxCombo == right.maxCombo &&
         left.comboBreak == right.comboBreak && left.score == right.score &&
         left.gauge == right.gauge && left.gaugeType == right.gaugeType &&
         left.clearTypeRank == right.clearTypeRank;
}
```

Set it in `FakeAudio::commit()` immediately before incrementing `commitCount`:

```cpp
    self.lastCommitted.store(noteId, std::memory_order_release);
```

Replace `testActivationGateRejectsPreparationInputAndDeadlines()` with:

```cpp
void testActivationGateAllowsPreparationFeedbackButNoGameplay() {
  FakeClock clock;
  FakeAudio audio;
  auto config = makeConfig(clock, audio);
  config.activationSongTimeMicros = 1'000'000;
  gameplay::RealtimeGameplayWorker worker(makeRapidDefinition(),
                                           std::move(config));
  const auto initial = worker.acquireLatestSnapshot()->attempt;
  require(worker.start(), "activation-gate fixture starts");
  require(worker.enqueueInput({.epoch = 7,
                               .type = gameplay::RealtimeGameplayInputType::Press,
                               .lane = 1,
                               .compensateLane = 1,
                               .steadyTimestampMicros = 900'000}),
          "preparation press reaches the serial authority");
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return audio.commitCount.load(std::memory_order_acquire) == 1 &&
                   snapshot && snapshot->transactionSequence >= 1;
          }),
          "preparation keysound and visual transaction commit without a frame pump");
  {
    auto snapshot = worker.acquireLatestSnapshot();
    const auto &transaction = snapshot->latestTransaction;
    require(snapshot->lanePressed[1] &&
                audio.lastCommitted.load(std::memory_order_acquire) == 0 &&
                transaction.soundNoteId == 0 &&
                transaction.noteId == gameplay::kInvalidNoteId &&
                !transaction.hasJudge && transaction.hasLaneVisual &&
                transaction.laneVisual.action ==
                    gameplay::LaneVisualAction::Press &&
                transaction.hasReplayEvent &&
                !snapshot->noteStates[0].played &&
                !snapshot->noteStates[0].dead &&
                sameAttemptSnapshot(initial, snapshot->attempt),
            "preparation feedback leaves note, judgement, score, combo, and gauge untouched");
  }

  clock.nowMicros.store(999'999, std::memory_order_release);
  std::this_thread::sleep_for(10ms);
  {
    auto snapshot = worker.acquireLatestSnapshot();
    require(snapshot && !snapshot->noteStates[0].dead &&
                snapshot->attempt.judgeCounts[Poor] == 0,
            "automatic miss deadlines remain blocked before activation");
  }

  require(worker.enqueueInput({.epoch = 7,
                               .type = gameplay::RealtimeGameplayInputType::Press,
                               .lane = 1,
                               .compensateLane = 1,
                               .steadyTimestampMicros = 1'000'000}),
          "held-lane active press reaches the serial authority");
  std::this_thread::sleep_for(10ms);
  require(audio.commitCount.load() == 1,
          "held input crossing activation cannot retrigger sound or judgement");
  {
    auto snapshot = worker.acquireLatestSnapshot();
    require(snapshot && !snapshot->noteStates[0].played,
            "held input crossing activation leaves the note unresolved");
  }

  require(worker.enqueueInput({.epoch = 7,
                               .type = gameplay::RealtimeGameplayInputType::Release,
                               .lane = 1,
                               .steadyTimestampMicros = 1'001'000}),
          "post-activation release reaches the authority");
  require(worker.enqueueInput({.epoch = 7,
                               .type = gameplay::RealtimeGameplayInputType::Press,
                               .lane = 1,
                               .compensateLane = 1,
                               .steadyTimestampMicros = 1'005'000}),
          "post-activation repress reaches the authority");
  require(waitUntil([&] { return audio.commitCount.load() == 2; }),
          "release and repress commits the ordinary judged keysound");
  {
    auto snapshot = worker.acquireLatestSnapshot();
    require(snapshot && snapshot->noteStates[0].played &&
                snapshot->attempt.judgeCounts[PGreat] == 1,
            "post-activation repress judges normally");
  }
  worker.stop();
}

void testPreparationReleasePublishesOrderedVisualTransaction() {
  FakeClock clock;
  FakeAudio audio;
  auto config = makeConfig(clock, audio);
  config.activationSongTimeMicros = 1'000'000;
  gameplay::RealtimeGameplayWorker worker(makeRapidDefinition(),
                                           std::move(config));
  require(worker.start(), "preparation release fixture starts");
  require(worker.enqueueInput({.epoch = 7,
                               .type = gameplay::RealtimeGameplayInputType::Press,
                               .lane = 1,
                               .compensateLane = 1,
                               .steadyTimestampMicros = 900'000}) &&
              worker.enqueueInput({.epoch = 7,
                                   .type = gameplay::RealtimeGameplayInputType::Release,
                                   .lane = 1,
                                   .steadyTimestampMicros = 910'000}),
          "preparation press and release enter the serial authority");
  require(waitUntil([&] {
            auto snapshot = worker.acquireLatestSnapshot();
            return snapshot && snapshot->transactionSequence >= 2;
          }),
          "both preparation transactions publish without a frame pump");
  auto snapshot = worker.acquireLatestSnapshot();
  require(snapshot && !snapshot->lanePressed[1] &&
              snapshot->transactionCount >= 2 &&
              snapshot->transactions[0].result.laneVisual.action ==
                  gameplay::LaneVisualAction::Press &&
              snapshot->transactions[1].result.laneVisual.action ==
                  gameplay::LaneVisualAction::Release &&
              !snapshot->transactions[1].result.hasJudge,
          "preparation lane visuals preserve press-release order");
  worker.stop();
}

void testPreparationAudioReservationFailureDoesNotClaimLane() {
  FakeClock clock;
  FakeAudio audio;
  audio.allowReserve.store(false, std::memory_order_release);
  auto config = makeConfig(clock, audio);
  config.activationSongTimeMicros = 1'000'000;
  gameplay::RealtimeGameplayWorker worker(makeRapidDefinition(),
                                           std::move(config));
  require(worker.start(), "preparation audio-fault fixture starts");
  require(worker.enqueueInput({.epoch = 7,
                               .type = gameplay::RealtimeGameplayInputType::Press,
                               .lane = 1,
                               .compensateLane = 1,
                               .steadyTimestampMicros = 900'000}),
          "preparation audio-fault press enters the authority");
  require(waitUntil([&] {
            return worker.fault() ==
                   gameplay::RealtimeGameplayFault::AudioCapacityUnavailable;
          }),
          "preparation audio reservation failure faults the attempt");
  auto snapshot = worker.acquireLatestSnapshot();
  require(snapshot && !snapshot->lanePressed[1] &&
              !snapshot->noteStates[0].played &&
              snapshot->transactionSequence == 0,
          "failed preparation reservation commits no lane or gameplay state");
  worker.stop();
}
```

Register these three functions where the old activation-gate test was registered.

- [ ] **Step 3: Build to verify RED**

Run:

```bash
cmake --build cmake-build-debug \
  --target gameplay_simulation_tests realtime_gameplay_worker_tests -j 6
```

Expected: compilation fails because the preparation simulation methods do not exist.

- [ ] **Step 4: Implement explicit simulation preparation transactions**

Add these public declarations immediately after the ordinary press/release declarations in `GameplaySimulation.h`:

```cpp
[[nodiscard]] NoteId previewPreparationPressSoundNote(
    int mainLane, int compensateLane,
    const GameplayInputContext &context) const;
GameplayInputResult pressLaneForPreparation(
    int mainLane, int compensateLane,
    const GameplayInputContext &context);
GameplayInputResult releaseLaneForPreparation(
    int lane, const GameplayInputContext &context);
```

Implement them immediately before ordinary `pressLane()` in `GameplaySimulation.cpp`:

```cpp
NoteId GameplaySimulation::previewPreparationPressSoundNote(
    int mainLane, int compensateLane,
    const GameplayInputContext &context) const {
  if (terminal() ||
      (config_.allowedNoteRange.has_value() &&
       context.songTimeMicros >= config_.allowedNoteRange->endMicros)) {
    return kInvalidNoteId;
  }
  return selectFallbackPressSoundNote(mainLane, compensateLane,
                                      inputTime(context));
}

GameplayInputResult GameplaySimulation::pressLaneForPreparation(
    int mainLane, int compensateLane,
    const GameplayInputContext &context) {
  GameplayInputResult result;
  if (terminal() ||
      (config_.allowedNoteRange.has_value() &&
       context.songTimeMicros >= config_.allowedNoteRange->endMicros)) {
    return result;
  }
  auto *mainState = findLane(mainLane);
  const auto *compensationState = findLane(compensateLane);
  if ((mainState == nullptr || mainState->pressed) &&
      (compensateLane == mainLane || compensationState == nullptr ||
       compensationState->pressed)) {
    return result;
  }

  const std::int64_t eventTime = inputTime(context);
  result.soundNoteId = selectFallbackPressSoundNote(
      mainLane, compensateLane, eventTime);
  if (mainState != nullptr) {
    mainState->pressed = true;
  }
  result.hasLaneVisual = true;
  result.laneVisual = {LaneVisualAction::Press, mainLane,
                       context.laneBeamTimeMicros, JudgeResult(None, 0)};
  result.hasReplayEvent = true;
  result.replayEvent = {
      .action = GameplayReplayAction::Press,
      .lane = mainLane,
      .songTimeMicros = eventTime,
      .judgeTimeMicros = eventTime,
  };
  recordReplay(result.replayEvent);
  finishTransaction(eventTime);
  return result;
}

GameplayInputResult GameplaySimulation::releaseLaneForPreparation(
    int lane, const GameplayInputContext &context) {
  GameplayInputResult result;
  if (terminal() ||
      (config_.allowedNoteRange.has_value() &&
       context.songTimeMicros >= config_.allowedNoteRange->endMicros)) {
    return result;
  }
  auto *laneState = findLane(lane);
  if (laneState == nullptr || !laneState->pressed) {
    return result;
  }
  laneState->pressed = false;
  const std::int64_t eventTime = inputTime(context);
  result.hasLaneVisual = true;
  result.laneVisual = {LaneVisualAction::Release, lane,
                       context.laneBeamTimeMicros, JudgeResult(None, 0)};
  result.hasReplayEvent = true;
  result.replayEvent = {
      .action = GameplayReplayAction::Release,
      .lane = lane,
      .songTimeMicros = eventTime,
      .judgeTimeMicros = eventTime,
  };
  recordReplay(result.replayEvent);
  finishTransaction(eventTime);
  return result;
}
```

These methods intentionally do not call `advanceTo()`, `selectPressCandidate()`, `selectReleaseCandidate()`, `commitJudge()`, or any gauge mutator.

- [ ] **Step 5: Add matching legacy preparation methods**

Declare these public methods beside ordinary press/release in `RhythmLaneInputController.h`:

```cpp
Result pressLaneForPreparation(int mainLane, int compensateLane,
                               const InputContext &context);
Result releaseLaneForPreparation(int lane, const InputContext &context);
```

Implement them immediately before ordinary `pressLane()` in `RhythmLaneInputController.cpp`:

```cpp
RhythmLaneInputController::Result
RhythmLaneInputController::pressLaneForPreparation(
    int mainLane, int compensateLane, const InputContext &context) {
  Result result;
  if (chart == nullptr ||
      (allowedNoteRange.has_value() &&
       context.songTimeMicros >= allowedNoteRange->endMicros)) {
    return result;
  }
  auto mainState = lanePressed.find(mainLane);
  const auto compensationState = lanePressed.find(compensateLane);
  if ((mainState == lanePressed.end() || mainState->second) &&
      (compensateLane == mainLane ||
       compensationState == lanePressed.end() ||
       compensationState->second)) {
    return result;
  }

  const long long eventTime = inputTimeMicros(context);
  result.keySoundNote =
      selectFallbackKeysound(mainLane, compensateLane, eventTime);
  if (mainState != lanePressed.end()) {
    mainState->second = true;
  }
  if (renderer != nullptr) {
    renderer->onLanePressed(mainLane, JudgeResult(None, 0),
                            context.laneBeamTimeMicros);
  }
  setReplayEvent(result, ReplayEventAction::Press, mainLane, nullptr,
                 eventTime, eventTime, JudgeResult(None, 0));
  return result;
}

RhythmLaneInputController::Result
RhythmLaneInputController::releaseLaneForPreparation(
    int lane, const InputContext &context) {
  Result result;
  if (chart == nullptr ||
      (allowedNoteRange.has_value() &&
       context.songTimeMicros >= allowedNoteRange->endMicros)) {
    return result;
  }
  auto laneState = lanePressed.find(lane);
  if (laneState == lanePressed.end() || !laneState->second) {
    return result;
  }
  laneState->second = false;
  if (renderer != nullptr) {
    renderer->onLaneReleased(lane, context.laneBeamTimeMicros);
  }
  const long long eventTime = inputTimeMicros(context);
  setReplayEvent(result, ReplayEventAction::Release, lane, nullptr,
                 eventTime, eventTime, JudgeResult(None, 0));
  return result;
}
```

- [ ] **Step 6: Route worker input through preparation methods without advancing**

Replace `RealtimeGameplayWorker::processInput()` in `src/scene/play/RealtimeGameplayWorker.cpp` with:

```cpp
void RealtimeGameplayWorker::processInput(
    const RealtimeGameplayInput &input) {
  if (input.epoch != config_.epoch || config_.clock.mapSteadyToSong == nullptr) {
    return;
  }
  const auto songTime = config_.clock.mapSteadyToSong(
      config_.clock.context, input.steadyTimestampMicros);
  if (!songTime.has_value()) {
    latchFault(RealtimeGameplayFault::ClockUnavailable);
    return;
  }
  const bool preparationInput =
      config_.activationSongTimeMicros.has_value() &&
      *songTime < *config_.activationSongTimeMicros;
  const GameplayInputContext context{
      .songTimeMicros = *songTime,
      .laneBeamTimeMicros = input.steadyTimestampMicros,
      .inputDelayMicros = input.inputDelayMicros,
  };

  if (!preparationInput) {
    simulation_.advanceTo(*songTime, input.steadyTimestampMicros);
    if (simulation_.terminal()) {
      return;
    }
  }

  if (input.type == RealtimeGameplayInputType::Release) {
    recordTransaction(preparationInput
                          ? simulation_.releaseLaneForPreparation(
                                input.lane, context)
                          : simulation_.releaseLane(input.lane, context,
                                                    input.backSpin));
    return;
  }

  const int compensateLane =
      input.compensateLane >= 0 ? input.compensateLane : input.lane;
  const NoteId preview =
      preparationInput
          ? simulation_.previewPreparationPressSoundNote(
                input.lane, compensateLane, context)
          : simulation_.previewPressSoundNote(input.lane, compensateLane,
                                              context);
  const bool requiresSound =
      config_.inputTriggeredKeysounds && preview != kInvalidNoteId &&
      definition_.note(preview).wav != bms_parser::Parser::NoWav;
  RealtimeGameplayAudioReservation reservation;
  if (requiresSound &&
      (config_.audio.reserve == nullptr ||
       !config_.audio.reserve(config_.audio.context, preview, reservation))) {
    latchFault(RealtimeGameplayFault::AudioCapacityUnavailable);
    return;
  }

  recordTransaction(preparationInput
                        ? simulation_.pressLaneForPreparation(
                              input.lane, compensateLane, context)
                        : simulation_.pressLane(input.lane, compensateLane,
                                                context));
  if (!requiresSound) {
    return;
  }
  if (latestTransaction_.soundNoteId != preview) {
    if (reservation.requiresCommit && config_.audio.cancel != nullptr) {
      config_.audio.cancel(config_.audio.context, reservation, preview);
    }
    latchFault(RealtimeGameplayFault::InternalConsistency);
    return;
  }
  if (config_.audio.commit == nullptr) {
    if (reservation.requiresCommit && config_.audio.cancel != nullptr) {
      config_.audio.cancel(config_.audio.context, reservation, preview);
    }
    latchFault(RealtimeGameplayFault::AudioCommitFailed);
    return;
  }
  if (!config_.audio.commit(config_.audio.context, reservation, preview)) {
    latchFault(RealtimeGameplayFault::AudioCommitFailed);
  }
}
```

Leave the existing activation check in `advanceAutomatic()` unchanged. It is the independent guarantee that autoplay, misses, landmines, and gauge deadlines cannot advance during the normal indicator.

- [ ] **Step 7: Route the legacy scene preparation branch through controller results**

In `GamePlayScene::pressLane(int, int, double)`, construct `inputContext` before checking `preparationInputUsesVisualOnlyPath()`, store that boolean, and replace the direct lane mutation branch plus ordinary controller call with:

```cpp
  const RhythmLaneInputController::InputContext inputContext{
      .songTimeMicros = getGameplayTimeMicros(rawSongTimeMicros),
      .laneBeamTimeMicros = nowMicros(),
      .inputDelay = inputDelay,
      .notePriorityMode = context.settings.notePriorityMode,
  };
  const bool preparationInput =
      gameplay::preparationInputUsesVisualOnlyPath(
          preparationIndicatorActive(rawSongTimeMicros),
          options.practiceSession != nullptr);
  auto result =
      preparationInput
          ? laneInputController->pressLaneForPreparation(
                mainLane, compensateLane, inputContext)
          : laneInputController->pressLane(mainLane, compensateLane,
                                           inputContext);
  updateLaneStateText();
  if (result.keySoundNote != nullptr &&
      result.keySoundNote->Wav != bms_parser::Parser::NoWav &&
      !options.autoKeySound && !isReplayPlayback()) {
    context.jukebox.playKeySound(result.keySoundNote->Wav);
  }
  if (result.hasJudge) {
    onJudge(result.judge, !options.autoPlay || isReplayPlayback());
  }
  if (result.hasReplayEvent) {
    const auto &event = result.replayEvent;
    if (preparationInput) {
      recordPreparationLaneEvent(event.action, event.lane,
                                 event.songTimeMicros);
    } else {
      appendReplayEvent(event.action, event.lane, event.note,
                        event.songTimeMicros, event.judgeTimeMicros,
                        event.judge);
    }
  }
  return result.note;
```

In `GamePlayScene::releaseLane(int, double, bool)`, make the equivalent replacement:

```cpp
  const RhythmLaneInputController::InputContext inputContext{
      .songTimeMicros = getGameplayTimeMicros(rawSongTimeMicros),
      .laneBeamTimeMicros = nowMicros(),
      .inputDelay = inputDelay,
      .notePriorityMode = context.settings.notePriorityMode,
  };
  const bool preparationInput =
      gameplay::preparationInputUsesVisualOnlyPath(
          preparationIndicatorActive(rawSongTimeMicros),
          options.practiceSession != nullptr);
  auto result =
      preparationInput
          ? laneInputController->releaseLaneForPreparation(lane, inputContext)
          : laneInputController->releaseLane(lane, inputContext, isBackSpin);
  updateLaneStateText();
  if (result.hasJudge) {
    onJudge(result.judge, !options.autoPlay || isReplayPlayback());
  }
  if (result.hasReplayEvent) {
    const auto &event = result.replayEvent;
    if (preparationInput) {
      recordPreparationLaneEvent(event.action, event.lane,
                                 event.songTimeMicros);
    } else {
      appendReplayEvent(event.action, event.lane, event.note,
                        event.songTimeMicros, event.judgeTimeMicros,
                        event.judge);
    }
  }
  return result.note;
```

Keeping `recordPreparationLaneEvent()` for this branch preserves its existing recorded-replay-only policy rather than broadening preparation events into analytics capture.

- [ ] **Step 8: Run the focused suites to verify GREEN**

Run:

```bash
cmake --build cmake-build-debug \
  --target gameplay_simulation_tests \
           gameplay_practice_input_boundary_tests \
           realtime_gameplay_worker_tests \
  -j 6
./cmake-build-debug/gameplay_simulation_tests
./cmake-build-debug/gameplay_practice_input_boundary_tests
./cmake-build-debug/realtime_gameplay_worker_tests
```

Expected: all three executables exit with status 0. Preparation commits sound and ordered lane visuals, keeps note/gauge/deadline state unchanged, rejects held duplicate input, and preserves count-in judgement for session-backed practice.

- [ ] **Step 9: Commit Task 3**

```bash
git add src/scene/play/GameplaySimulation.h \
        src/scene/play/GameplaySimulation.cpp \
        src/scene/play/RhythmLaneInputController.h \
        src/scene/play/RhythmLaneInputController.cpp \
        src/scene/play/RealtimeGameplayWorker.cpp \
        src/scene/play/GamePlayScene.cpp \
        tests/gameplay_simulation_tests.cpp \
        tests/realtime_gameplay_worker_tests.cpp
git commit -m "fix: enable preparation lane feedback and keysounds"
```

---

## Three-Task Review and Verification Checkpoint

- [ ] **Step 1: Review the combined three-task delta**

Run:

```bash
git diff --check HEAD~3..HEAD
git diff --stat HEAD~3..HEAD
git diff HEAD~3..HEAD -- \
  src/scene/play/ManualKeysoundSelection.h \
  src/scene/play/GameplayDefinition.h \
  src/scene/play/GameplayDefinition.cpp \
  src/scene/play/GameplaySimulation.h \
  src/scene/play/GameplaySimulation.cpp \
  src/scene/play/RhythmLaneInputController.h \
  src/scene/play/RhythmLaneInputController.cpp \
  src/scene/play/RealtimeGameplayWorker.cpp \
  src/scene/play/GamePlayScene.cpp \
  tests/gameplay_simulation_tests.cpp \
  tests/gameplay_practice_input_boundary_tests.cpp \
  tests/realtime_gameplay_worker_tests.cpp
```

Review specifically for: any per-press vector/map insertion, any fallback filtering by `IsPlayed`/`IsDead` or `NoteRuntimeState`, tails entering either keysound index, inconsistent main-lane ties, `NoWav` skipping, automatic advancement before activation, state mutation before audio reservation, and preparation calls reaching judge/gauge methods.

- [ ] **Step 2: Run all configured tests**

```bash
ctest --test-dir cmake-build-debug --output-on-failure
```

Expected: every configured test passes.

- [ ] **Step 3: Build the desktop application**

```bash
cmake --build cmake-build-debug --target main -j 6
```

Expected: target `main` builds successfully.

- [ ] **Step 4: Run the non-deploying iOS compile check**

```bash
scripts/ios_firebase_deploy.sh --build-only --skip-init
```

Expected: Xcode ends with `** BUILD SUCCEEDED **`; no Firebase upload occurs.

- [ ] **Step 5: Commit only if review or verification required corrections**

If the checkpoint changed files, stage only those corrections and commit them:

```bash
git add src/scene/play tests CMakeLists.txt
git commit -m "fix: close preparation keysound review findings"
```

If no file changed, do not create an empty commit.
