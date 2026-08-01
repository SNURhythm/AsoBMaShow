# File Replay Contracts and Characterization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an executable contract matrix and pure replay invariants on top of `develop`, while characterizing exactly which schema-v10 header facts can survive as legacy summaries without activating file-based replay persistence.

**Architecture:** Introduce a small `src/replay` contract layer whose capability policy, resource limits, canonical setup model, and identity checks have no dependency on result persistence, provenance, IR, repositories, scenes, or files. Keep the existing schema and runtime untouched; a schema-v10 characterization test records the legacy header/detail boundary that the final migration must honor.

**Tech Stack:** C++23, CMake/CTest, existing manual `expect`/`assert` test executables, SQLite test helpers already compiled into `replay_repository_tests`.

## Global Constraints

- Work only on `feature/file-based-replays-v2`, rooted at `develop` commit `5935b7847ff197dc66ea43e8fecb86ff426d3d4c`.
- This plan implements delivery Slice 1 only. Do not add a BRD codec, replay path layout, gzip/JSON handling, file I/O, file references, modern result tables, legacy summary tables, schema-version changes, or runtime call-site integration.
- Do not cherry-pick implementation commits from PR #82. Manually reuse a reviewed idea only after a failing contract test in this plan requires it.
- Existing `ReplayData`, `ReplayRepository`, result persistence, result recall, IR, profile archive, gameplay, and UI behavior must remain unchanged in this slice.
- Legacy rows remain schema-v10 replay rows during this slice. Production code must not read, convert, delete, or reconstruct any legacy event, touch, or lane-cover row.
- The eventual cutover keeps legacy records as summary-only history: Records list yes; detailed View Result, Watch, Retry Same, G-Battle, practice ghost, video export, replay sharing, and new postponed IR reconstruction no.
- Replay setup code must not include or name `ReplayData`, `ScoreProvenance`, result persistence models, IR models, repository models, SQLite, database IDs, result fingerprints, final score facts, or delivery state.
- Result and IR availability must not depend on replay file state. Replay-dependent actions require a verified replay, except that an existing invalid replay remains deletable.
- Signed replay song time accepts an inclusive 30-second pre-roll: `-30'000'000` microseconds is valid and any smaller timestamp is invalid.
- Default limits are: 64 MiB compressed file, 256 MiB expanded JSON, 9,000,000 key-input bytes, 1,000,000 input transitions, 1,000,000 touch samples, 100,000 lane-cover events, 100,000 RANDOM values, 256 course stages, 1 hour course rest, JSON depth 64, replay string 16 KiB, and filename 255 bytes.
- Captured/Aso-extension setup requires canonical lowercase MD5 and SHA-256. Stock Beatoraja setup may omit MD5 but still requires canonical lowercase SHA-256.
- The replay setup contract accepts the demonstrated Beatoraja key modes `{5, 7, 9, 10, 14, 24, 48}`. The initial required matrix directly exercises 5K, 7K, 9K, 10K, and 14K; 24K/48K remain contract-covered because the retained PR evidence already demonstrated their stock key mapping.
- Result/shared-fact agreement, replay-file ownership transitions, and carried course score/gauge transitions are outside this slice because their owning modern-result, file-store, and continuation models do not exist on `develop`. Their respective delivery slices must begin with dedicated contract plans and may consume the types produced here without copying their rules.
- Every production change starts with a failing test, every task ends with its focused tests passing, and every task receives its own commit.
- Request automated review only once after the complete slice passes its gate, not after individual tasks.

---

## File Map

| File | Responsibility |
| --- | --- |
| `docs/replay/file-replay-contract-matrix.md` | Human-readable authority and capability baseline for all record origins and replay states |
| `src/replay/ReplayCapabilities.h` | Pure origin/state/capability vocabulary |
| `src/replay/ReplayCapabilities.cpp` | The only capability-policy implementation |
| `src/replay/ReplayLimits.h` | Shared resource/time bounds and constexpr boundary predicates |
| `src/replay/ReplaySetup.h` | Canonical setup, chart identity, validation result, and identity-agreement interfaces |
| `src/replay/ReplaySetup.cpp` | Structural setup validation and chart-identity agreement |
| `src/replay/ReplayPlayback.h` | Raw logical input, touch, lane-cover, and course playback value types plus validation interface |
| `src/replay/ReplayPlayback.cpp` | Shared producer/codec validation for playback collections, time, controls, scratch ownership, and course envelopes |
| `tests/replay_capabilities_tests.cpp` | Executable record-origin × replay-state consumer matrix |
| `tests/replay_limits_tests.cpp` | Exact boundary and producer/consumer closure tests for shared limits |
| `tests/replay_setup_tests.cpp` | Setup-axis validation and identity authority tests |
| `tests/replay_playback_tests.cpp` | Pre-roll, ordering, scratch, touch, lane-cover, count, and course-envelope closure tests |
| `tests/replay_repository_tests.cpp` | Schema-v10 header/detail characterization using existing SQLite helpers |
| `tests/replay_contract_boundary_tests.cpp` | Compile-time and source-level guard against replay/result/IR coupling |
| `src/CMakeLists.txt` | Compile pure contract `.cpp` files into `main` without using them at runtime |
| `CMakeLists.txt` | Build and register the five focused contract test executables |

---

### Task 1: Record Origin and Replay Capability Matrix

**Files:**
- Create: `docs/replay/file-replay-contract-matrix.md`
- Create: `src/replay/ReplayCapabilities.h`
- Create: `src/replay/ReplayCapabilities.cpp`
- Create: `tests/replay_capabilities_tests.cpp`
- Modify: `src/CMakeLists.txt:13`
- Modify: `CMakeLists.txt:704-744`
- Modify: `CMakeLists.txt:2808-2841`

**Interfaces:**
- Consumes: no replay, result, repository, IR, scene, or filesystem types.
- Produces: `replay::RecordOrigin`, `replay::ReplayState`, `replay::ReplayCapabilityInput`, `replay::ReplayCapabilities`, and `replay::capabilitiesFor(ReplayCapabilityInput) noexcept`.

- [ ] **Step 1: Verify the existing Records projection baseline before adding the new policy**

Run:

```bash
cmake --build cmake-build-debug --target result_record_summary_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^result_record_summary_tests$'
```

Expected: the existing target builds and passes. This confirms that Slice 1 starts without changing current runtime capability behavior.

- [ ] **Step 2: Write the failing capability-matrix test**

Create `tests/replay_capabilities_tests.cpp` with a table that checks every origin, every replay-state class, chart/course differences, invalid-file deletion, and IR independence:

```cpp
#include "replay/ReplayCapabilities.h"

#include <array>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

using replay::RecordOrigin;
using replay::ReplayCapabilities;
using replay::ReplayCapabilityInput;
using replay::ReplayState;

ReplayCapabilities modernRecordOnly(bool irEligible) {
  return {
      .recordsList = true,
      .viewResult = true,
      .irUpload = irEligible,
      .profileDuplicateRecord = true,
      .profileArchiveRecord = true,
  };
}

void testVerifiedModernMatrix() {
  ReplayCapabilities chart = modernRecordOnly(true);
  chart.watch = true;
  chart.retrySame = true;
  chart.gBattle = true;
  chart.practiceGhost = true;
  chart.videoExport = true;
  chart.shareOrCopy = true;
  chart.deleteReplayFile = true;
  chart.profileDuplicateReplay = true;
  chart.profileArchiveReplay = true;

  ReplayCapabilities course = chart;
  course.gBattle = false;
  course.practiceGhost = false;

  expect(replay::capabilitiesFor({
             .origin = RecordOrigin::ModernChartResult,
             .replayState = ReplayState::Verified,
             .postponedIrSnapshotEligible = true,
         }) == chart,
         "verified modern chart exposes chart replay actions");
  expect(replay::capabilitiesFor({
             .origin = RecordOrigin::ModernCourseResult,
             .replayState = ReplayState::Verified,
             .postponedIrSnapshotEligible = true,
         }) == course,
         "verified modern course excludes chart-only actions");
}

void testAbsentReplayKeepsModernResultAndIr() {
  constexpr std::array states{
      ReplayState::NotApplicable,
      ReplayState::UserDeleted,
      ReplayState::Missing,
  };
  for (ReplayState state : states) {
    expect(replay::capabilitiesFor({
               .origin = RecordOrigin::ModernChartResult,
               .replayState = state,
               .postponedIrSnapshotEligible = true,
           }) == modernRecordOnly(true),
           "absent replay changes no modern result or IR capability");
  }
}

void testInvalidReplayIsOnlyDeletable() {
  constexpr std::array states{
      ReplayState::Corrupt,
      ReplayState::Mismatched,
      ReplayState::UnsupportedExtension,
  };
  for (ReplayState state : states) {
    ReplayCapabilities expected = modernRecordOnly(false);
    expected.deleteReplayFile = true;
    expect(replay::capabilitiesFor({
               .origin = RecordOrigin::ModernChartResult,
               .replayState = state,
           }) == expected,
           "existing invalid replay is deletable but not playable");
  }
}

void testIrEligibilityNeverComesFromReplayState() {
  constexpr std::array states{
      ReplayState::NotApplicable,
      ReplayState::Verified,
      ReplayState::UserDeleted,
      ReplayState::Missing,
      ReplayState::Corrupt,
      ReplayState::Mismatched,
      ReplayState::UnsupportedExtension,
  };
  for (ReplayState state : states) {
    const auto eligible = replay::capabilitiesFor({
        .origin = RecordOrigin::ModernCourseResult,
        .replayState = state,
        .postponedIrSnapshotEligible = true,
    });
    const auto ineligible = replay::capabilitiesFor({
        .origin = RecordOrigin::ModernCourseResult,
        .replayState = state,
        .postponedIrSnapshotEligible = false,
    });
    expect(eligible.irUpload && !ineligible.irUpload,
           "IR capability is controlled only by the saved snapshot");
  }
}

void testLegacyAndRemoteRecordsIgnoreReplayState() {
  ReplayCapabilities legacy;
  legacy.recordsList = true;
  legacy.profileDuplicateRecord = true;
  legacy.profileArchiveRecord = true;

  ReplayCapabilities remote = legacy;
  remote.viewResult = true;

  constexpr std::array states{
      ReplayState::NotApplicable,
      ReplayState::Verified,
      ReplayState::UserDeleted,
      ReplayState::Missing,
      ReplayState::Corrupt,
      ReplayState::Mismatched,
      ReplayState::UnsupportedExtension,
  };
  for (ReplayState state : states) {
    expect(replay::capabilitiesFor({
               .origin = RecordOrigin::LegacyChartSummary,
               .replayState = state,
               .postponedIrSnapshotEligible = true,
           }) == legacy,
           "legacy chart is summary-only for every replay state");
    expect(replay::capabilitiesFor({
               .origin = RecordOrigin::LegacyCourseSummary,
               .replayState = state,
               .postponedIrSnapshotEligible = true,
           }) == legacy,
           "legacy course is summary-only for every replay state");
    expect(replay::capabilitiesFor({
               .origin = RecordOrigin::ImportedRemoteResult,
               .replayState = state,
               .postponedIrSnapshotEligible = true,
           }) == remote,
           "remote result has detail but no local replay or upload actions");
  }
}

void testImportedStockReplaySurface() {
  ReplayCapabilities verified;
  verified.watch = true;
  verified.shareOrCopy = true;
  verified.deleteReplayFile = true;
  verified.profileDuplicateReplay = true;
  verified.profileArchiveReplay = true;
  expect(replay::capabilitiesFor({
             .origin = RecordOrigin::ImportedStockBrd,
             .replayState = ReplayState::Verified,
             .postponedIrSnapshotEligible = true,
         }) == verified,
         "verified stock BRD is playback/file-only evidence");

  ReplayCapabilities invalid;
  invalid.deleteReplayFile = true;
  expect(replay::capabilitiesFor({
             .origin = RecordOrigin::ImportedStockBrd,
             .replayState = ReplayState::Corrupt,
         }) == invalid,
         "invalid imported stock file remains deletable");
  expect(replay::capabilitiesFor({
             .origin = RecordOrigin::ImportedStockBrd,
             .replayState = ReplayState::Missing,
         }) == ReplayCapabilities{},
         "missing imported stock file has no record capability");
}

void testUnknownEnumValuesFailClosed() {
  expect(replay::capabilitiesFor({
             .origin = static_cast<RecordOrigin>(255),
             .replayState = ReplayState::Verified,
             .postponedIrSnapshotEligible = true,
         }) == ReplayCapabilities{},
         "unknown origin receives no capability");
  expect(replay::capabilitiesFor({
             .origin = RecordOrigin::ModernChartResult,
             .replayState = static_cast<ReplayState>(255),
         }) == modernRecordOnly(false),
         "unknown replay state grants no replay action");
}

} // namespace

int main() {
  testVerifiedModernMatrix();
  testAbsentReplayKeepsModernResultAndIr();
  testInvalidReplayIsOnlyDeletable();
  testIrEligibilityNeverComesFromReplayState();
  testLegacyAndRemoteRecordsIgnoreReplayState();
  testImportedStockReplaySurface();
  testUnknownEnumValuesFailClosed();
  if (failures != 0) {
    std::cerr << failures << " replay capability test(s) failed\n";
    return 1;
  }
  std::cout << "replay capability tests passed\n";
  return 0;
}
```

- [ ] **Step 3: Register the missing target and verify the test fails for the missing interface**

Add this block near the existing replay/result tests in root `CMakeLists.txt`:

```cmake
    add_executable(replay_capabilities_tests
        tests/replay_capabilities_tests.cpp
        src/replay/ReplayCapabilities.cpp
    )
    target_include_directories(replay_capabilities_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(replay_capabilities_tests PRIVATE cxx_std_23)
```

Add `replay_capabilities_tests` to the registered `foreach(test_target IN ITEMS)` list immediately after `replay_record_filters_tests`.

Run:

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target replay_capabilities_tests -j 6
```

Expected: FAIL because `src/replay/ReplayCapabilities.h` and `.cpp` do not exist.

- [ ] **Step 4: Add the pure capability vocabulary**

Create `src/replay/ReplayCapabilities.h`:

```cpp
#pragma once

#include <cstdint>

namespace replay {

enum class RecordOrigin : std::uint8_t {
  ModernChartResult,
  ModernCourseResult,
  LegacyChartSummary,
  LegacyCourseSummary,
  ImportedStockBrd,
  ImportedRemoteResult,
};

enum class ReplayState : std::uint8_t {
  NotApplicable,
  Verified,
  UserDeleted,
  Missing,
  Corrupt,
  Mismatched,
  UnsupportedExtension,
};

struct ReplayCapabilityInput {
  RecordOrigin origin = RecordOrigin::ModernChartResult;
  ReplayState replayState = ReplayState::NotApplicable;
  bool postponedIrSnapshotEligible = false;
};

struct ReplayCapabilities {
  bool recordsList = false;
  bool viewResult = false;
  bool watch = false;
  bool retrySame = false;
  bool gBattle = false;
  bool practiceGhost = false;
  bool videoExport = false;
  bool shareOrCopy = false;
  bool deleteReplayFile = false;
  bool irUpload = false;
  bool profileDuplicateRecord = false;
  bool profileDuplicateReplay = false;
  bool profileArchiveRecord = false;
  bool profileArchiveReplay = false;

  bool operator==(const ReplayCapabilities &) const = default;
};

[[nodiscard]] ReplayCapabilities
capabilitiesFor(ReplayCapabilityInput input) noexcept;

} // namespace replay
```

- [ ] **Step 5: Implement the single capability policy**

Create `src/replay/ReplayCapabilities.cpp`:

```cpp
#include "ReplayCapabilities.h"

namespace replay {
namespace {

bool invalidFileIsPresent(ReplayState state) noexcept {
  return state == ReplayState::Corrupt ||
         state == ReplayState::Mismatched ||
         state == ReplayState::UnsupportedExtension;
}

void addVerifiedOwnedReplayCapabilities(ReplayCapabilities &value,
                                        bool chart) noexcept {
  value.watch = true;
  value.retrySame = true;
  value.gBattle = chart;
  value.practiceGhost = chart;
  value.videoExport = true;
  value.shareOrCopy = true;
  value.deleteReplayFile = true;
  value.profileDuplicateReplay = true;
  value.profileArchiveReplay = true;
}

} // namespace

ReplayCapabilities capabilitiesFor(ReplayCapabilityInput input) noexcept {
  ReplayCapabilities result;
  switch (input.origin) {
  case RecordOrigin::ModernChartResult:
  case RecordOrigin::ModernCourseResult: {
    result.recordsList = true;
    result.viewResult = true;
    result.irUpload = input.postponedIrSnapshotEligible;
    result.profileDuplicateRecord = true;
    result.profileArchiveRecord = true;
    if (input.replayState == ReplayState::Verified) {
      addVerifiedOwnedReplayCapabilities(
          result, input.origin == RecordOrigin::ModernChartResult);
    } else if (invalidFileIsPresent(input.replayState)) {
      result.deleteReplayFile = true;
    }
    return result;
  }
  case RecordOrigin::LegacyChartSummary:
  case RecordOrigin::LegacyCourseSummary:
    result.recordsList = true;
    result.profileDuplicateRecord = true;
    result.profileArchiveRecord = true;
    return result;
  case RecordOrigin::ImportedStockBrd:
    if (input.replayState == ReplayState::Verified) {
      result.watch = true;
      result.shareOrCopy = true;
      result.deleteReplayFile = true;
      result.profileDuplicateReplay = true;
      result.profileArchiveReplay = true;
    } else if (invalidFileIsPresent(input.replayState)) {
      result.deleteReplayFile = true;
    }
    return result;
  case RecordOrigin::ImportedRemoteResult:
    result.recordsList = true;
    result.viewResult = true;
    result.profileDuplicateRecord = true;
    result.profileArchiveRecord = true;
    return result;
  }
  return result;
}

} // namespace replay
```

Append this exact block after the existing application source declaration in
`src/CMakeLists.txt`:

```cmake
target_sources(main PRIVATE replay/ReplayCapabilities.cpp)
```

Do not include its header from an existing runtime file.

- [ ] **Step 6: Document the exact human-readable matrix**

Create `docs/replay/file-replay-contract-matrix.md` with this policy statement and table:

```markdown
# File Replay Contract Matrix

`replay::capabilitiesFor` is the executable authority for this table. UI and
service call sites must consume that policy when their delivery slice is
activated; they must not recreate it with local booleans.

| Origin | Replay state | Records | View Result | Replay actions | Delete | IR | Profile transfer |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Modern chart | verified | yes | yes | Watch, Retry Same, G-Battle, practice ghost, video, share/copy | yes | saved snapshot only | record and verified file |
| Modern course | verified | yes | yes | Watch, Retry Same, video, share/copy | yes | saved snapshot only | record and verified file |
| Modern chart/course | missing or user-deleted | yes | yes | none | no | saved snapshot only | record only |
| Modern chart/course | corrupt, mismatched, or unsupported extension | yes | yes | none | yes | saved snapshot only | record only |
| Legacy chart/course summary | every state | yes | no | none | no | no reconstruction | summary only |
| Imported stock BRD | verified | no | no | Watch and share/copy only | yes | never | verified file only |
| Imported stock BRD | missing or user-deleted | no | no | none | no | never | none |
| Imported stock BRD | corrupt, mismatched, or unsupported extension | no | no | none | yes | never | none |
| Imported remote result | every state | yes | existing remote detail | none | no | never | remote record only |

G-Battle and practice ghost are chart-only. A replay file never grants result
or IR authority. A result or saved IR snapshot never makes an absent replay
playable. Invalid-file deletion means deletion of an existing contained file;
missing and user-deleted states are already absent.
```

- [ ] **Step 7: Run the focused test and commit**

Run:

```bash
cmake --build cmake-build-debug --target replay_capabilities_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^replay_capabilities_tests$'
git diff --check
```

Expected: both targets build, the focused test passes, and `git diff --check` prints nothing.

Commit:

```bash
git add CMakeLists.txt src/CMakeLists.txt src/replay/ReplayCapabilities.h src/replay/ReplayCapabilities.cpp tests/replay_capabilities_tests.cpp docs/replay/file-replay-contract-matrix.md
git commit -m "test: define file replay capability contracts"
```

---

### Task 2: Shared Replay Limits and Time Invariants

**Files:**
- Create: `src/replay/ReplayLimits.h`
- Create: `tests/replay_limits_tests.cpp`
- Modify: `CMakeLists.txt:704-754`
- Modify: `CMakeLists.txt:2808-2842`

**Interfaces:**
- Consumes: standard integer and size types only.
- Produces: `replay::ReplayLimits`, `replay::kReplayLimits`, `replay::ReplayTimeBounds`, `replay::isMonotonicReplayTime`, `replay::validCourseRestMicros`, and `replay::clampCourseRestMicros`.

- [ ] **Step 1: Write exact failing boundary tests**

Create `tests/replay_limits_tests.cpp`:

```cpp
#include "replay/ReplayLimits.h"

#include <array>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testDefaultResourceLimitsArePinned() {
  constexpr auto limits = replay::kReplayLimits;
  expect(limits.valid(), "default replay limits are internally consistent");
  expect(limits.minimumSongTimeMicros == -30'000'000,
         "pre-roll is exactly thirty seconds");
  expect(limits.maxCompressedBytes == 64U * 1024U * 1024U,
         "compressed file bound is 64 MiB");
  expect(limits.maxJsonBytes == 256U * 1024U * 1024U,
         "expanded JSON bound is 256 MiB");
  expect(limits.maxKeyInputBytes == 9U * 1'000'000U,
         "key-input byte bound holds one million records");
  expect(limits.maxInputTransitions == 1'000'000U &&
             limits.maxTouchSamples == 1'000'000U &&
             limits.maxLaneCoverEvents == 100'000U &&
             limits.maxRandomValues == 100'000U,
         "raw replay collection bounds are pinned");
  expect(limits.maxCourseStages == 256U &&
             limits.maxCourseRestMicros == 3'600'000'000LL,
         "course stage/rest bounds are pinned");
  expect(limits.maxJsonDepth == 64U &&
             limits.maxStringBytes == 16U * 1024U &&
             limits.maxFilenameBytes == 255U,
         "JSON, string, and filename bounds are pinned");
}

void testSignedSongTimeUsesInclusiveAttemptBounds() {
  constexpr replay::ReplayTimeBounds bounds{
      .completionSongTimeMicros = 5'000'000,
  };
  expect(bounds.valid(), "nonnegative completion time is valid");
  expect(bounds.contains(-30'000'000),
         "exact thirty-second pre-roll is accepted");
  expect(!bounds.contains(-30'000'001),
         "timestamp before pre-roll is rejected");
  expect(bounds.contains(5'000'000),
         "exact completion timestamp is accepted");
  expect(!bounds.contains(5'000'001),
         "timestamp after completion is rejected");

  constexpr replay::ReplayTimeBounds invalid{
      .completionSongTimeMicros = -1,
  };
  expect(!invalid.valid() && !invalid.contains(-1),
         "negative completion boundary rejects every timestamp");
}

void testOrderingIsMonotonicAndNeverSortedIntoValidity() {
  constexpr replay::ReplayTimeBounds bounds{
      .completionSongTimeMicros = 10'000'000,
  };
  expect(replay::isMonotonicReplayTime(-1'000'000, -1'000'000, bounds),
         "equal adjacent timestamps are valid");
  expect(replay::isMonotonicReplayTime(-1'000'000, 0, bounds),
         "increasing adjacent timestamps are valid");
  expect(!replay::isMonotonicReplayTime(1, 0, bounds),
         "decreasing adjacent timestamps are rejected");
  expect(!replay::isMonotonicReplayTime(0, 10'000'001, bounds),
         "ordered timestamp still respects completion");
}

void testCourseRestUsesOnePredicateAndClamp() {
  expect(replay::validCourseRestMicros(0), "zero rest is valid");
  expect(replay::validCourseRestMicros(3'600'000'000LL),
         "exact one-hour rest is valid");
  expect(!replay::validCourseRestMicros(-1), "negative rest is invalid");
  expect(!replay::validCourseRestMicros(3'600'000'001LL),
         "rest above one hour is invalid");
  expect(replay::clampCourseRestMicros(-1) == 0,
         "producer clamp floors negative rest");
  expect(replay::clampCourseRestMicros(3'600'000'001LL) ==
             3'600'000'000LL,
         "producer clamp caps oversized rest");
}

void testCountsUseInclusiveUpperBounds() {
  constexpr auto limits = replay::kReplayLimits;
  constexpr std::array maxima{
      limits.maxInputTransitions,
      limits.maxTouchSamples,
      limits.maxLaneCoverEvents,
      limits.maxRandomValues,
      limits.maxCourseStages,
  };
  for (std::size_t maximum : maxima) {
    expect(replay::withinReplayCountLimit(maximum, maximum),
           "exact collection maximum is accepted");
    expect(!replay::withinReplayCountLimit(maximum + 1, maximum),
           "collection count above maximum is rejected");
  }
}

void testMalformedCustomLimitSetsFailClosed() {
  replay::ReplayLimits zero = replay::kReplayLimits;
  zero.maxInputTransitions = 0;
  expect(!zero.valid(), "zero collection limit is invalid");

  replay::ReplayLimits inverted = replay::kReplayLimits;
  inverted.maxCompressedBytes = inverted.maxJsonBytes + 1;
  expect(!inverted.valid(), "compressed bound cannot exceed JSON bound");

  replay::ReplayLimits positivePreRoll = replay::kReplayLimits;
  positivePreRoll.minimumSongTimeMicros = 1;
  expect(!positivePreRoll.valid(), "minimum song time cannot be positive");
}

} // namespace

int main() {
  testDefaultResourceLimitsArePinned();
  testSignedSongTimeUsesInclusiveAttemptBounds();
  testOrderingIsMonotonicAndNeverSortedIntoValidity();
  testCourseRestUsesOnePredicateAndClamp();
  testCountsUseInclusiveUpperBounds();
  testMalformedCustomLimitSetsFailClosed();
  if (failures != 0) {
    std::cerr << failures << " replay limit test(s) failed\n";
    return 1;
  }
  std::cout << "replay limit tests passed\n";
  return 0;
}
```

- [ ] **Step 2: Register the test and verify the header is missing**

Add:

```cmake
    add_executable(replay_limits_tests
        tests/replay_limits_tests.cpp
    )
    target_include_directories(replay_limits_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(replay_limits_tests PRIVATE cxx_std_23)
```

Add `replay_limits_tests` to the test registration list immediately after `replay_capabilities_tests`.

Run:

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target replay_limits_tests -j 6
```

Expected: FAIL because `replay/ReplayLimits.h` does not exist.

- [ ] **Step 3: Implement the complete shared limit value object and predicates**

Create `src/replay/ReplayLimits.h`:

```cpp
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace replay {

struct ReplayLimits {
  std::size_t maxCompressedBytes = 64U * 1024U * 1024U;
  std::size_t maxJsonBytes = 256U * 1024U * 1024U;
  std::size_t maxKeyInputBytes = 9U * 1'000'000U;
  std::size_t maxInputTransitions = 1'000'000U;
  std::size_t maxTouchSamples = 1'000'000U;
  std::size_t maxLaneCoverEvents = 100'000U;
  std::size_t maxRandomValues = 100'000U;
  std::size_t maxCourseStages = 256U;
  std::size_t maxJsonDepth = 64U;
  std::size_t maxStringBytes = 16U * 1024U;
  std::size_t maxFilenameBytes = 255U;
  std::int64_t minimumSongTimeMicros = -30'000'000LL;
  std::int64_t maxCourseRestMicros = 60LL * 60LL * 1'000'000LL;

  [[nodiscard]] constexpr bool valid() const noexcept {
    return maxCompressedBytes > 0 && maxJsonBytes > 0 &&
           maxCompressedBytes <= maxJsonBytes && maxKeyInputBytes > 0 &&
           maxKeyInputBytes <= maxJsonBytes && maxInputTransitions > 0 &&
           maxTouchSamples > 0 && maxLaneCoverEvents > 0 &&
           maxRandomValues > 0 && maxCourseStages > 0 && maxJsonDepth > 0 &&
           maxStringBytes > 0 && maxFilenameBytes > 0 &&
           minimumSongTimeMicros <= 0 && maxCourseRestMicros >= 0;
  }

  bool operator==(const ReplayLimits &) const = default;
};

inline constexpr ReplayLimits kReplayLimits{};
static_assert(kReplayLimits.valid());

[[nodiscard]] constexpr bool
withinReplayCountLimit(std::size_t count, std::size_t maximum) noexcept {
  return count <= maximum;
}

struct ReplayTimeBounds {
  std::int64_t completionSongTimeMicros = -1;

  [[nodiscard]] constexpr bool valid() const noexcept {
    return completionSongTimeMicros >= 0;
  }

  [[nodiscard]] constexpr bool
  contains(std::int64_t songTimeMicros,
           const ReplayLimits &limits = kReplayLimits) const noexcept {
    return limits.valid() && valid() &&
           songTimeMicros >= limits.minimumSongTimeMicros &&
           songTimeMicros <= completionSongTimeMicros;
  }
};

[[nodiscard]] constexpr bool isMonotonicReplayTime(
    std::int64_t previousSongTimeMicros, std::int64_t nextSongTimeMicros,
    ReplayTimeBounds bounds,
    const ReplayLimits &limits = kReplayLimits) noexcept {
  return nextSongTimeMicros >= previousSongTimeMicros &&
         bounds.contains(nextSongTimeMicros, limits);
}

[[nodiscard]] constexpr bool validCourseRestMicros(
    std::int64_t restMicros,
    const ReplayLimits &limits = kReplayLimits) noexcept {
  return limits.valid() && restMicros >= 0 &&
         restMicros <= limits.maxCourseRestMicros;
}

[[nodiscard]] constexpr std::int64_t clampCourseRestMicros(
    std::int64_t restMicros,
    const ReplayLimits &limits = kReplayLimits) noexcept {
  if (!limits.valid()) {
    return 0;
  }
  return std::clamp(restMicros, std::int64_t{0},
                    limits.maxCourseRestMicros);
}

} // namespace replay
```

- [ ] **Step 4: Run the focused test and commit**

Run:

```bash
cmake --build cmake-build-debug --target replay_limits_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^replay_limits_tests$'
git diff --check
```

Expected: PASS and no whitespace errors.

Commit:

```bash
git add CMakeLists.txt src/replay/ReplayLimits.h tests/replay_limits_tests.cpp
git commit -m "test: pin shared replay limits"
```

---

### Task 3: Canonical Replay Setup and Chart Identity Authority

**Files:**
- Create: `src/replay/ReplaySetup.h`
- Create: `src/replay/ReplaySetup.cpp`
- Create: `tests/replay_setup_tests.cpp`
- Modify: `src/CMakeLists.txt:13`
- Modify: `CMakeLists.txt:704-770`
- Modify: `CMakeLists.txt:2808-2844`

**Interfaces:**
- Consumes: `replay::ReplayLimits`, `GaugeType`, `GaugeProfile`, `GaugeAutoShiftMode`, `RulesetDescriptor`, `audio::PlaybackRate`, `gameplay::CandidateSelectionMode`, and the parser-owned `bms_parser::Parser::RandomPrngId` constant in the `.cpp` only.
- Produces: `replay::ReplayChartIdentity`, `replay::ReplayPlayerOption`, `replay::DoublePlayOption`, `replay::ReplaySetup`, `replay::ReplaySetupSource`, `replay::ReplaySetupIssue`, `replay::validateReplaySetup`, `replay::ReplayChartMatch`, and `replay::compareReplayChartIdentity`.

- [ ] **Step 1: Write the failing setup contract test**

Create `tests/replay_setup_tests.cpp` with a valid fixture, a mutation matrix, source-specific MD5 behavior, all supported key modes, DP FLIP, undefined-LN handling, and chart authority:

```cpp
#include "replay/ReplaySetup.h"

#include <array>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

replay::ReplaySetup validSetup() {
  return {
      .chart = {
          .md5 = std::string(32, 'b'),
          .sha256 = std::string(64, 'a'),
          .keyMode = 14,
      },
      .longNoteMode = 2,
      .hasUndefinedLongNotes = true,
      .chartRandomSeed = 42,
      .chartRandomPrng = "std::mt19937_64",
      .chartRandomValues = {7, 3, 11},
      .player1 = {.option = "RANDOM", .seed = 1234},
      .player2 = {.option = "MIRROR", .seed = 5678},
      .doublePlayOption = replay::DoublePlayOption::Flip,
      .assistOption = "DRAG",
      .initialGaugeType = GaugeType::Hard,
      .gaugeProfile = GaugeProfile::Standard,
      .gaugeAutoShift = GaugeAutoShiftMode::BestClear,
      .gaugeAutoShiftLowerBound = GaugeType::Easy,
      .ruleset = RulesetDescriptor::For(GameplayRuleset::Beatoraja),
      .playback = {.percent = 90,
                   .mode = audio::PlaybackMode::TimeStretch},
      .candidateSelection = gameplay::CandidateSelectionMode::Score,
      .judgeWindowScalePercent = 85,
      .startingGaugePercent = 35.0F,
      .initialLaneCoverPercent = 31,
      .laneCoverEnabled = true,
      .clubMode = true,
  };
}

void testValidCapturedAndStockSetups() {
  const auto setup = validSetup();
  expect(replay::validateReplaySetup(
             setup, replay::ReplaySetupSource::LocalCapture).valid(),
         "full local setup is valid");
  expect(replay::validateReplaySetup(
             setup, replay::ReplaySetupSource::AsoExtension).valid(),
         "full Aso extension setup is valid");

  auto stock = setup;
  stock.chart.md5.clear();
  expect(replay::validateReplaySetup(
             stock, replay::ReplaySetupSource::StockBeatoraja).valid(),
         "stock setup may omit MD5");
  expect(replay::validateReplaySetup(
             stock, replay::ReplaySetupSource::LocalCapture).issue ==
             replay::ReplaySetupIssue::ChartMd5,
         "local capture cannot omit MD5");

  stock.ruleset = RulesetDescriptor::Current();
  expect(replay::validateReplaySetup(
             stock, replay::ReplaySetupSource::StockBeatoraja).issue ==
             replay::ReplaySetupIssue::Ruleset,
         "stock setup identifies Beatoraja gameplay authority");
}

void testUnknownSetupSourceFailsClosed() {
  expect(replay::validateReplaySetup(
             validSetup(), static_cast<replay::ReplaySetupSource>(255)).issue ==
             replay::ReplaySetupIssue::Source,
         "unknown setup source cannot inherit local or stock authority");

  replay::ReplayLimits invalidLimits = replay::kReplayLimits;
  invalidLimits.maxStringBytes = 0;
  expect(replay::validateReplaySetup(
             validSetup(), replay::ReplaySetupSource::LocalCapture,
             invalidLimits).issue == replay::ReplaySetupIssue::Limits,
         "invalid limit policy cannot validate setup");
}

void testSupportedKeyModesAndDoublePlayOption() {
  constexpr std::array keyModes{5, 7, 9, 10, 14, 24, 48};
  for (int keyMode : keyModes) {
    auto setup = validSetup();
    setup.chart.keyMode = keyMode;
    setup.doublePlayOption = replay::DoublePlayOption::Normal;
    expect(replay::validateReplaySetup(
               setup, replay::ReplaySetupSource::LocalCapture).valid(),
           "demonstrated Beatoraja key mode is accepted");
  }

  auto invalid = validSetup();
  invalid.chart.keyMode = 8;
  invalid.doublePlayOption = replay::DoublePlayOption::Normal;
  expect(replay::validateReplaySetup(
             invalid, replay::ReplaySetupSource::LocalCapture).issue ==
             replay::ReplaySetupIssue::KeyMode,
         "unsupported key mode fails closed");

  auto single = validSetup();
  single.chart.keyMode = 7;
  expect(replay::validateReplaySetup(
             single, replay::ReplaySetupSource::LocalCapture).issue ==
             replay::ReplaySetupIssue::DoublePlayOption,
         "FLIP requires a supported double-play key mode");
}

void testUndefinedLongNoteContract() {
  auto noLongNotes = validSetup();
  noLongNotes.longNoteMode = 0;
  noLongNotes.hasUndefinedLongNotes = false;
  expect(replay::validateReplaySetup(
             noLongNotes, replay::ReplaySetupSource::LocalCapture).valid(),
         "mode zero is valid when no undefined long notes need interpretation");

  noLongNotes.hasUndefinedLongNotes = true;
  expect(replay::validateReplaySetup(
             noLongNotes, replay::ReplaySetupSource::LocalCapture).issue ==
             replay::ReplaySetupIssue::LongNoteMode,
         "undefined long notes require an effective LN/CN/HCN mode");
}

void testInvalidFieldMatrix() {
  struct Case {
    replay::ReplaySetupIssue issue;
    std::function<void(replay::ReplaySetup &)> mutate;
  };
  const std::vector<Case> cases{
      {replay::ReplaySetupIssue::ChartSha256,
       [](auto &v) { v.chart.sha256[0] = 'A'; }},
      {replay::ReplaySetupIssue::ChartMd5,
       [](auto &v) { v.chart.md5.pop_back(); }},
      {replay::ReplaySetupIssue::LongNoteMode,
       [](auto &v) { v.longNoteMode = 4; }},
      {replay::ReplaySetupIssue::RandomValues,
       [](auto &v) { v.chartRandomValues.resize(100'001); }},
      {replay::ReplaySetupIssue::RandomPrng,
       [](auto &v) { v.chartRandomPrng = "unknown"; }},
      {replay::ReplaySetupIssue::PlayerOneOption,
       [](auto &v) { v.player1.option = "random"; }},
      {replay::ReplaySetupIssue::PlayerTwoOption,
       [](auto &v) { v.player2.seed = -1; }},
      {replay::ReplaySetupIssue::PlayerOptions,
       [](auto &v) {
         v.player1.option = "ASSIGN:L123456789ABCDER";
         v.player2.option = "RANDOM";
       }},
      {replay::ReplaySetupIssue::AssistOption,
       [](auto &v) { v.assistOption = "drag"; }},
      {replay::ReplaySetupIssue::GaugeType,
       [](auto &v) { v.initialGaugeType = static_cast<GaugeType>(99); }},
      {replay::ReplaySetupIssue::GaugeProfile,
       [](auto &v) { v.gaugeProfile = static_cast<GaugeProfile>(99); }},
      {replay::ReplaySetupIssue::GaugeAutoShift,
       [](auto &v) {
         v.gaugeAutoShift = static_cast<GaugeAutoShiftMode>(99);
       }},
      {replay::ReplaySetupIssue::GaugeAutoShiftLowerBound,
       [](auto &v) {
         v.gaugeAutoShiftLowerBound = static_cast<GaugeType>(99);
       }},
      {replay::ReplaySetupIssue::Ruleset,
       [](auto &v) { v.ruleset.id = "future"; }},
      {replay::ReplaySetupIssue::PlaybackRate,
       [](auto &v) { v.playback.percent = 91; }},
      {replay::ReplaySetupIssue::CandidateSelection,
       [](auto &v) {
         v.candidateSelection =
             static_cast<gameplay::CandidateSelectionMode>(99);
       }},
      {replay::ReplaySetupIssue::JudgeWindowScale,
       [](auto &v) { v.judgeWindowScalePercent = 0; }},
      {replay::ReplaySetupIssue::StartingGauge,
       [](auto &v) { v.startingGaugePercent = 101.0F; }},
      {replay::ReplaySetupIssue::InitialLaneCover,
       [](auto &v) { v.initialLaneCoverPercent = 101; }},
  };

  for (const auto &test : cases) {
    auto setup = validSetup();
    test.mutate(setup);
    expect(replay::validateReplaySetup(
               setup, replay::ReplaySetupSource::LocalCapture).issue ==
               test.issue,
           "invalid setup field reports its canonical issue");
  }
}

void testChartIdentityAgreementUsesParsedIdentity() {
  const auto recorded = validSetup().chart;
  expect(replay::compareReplayChartIdentity(recorded, recorded) ==
             replay::ReplayChartMatch::Match,
         "identical parsed and recorded identity matches");

  auto selected = recorded;
  selected.sha256[0] = 'c';
  expect(replay::compareReplayChartIdentity(recorded, selected) ==
             replay::ReplayChartMatch::Sha256Mismatch,
         "SHA-256 mismatch has highest identity priority");

  selected = recorded;
  selected.md5[0] = 'c';
  expect(replay::compareReplayChartIdentity(recorded, selected) ==
             replay::ReplayChartMatch::Md5Mismatch,
         "present recorded MD5 must match parsed content");

  selected = recorded;
  selected.keyMode = 7;
  expect(replay::compareReplayChartIdentity(recorded, selected) ==
             replay::ReplayChartMatch::KeyModeMismatch,
         "selected key mode cannot be overwritten by replay metadata");

  auto stock = recorded;
  stock.md5.clear();
  expect(replay::compareReplayChartIdentity(stock, recorded) ==
             replay::ReplayChartMatch::Match,
         "stock identity without MD5 is matched by SHA-256 and key mode");
}

} // namespace

int main() {
  testValidCapturedAndStockSetups();
  testUnknownSetupSourceFailsClosed();
  testSupportedKeyModesAndDoublePlayOption();
  testUndefinedLongNoteContract();
  testInvalidFieldMatrix();
  testChartIdentityAgreementUsesParsedIdentity();
  if (failures != 0) {
    std::cerr << failures << " replay setup test(s) failed\n";
    return 1;
  }
  std::cout << "replay setup tests passed\n";
  return 0;
}
```

- [ ] **Step 2: Register the target and verify the setup interface is missing**

Add:

```cmake
    add_executable(replay_setup_tests
        tests/replay_setup_tests.cpp
        src/replay/ReplaySetup.cpp
    )
    target_include_directories(replay_setup_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(replay_setup_tests PRIVATE cxx_std_23)
```

Add `replay_setup_tests` to the registration list immediately after `replay_limits_tests`.

Run:

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target replay_setup_tests -j 6
```

Expected: FAIL because `ReplaySetup.h` and `.cpp` do not exist.

- [ ] **Step 3: Add the canonical setup interface**

Create `src/replay/ReplaySetup.h`:

```cpp
#pragma once

#include "ReplayLimits.h"

#include "../AssistOptionUtils.h"
#include "../audio/PlaybackRate.h"
#include "../scene/play/GameplayGaugeTypes.h"
#include "../scene/play/GameplayJudgeRules.h"
#include "../scene/play/GameplayRuleset.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace replay {

struct ReplayChartIdentity {
  std::string md5;
  std::string sha256;
  int keyMode = 0;

  bool operator==(const ReplayChartIdentity &) const = default;
};

struct ReplayPlayerOption {
  std::string option = "NORMAL";
  std::optional<std::int64_t> seed;

  bool operator==(const ReplayPlayerOption &) const = default;
};

enum class DoublePlayOption : std::uint8_t {
  Normal = 0,
  Flip = 1,
};

struct ReplaySetup {
  ReplayChartIdentity chart;
  int longNoteMode = 0;
  bool hasUndefinedLongNotes = false;
  std::optional<std::uint64_t> chartRandomSeed;
  std::optional<std::string> chartRandomPrng;
  std::vector<int> chartRandomValues;
  ReplayPlayerOption player1;
  ReplayPlayerOption player2;
  DoublePlayOption doublePlayOption = DoublePlayOption::Normal;
  std::string assistOption = assist_options::kOff;
  GaugeType initialGaugeType = GaugeType::Normal;
  GaugeProfile gaugeProfile = GaugeProfile::Standard;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  GaugeType gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
  RulesetDescriptor ruleset = RulesetDescriptor::Current();
  audio::PlaybackRate playback;
  gameplay::CandidateSelectionMode candidateSelection =
      gameplay::CandidateSelectionMode::Lowest;
  int judgeWindowScalePercent = 100;
  float startingGaugePercent = 20.0F;
  int initialLaneCoverPercent = 0;
  bool laneCoverEnabled = false;
  bool clubMode = false;

  bool operator==(const ReplaySetup &) const = default;
};

enum class ReplaySetupSource : std::uint8_t {
  LocalCapture,
  AsoExtension,
  StockBeatoraja,
};

enum class ReplaySetupIssue : std::uint8_t {
  None,
  Source,
  Limits,
  ChartSha256,
  ChartMd5,
  KeyMode,
  LongNoteMode,
  RandomValues,
  RandomPrng,
  PlayerOneOption,
  PlayerTwoOption,
  PlayerOptions,
  DoublePlayOption,
  AssistOption,
  GaugeType,
  GaugeProfile,
  GaugeAutoShift,
  GaugeAutoShiftLowerBound,
  Ruleset,
  PlaybackRate,
  CandidateSelection,
  JudgeWindowScale,
  StartingGauge,
  InitialLaneCover,
};

struct ReplaySetupValidation {
  ReplaySetupIssue issue = ReplaySetupIssue::None;

  [[nodiscard]] constexpr bool valid() const noexcept {
    return issue == ReplaySetupIssue::None;
  }
};

[[nodiscard]] ReplaySetupValidation validateReplaySetup(
    const ReplaySetup &setup, ReplaySetupSource source,
    const ReplayLimits &limits = kReplayLimits);

enum class ReplayChartMatch : std::uint8_t {
  Match,
  Sha256Mismatch,
  Md5Mismatch,
  KeyModeMismatch,
};

[[nodiscard]] ReplayChartMatch compareReplayChartIdentity(
    const ReplayChartIdentity &recorded,
    const ReplayChartIdentity &selected) noexcept;

} // namespace replay
```

- [ ] **Step 4: Implement structural validation without result/provenance dependencies**

Create `src/replay/ReplaySetup.cpp`. Use the following exact helper rules and validation order so the mutation matrix has deterministic issues:

```cpp
#include "ReplaySetup.h"

#include "../bms_parser.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>
#include <string_view>

namespace replay {
namespace {

bool canonicalHex(std::string_view value, std::size_t size) noexcept {
  return value.size() == size &&
         std::ranges::all_of(value, [](unsigned char ch) {
           return (ch >= '0' && ch <= '9') ||
                  (ch >= 'a' && ch <= 'f');
         });
}

bool supportedKeyMode(int keyMode) noexcept {
  constexpr std::array modes{5, 7, 9, 10, 14, 24, 48};
  return std::ranges::find(modes, keyMode) != modes.end();
}

bool enumBetween(int value, int first, int last) noexcept {
  return value >= first && value <= last;
}

std::string_view assignmentSymbols(int keyMode) noexcept {
  switch (keyMode) {
  case 5:
    return "S12345";
  case 7:
    return "S1234567";
  case 10:
    return "L123456789AR";
  case 14:
    return "L123456789ABCDER";
  default:
    return {};
  }
}

bool validManualOption(std::string_view option, int keyMode) noexcept {
  constexpr std::string_view prefix = "ASSIGN:";
  if (!option.starts_with(prefix)) {
    return false;
  }
  const std::string_view symbols = assignmentSymbols(keyMode);
  const std::string_view notation = option.substr(prefix.size());
  if (symbols.empty() || notation.size() != symbols.size()) {
    return false;
  }
  for (std::size_t index = 0; index < notation.size(); ++index) {
    if (!symbols.contains(notation[index]) ||
        notation.find(notation[index]) != index) {
      return false;
    }
  }
  return true;
}

bool validStockOption(std::string_view option) noexcept {
  constexpr std::array<std::string_view, 10> options{
      "NORMAL", "MIRROR",   "RANDOM",  "R-RANDOM",  "S-RANDOM",
      "SPIRAL", "H-RANDOM", "ALL-SCR", "RANDOM-EX", "S-RANDOM-EX",
  };
  return std::ranges::find(options, option) != options.end();
}

bool validOption(const ReplayPlayerOption &option, int keyMode,
                 const ReplayLimits &limits) noexcept {
  return option.option.size() <= limits.maxStringBytes &&
         (!option.seed.has_value() || *option.seed >= 0) &&
         (validStockOption(option.option) ||
          validManualOption(option.option, keyMode));
}

bool optionsCompatible(const ReplayPlayerOption &first,
                       const ReplayPlayerOption &second) noexcept {
  const bool firstManual = first.option.starts_with("ASSIGN:");
  const bool secondManual = second.option.starts_with("ASSIGN:");
  if (firstManual && secondManual) {
    return first.option == second.option;
  }
  if (firstManual) {
    return second.option == "NORMAL";
  }
  if (secondManual) {
    return first.option == "NORMAL";
  }
  return true;
}

ReplaySetupValidation invalid(ReplaySetupIssue issue) noexcept {
  return {.issue = issue};
}

} // namespace

ReplaySetupValidation validateReplaySetup(
    const ReplaySetup &setup, ReplaySetupSource source,
    const ReplayLimits &limits) {
  if (source != ReplaySetupSource::LocalCapture &&
      source != ReplaySetupSource::AsoExtension &&
      source != ReplaySetupSource::StockBeatoraja) {
    return invalid(ReplaySetupIssue::Source);
  }
  if (!limits.valid()) {
    return invalid(ReplaySetupIssue::Limits);
  }
  if (!canonicalHex(setup.chart.sha256, 64)) {
    return invalid(ReplaySetupIssue::ChartSha256);
  }
  const bool md5Required = source != ReplaySetupSource::StockBeatoraja;
  if ((md5Required && !canonicalHex(setup.chart.md5, 32)) ||
      (!setup.chart.md5.empty() &&
       !canonicalHex(setup.chart.md5, 32))) {
    return invalid(ReplaySetupIssue::ChartMd5);
  }
  if (!supportedKeyMode(setup.chart.keyMode)) {
    return invalid(ReplaySetupIssue::KeyMode);
  }
  if (setup.longNoteMode < 0 || setup.longNoteMode > 3 ||
      (setup.hasUndefinedLongNotes && setup.longNoteMode == 0)) {
    return invalid(ReplaySetupIssue::LongNoteMode);
  }
  if (setup.chartRandomValues.size() > limits.maxRandomValues) {
    return invalid(ReplaySetupIssue::RandomValues);
  }
  if (setup.chartRandomPrng.has_value() &&
      (*setup.chartRandomPrng != bms_parser::Parser::RandomPrngId ||
       setup.chartRandomPrng->size() > limits.maxStringBytes)) {
    return invalid(ReplaySetupIssue::RandomPrng);
  }
  if (!validOption(setup.player1, setup.chart.keyMode, limits)) {
    return invalid(ReplaySetupIssue::PlayerOneOption);
  }
  if (!validOption(setup.player2, setup.chart.keyMode, limits)) {
    return invalid(ReplaySetupIssue::PlayerTwoOption);
  }
  if (!optionsCompatible(setup.player1, setup.player2)) {
    return invalid(ReplaySetupIssue::PlayerOptions);
  }
  const int doublePlayOption = static_cast<int>(setup.doublePlayOption);
  if (!enumBetween(doublePlayOption,
                   static_cast<int>(DoublePlayOption::Normal),
                   static_cast<int>(DoublePlayOption::Flip)) ||
      (setup.doublePlayOption == DoublePlayOption::Flip &&
       setup.chart.keyMode != 10 && setup.chart.keyMode != 14)) {
    return invalid(ReplaySetupIssue::DoublePlayOption);
  }
  if (setup.assistOption.size() > limits.maxStringBytes ||
      assist_options::normalize(setup.assistOption) != setup.assistOption) {
    return invalid(ReplaySetupIssue::AssistOption);
  }
  if (!enumBetween(static_cast<int>(setup.initialGaugeType),
                   static_cast<int>(GaugeType::AssistedEasy),
                   static_cast<int>(GaugeType::Hazard))) {
    return invalid(ReplaySetupIssue::GaugeType);
  }
  if (!enumBetween(static_cast<int>(setup.gaugeProfile),
                   static_cast<int>(GaugeProfile::Standard),
                   static_cast<int>(GaugeProfile::Standard24Keys))) {
    return invalid(ReplaySetupIssue::GaugeProfile);
  }
  if (!enumBetween(static_cast<int>(setup.gaugeAutoShift),
                   static_cast<int>(GaugeAutoShiftMode::None),
                   static_cast<int>(GaugeAutoShiftMode::BestClear))) {
    return invalid(ReplaySetupIssue::GaugeAutoShift);
  }
  if (!enumBetween(static_cast<int>(setup.gaugeAutoShiftLowerBound),
                   static_cast<int>(GaugeType::AssistedEasy),
                   static_cast<int>(GaugeType::Hazard))) {
    return invalid(ReplaySetupIssue::GaugeAutoShiftLowerBound);
  }
  if (!isSupportedRulesetDescriptor(setup.ruleset) ||
      (source == ReplaySetupSource::StockBeatoraja &&
       setup.ruleset !=
           RulesetDescriptor::For(GameplayRuleset::Beatoraja))) {
    return invalid(ReplaySetupIssue::Ruleset);
  }
  if (!setup.playback.valid()) {
    return invalid(ReplaySetupIssue::PlaybackRate);
  }
  if (!enumBetween(
          static_cast<int>(setup.candidateSelection),
          static_cast<int>(gameplay::CandidateSelectionMode::LR2),
          static_cast<int>(gameplay::CandidateSelectionMode::Score))) {
    return invalid(ReplaySetupIssue::CandidateSelection);
  }
  if (setup.judgeWindowScalePercent <= 0 ||
      setup.judgeWindowScalePercent > 1000) {
    return invalid(ReplaySetupIssue::JudgeWindowScale);
  }
  if (!std::isfinite(setup.startingGaugePercent) ||
      setup.startingGaugePercent < 0.0F ||
      setup.startingGaugePercent > 100.0F) {
    return invalid(ReplaySetupIssue::StartingGauge);
  }
  if (setup.initialLaneCoverPercent < 0 ||
      setup.initialLaneCoverPercent > 100) {
    return invalid(ReplaySetupIssue::InitialLaneCover);
  }
  return {};
}

ReplayChartMatch compareReplayChartIdentity(
    const ReplayChartIdentity &recorded,
    const ReplayChartIdentity &selected) noexcept {
  if (recorded.sha256 != selected.sha256) {
    return ReplayChartMatch::Sha256Mismatch;
  }
  if (!recorded.md5.empty() && recorded.md5 != selected.md5) {
    return ReplayChartMatch::Md5Mismatch;
  }
  if (recorded.keyMode != selected.keyMode) {
    return ReplayChartMatch::KeyModeMismatch;
  }
  return ReplayChartMatch::Match;
}

} // namespace replay
```

Append this exact block after the capability source registration in
`src/CMakeLists.txt`:

```cmake
target_sources(main PRIVATE replay/ReplaySetup.cpp)
```

Do not include `ReplaySetup.h` from an existing runtime file.

- [ ] **Step 5: Run the focused setup suite and commit**

Run:

```bash
cmake --build cmake-build-debug --target replay_setup_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^replay_setup_tests$'
git diff --check
```

Expected: the setup test and desktop application build pass, with no runtime integration.

Commit:

```bash
git add CMakeLists.txt src/CMakeLists.txt src/replay/ReplaySetup.h src/replay/ReplaySetup.cpp tests/replay_setup_tests.cpp
git commit -m "test: define canonical replay setup contract"
```

---

### Task 4: Shared Raw Playback Validation

**Files:**
- Create: `src/replay/ReplayPlayback.h`
- Create: `src/replay/ReplayPlayback.cpp`
- Create: `tests/replay_playback_tests.cpp`
- Modify: `src/CMakeLists.txt:13-15`
- Modify: `CMakeLists.txt:704-786`
- Modify: `CMakeLists.txt:2808-2846`

**Interfaces:**
- Consumes: `replay::ReplaySetup`, `replay::ReplaySetupSource`, `replay::ReplayLimits`, and `replay::ReplayTimeBounds`.
- Produces: raw playback value types, `replay::validateReplayPlayback`, and `replay::validateCourseReplayPlayback`. Recorder, encoder, decoder, Watch, and video paths must use these functions when their delivery slices activate; this slice does not connect those call sites.

- [ ] **Step 1: Write the failing producer/consumer closure tests**

Create `tests/replay_playback_tests.cpp`:

```cpp
#include "replay/ReplayPlayback.h"

#include <array>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

replay::ReplaySetup validSetup(int keyMode = 7) {
  replay::ReplaySetup setup;
  setup.chart.md5 = std::string(32, 'b');
  setup.chart.sha256 = std::string(64, 'a');
  setup.chart.keyMode = keyMode;
  setup.longNoteMode = 1;
  return setup;
}

replay::ReplayPlaybackData validPlayback() {
  replay::ReplayPlaybackData data;
  data.setup = validSetup();
  data.input = {
      {.songTimeMicros = -2'000'000,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = true},
      {.songTimeMicros = 1'000'000,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = false},
  };
  data.touchSamples = {
      {.action = replay::ReplayTouchAction::Down,
       .fingerId = 9,
       .songTimeMicros = -1'800'000,
       .x = 0.25F,
       .y = 0.5F},
      {.action = replay::ReplayTouchAction::Up,
       .fingerId = 9,
       .songTimeMicros = 1'100'000,
       .x = 0.25F,
       .y = 0.5F},
  };
  data.laneCoverEvents = {
      {.songTimeMicros = -2'000'000,
       .noteStartPositionPercent = 20,
       .resetVisibleTimeReference = false},
  };
  return data;
}

constexpr replay::ReplayTimeBounds kBounds{
    .completionSongTimeMicros = 5'000'000,
};

replay::ReplayPlaybackValidation validate(
    const replay::ReplayPlaybackData &data,
    const replay::ReplayLimits &limits = replay::kReplayLimits) {
  return replay::validateReplayPlayback(
      data, replay::ReplaySetupSource::LocalCapture, kBounds, limits);
}

void testPreRollIsSharedByEveryTimedCollection() {
  auto data = validPlayback();
  data.input.front().songTimeMicros = -30'000'000;
  data.touchSamples.front().songTimeMicros = -30'000'000;
  data.laneCoverEvents.front().songTimeMicros = -30'000'000;
  expect(validate(data).valid(),
         "input, touch, and lane cover all accept exact pre-roll");

  data = validPlayback();
  data.input.front().songTimeMicros = -30'000'001;
  expect(validate(data).issue == replay::ReplayPlaybackIssue::InputTime,
         "input before pre-roll is rejected");

  data = validPlayback();
  data.touchSamples.front().songTimeMicros = -30'000'001;
  expect(validate(data).issue == replay::ReplayPlaybackIssue::TouchTime,
         "touch before pre-roll is rejected");

  data = validPlayback();
  data.laneCoverEvents.front().songTimeMicros = -30'000'001;
  expect(validate(data).issue == replay::ReplayPlaybackIssue::LaneCoverTime,
         "lane cover before pre-roll is rejected");
}

void testOrderingIsRejectedInsteadOfSorted() {
  auto data = validPlayback();
  data.input.back().songTimeMicros = -2'000'001;
  expect(validate(data).issue == replay::ReplayPlaybackIssue::InputOrder,
         "decreasing input order is rejected");

  data = validPlayback();
  data.touchSamples.back().songTimeMicros = -1'800'001;
  expect(validate(data).issue == replay::ReplayPlaybackIssue::TouchOrder,
         "decreasing touch order is rejected");

  data = validPlayback();
  data.laneCoverEvents.push_back(data.laneCoverEvents.front());
  data.laneCoverEvents.back().songTimeMicros = -2'000'001;
  expect(validate(data).issue == replay::ReplayPlaybackIssue::LaneCoverOrder,
         "decreasing lane-cover order is rejected");
}

void testControlAndScratchDirectionMatrix() {
  struct LaneCase {
    int keyMode;
    int player;
    int lane;
  };
  constexpr std::array laneCases{
      LaneCase{5, 1, 4},  LaneCase{7, 1, 6},  LaneCase{9, 1, 8},
      LaneCase{10, 2, 4}, LaneCase{14, 2, 6}, LaneCase{24, 1, 25},
      LaneCase{48, 2, 25},
  };
  for (const auto &test : laneCases) {
    auto data = validPlayback();
    data.setup = validSetup(test.keyMode);
    data.input = {{.songTimeMicros = 0,
                   .control = {.kind = replay::LogicalControlKind::Lane,
                               .player = test.player,
                               .lane = test.lane},
                   .pressed = true}};
    expect(validate(data).valid(), "highest supported lane is valid");
    ++data.input.front().control.lane;
    expect(validate(data).issue ==
               replay::ReplayPlaybackIssue::InputControl,
           "lane above supported layout is invalid");
  }

  for (int keyMode : {5, 7, 10, 14}) {
    for (auto kind : {replay::LogicalControlKind::ScratchClockwise,
                      replay::LogicalControlKind::ScratchCounterClockwise}) {
      auto data = validPlayback();
      data.setup = validSetup(keyMode);
      data.input = {{.songTimeMicros = 0,
                     .control = {.kind = kind,
                                 .player = keyMode >= 10 ? 2 : 1,
                                 .lane = -1},
                     .pressed = true}};
      expect(validate(data).valid(),
             "both stock scratch directions are valid");
    }
  }

  auto scratchless = validPlayback();
  scratchless.setup = validSetup(9);
  scratchless.input = {{
      .songTimeMicros = 0,
      .control = {.kind = replay::LogicalControlKind::ScratchClockwise,
                  .player = 1,
                  .lane = -1},
      .pressed = true,
  }};
  expect(validate(scratchless).issue ==
             replay::ReplayPlaybackIssue::InputControl,
         "scratch direction is invalid for scratchless mode");
}

void testRedundancyAndScratchOwnershipHandoff() {
  auto redundant = validPlayback();
  redundant.input.insert(redundant.input.begin() + 1,
                         redundant.input.front());
  expect(validate(redundant).issue ==
             replay::ReplayPlaybackIssue::RedundantInput,
         "duplicate logical state transition is rejected");

  replay::ReplayPlaybackData handoff;
  handoff.setup = validSetup(7);
  handoff.input = {
      {.songTimeMicros = 0,
       .control = {.kind = replay::LogicalControlKind::ScratchClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = true},
      {.songTimeMicros = 1,
       .control = {.kind = replay::LogicalControlKind::ScratchClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = false,
       .replayOnly = true},
      {.songTimeMicros = 1,
       .control = {
           .kind = replay::LogicalControlKind::ScratchCounterClockwise,
           .player = 1,
           .lane = -1},
       .pressed = true,
       .replayOnly = true},
  };
  expect(validate(handoff).valid(),
         "same-timestamp opposite scratch ownership handoff is valid");
  handoff.input.back().songTimeMicros = 2;
  expect(validate(handoff).issue ==
             replay::ReplayPlaybackIssue::ScratchHandoff,
         "delayed replay-only scratch handoff is rejected");
}

void testCollectionCountsAndSupplementalValues() {
  replay::ReplayLimits limits = replay::kReplayLimits;
  limits.maxInputTransitions = 2;
  limits.maxTouchSamples = 2;
  limits.maxLaneCoverEvents = 1;
  expect(validate(validPlayback(), limits).valid(),
         "collections at custom maxima are accepted");

  auto data = validPlayback();
  data.input.push_back({.songTimeMicros = 2'000'000,
                        .control = {
                            .kind = replay::LogicalControlKind::Lane,
                            .player = 1,
                            .lane = 1},
                        .pressed = true});
  expect(validate(data, limits).issue ==
             replay::ReplayPlaybackIssue::InputCount,
         "input count above limit is rejected");

  data = validPlayback();
  data.touchSamples.push_back(data.touchSamples.back());
  data.touchSamples.back().songTimeMicros = 2'000'000;
  expect(validate(data, limits).issue ==
             replay::ReplayPlaybackIssue::TouchCount,
         "touch count above limit is rejected");

  data = validPlayback();
  data.laneCoverEvents.push_back(data.laneCoverEvents.back());
  expect(validate(data, limits).issue ==
             replay::ReplayPlaybackIssue::LaneCoverCount,
         "lane-cover count above limit is rejected");

  data = validPlayback();
  data.touchSamples.front().x = 1.01F;
  expect(validate(data).issue ==
             replay::ReplayPlaybackIssue::TouchCoordinate,
         "touch coordinates must remain normalized");

  data = validPlayback();
  data.laneCoverEvents.front().noteStartPositionPercent = 101;
  expect(validate(data).issue ==
             replay::ReplayPlaybackIssue::LaneCoverPercent,
         "lane-cover percentage is bounded");
}

void testCourseEnvelopeSupportsMixedStageSources() {
  replay::CourseReplayPlaybackData course;
  course.stages = {validPlayback(), validPlayback()};
  course.stages[1].setup.chart.md5.clear();
  course.stages[1].setup.ruleset =
      RulesetDescriptor::For(GameplayRuleset::Beatoraja);
  course.restMicrosAfterStage = {3'600'000'000LL, 0};
  const std::array sources{
      replay::ReplaySetupSource::LocalCapture,
      replay::ReplaySetupSource::StockBeatoraja,
  };
  const std::array bounds{kBounds, kBounds};
  expect(replay::validateCourseReplayPlayback(course, sources, bounds).valid(),
         "course accepts per-stage setup authority and bounded rest");

  course.restMicrosAfterStage[0] = 3'600'000'001LL;
  const auto badRest =
      replay::validateCourseReplayPlayback(course, sources, bounds);
  expect(badRest.issue == replay::ReplayPlaybackIssue::CourseRest &&
             badRest.stageIndex == 0,
         "course reports the stage with oversized rest");

  course.restMicrosAfterStage = {0};
  expect(replay::validateCourseReplayPlayback(course, sources, bounds).issue ==
             replay::ReplayPlaybackIssue::CourseShape,
         "course stage/source/bounds/rest counts must agree");

  course.restMicrosAfterStage = {0, 0};
  replay::ReplayLimits oneStage = replay::kReplayLimits;
  oneStage.maxCourseStages = 1;
  expect(replay::validateCourseReplayPlayback(
             course, sources, bounds, oneStage).issue ==
             replay::ReplayPlaybackIssue::CourseStageCount,
         "course stage count uses the shared limit");
}

} // namespace

int main() {
  testPreRollIsSharedByEveryTimedCollection();
  testOrderingIsRejectedInsteadOfSorted();
  testControlAndScratchDirectionMatrix();
  testRedundancyAndScratchOwnershipHandoff();
  testCollectionCountsAndSupplementalValues();
  testCourseEnvelopeSupportsMixedStageSources();
  if (failures != 0) {
    std::cerr << failures << " replay playback test(s) failed\n";
    return 1;
  }
  std::cout << "replay playback tests passed\n";
  return 0;
}
```

- [ ] **Step 2: Register the test and verify the shared playback interface is missing**

Add:

```cmake
    add_executable(replay_playback_tests
        tests/replay_playback_tests.cpp
        src/replay/ReplayPlayback.cpp
        src/replay/ReplaySetup.cpp
    )
    target_include_directories(replay_playback_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(replay_playback_tests PRIVATE cxx_std_23)
```

Add `replay_playback_tests` to the test registration list immediately after `replay_setup_tests`.

Run:

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target replay_playback_tests -j 6
```

Expected: FAIL because `ReplayPlayback.h` and `.cpp` do not exist.

- [ ] **Step 3: Add raw playback value types and the shared validation interface**

Create `src/replay/ReplayPlayback.h`:

```cpp
#pragma once

#include "ReplaySetup.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace replay {

enum class LogicalControlKind : std::uint8_t {
  Lane,
  ScratchClockwise,
  ScratchCounterClockwise,
  Start,
  Select,
};

struct LogicalControl {
  LogicalControlKind kind = LogicalControlKind::Lane;
  int player = 1;
  int lane = -1;

  bool operator==(const LogicalControl &) const = default;
};

struct InputTransition {
  std::int64_t songTimeMicros = 0;
  LogicalControl control;
  bool pressed = false;
  bool replayOnly = false;

  bool operator==(const InputTransition &) const = default;
};

enum class ReplayTouchAction : std::uint8_t {
  Down,
  Move,
  Up,
  Cancel,
};

struct ReplayTouchSample {
  ReplayTouchAction action = ReplayTouchAction::Move;
  std::int64_t fingerId = 0;
  std::int64_t songTimeMicros = 0;
  float x = 0.0F;
  float y = 0.0F;

  bool operator==(const ReplayTouchSample &) const = default;
};

struct ReplayLaneCoverEvent {
  std::int64_t songTimeMicros = 0;
  int noteStartPositionPercent = 0;
  bool resetVisibleTimeReference = false;

  bool operator==(const ReplayLaneCoverEvent &) const = default;
};

struct ReplayPlaybackData {
  ReplaySetup setup;
  std::vector<InputTransition> input;
  std::vector<ReplayTouchSample> touchSamples;
  std::vector<ReplayLaneCoverEvent> laneCoverEvents;

  bool operator==(const ReplayPlaybackData &) const = default;
};

struct CourseReplayPlaybackData {
  std::vector<ReplayPlaybackData> stages;
  std::vector<std::int64_t> restMicrosAfterStage;

  bool operator==(const CourseReplayPlaybackData &) const = default;
};

enum class ReplayPlaybackIssue : std::uint8_t {
  None,
  Setup,
  TimeBounds,
  InputCount,
  InputTime,
  InputOrder,
  InputControl,
  RedundantInput,
  ScratchHandoff,
  TouchCount,
  TouchTime,
  TouchOrder,
  TouchAction,
  TouchCoordinate,
  LaneCoverCount,
  LaneCoverTime,
  LaneCoverOrder,
  LaneCoverPercent,
  CourseStageCount,
  CourseShape,
  CourseRest,
};

struct ReplayPlaybackValidation {
  ReplayPlaybackIssue issue = ReplayPlaybackIssue::None;
  ReplaySetupIssue setupIssue = ReplaySetupIssue::None;
  std::size_t stageIndex = 0;

  [[nodiscard]] constexpr bool valid() const noexcept {
    return issue == ReplayPlaybackIssue::None;
  }
};

[[nodiscard]] ReplayPlaybackValidation validateReplayPlayback(
    const ReplayPlaybackData &data, ReplaySetupSource source,
    ReplayTimeBounds timeBounds,
    const ReplayLimits &limits = kReplayLimits);

[[nodiscard]] ReplayPlaybackValidation validateCourseReplayPlayback(
    const CourseReplayPlaybackData &data,
    std::span<const ReplaySetupSource> sources,
    std::span<const ReplayTimeBounds> timeBounds,
    const ReplayLimits &limits = kReplayLimits);

} // namespace replay
```

- [ ] **Step 4: Implement the one raw-playback validator used by later producers and codecs**

Create `src/replay/ReplayPlayback.cpp`:

```cpp
#include "ReplayPlayback.h"

#include <array>
#include <cmath>
#include <optional>

namespace replay {
namespace {

ReplayPlaybackValidation invalid(ReplayPlaybackIssue issue) noexcept {
  return {.issue = issue};
}

int playerCount(int keyMode) noexcept {
  return keyMode == 10 || keyMode == 14 || keyMode == 48 ? 2 : 1;
}

int laneCountPerPlayer(int keyMode) noexcept {
  switch (keyMode) {
  case 5:
  case 10:
    return 5;
  case 7:
  case 14:
    return 7;
  case 9:
    return 9;
  case 24:
  case 48:
    return 26;
  default:
    return 0;
  }
}

bool scratchMode(int keyMode) noexcept {
  return keyMode == 5 || keyMode == 7 || keyMode == 10 || keyMode == 14;
}

bool scratchKind(LogicalControlKind kind) noexcept {
  return kind == LogicalControlKind::ScratchClockwise ||
         kind == LogicalControlKind::ScratchCounterClockwise;
}

bool validControl(const LogicalControl &control, int keyMode) noexcept {
  const int players = playerCount(keyMode);
  if (control.player < 1 || control.player > players) {
    return false;
  }
  switch (control.kind) {
  case LogicalControlKind::Lane:
    return control.lane >= 0 &&
           control.lane < laneCountPerPlayer(keyMode);
  case LogicalControlKind::ScratchClockwise:
  case LogicalControlKind::ScratchCounterClockwise:
    return scratchMode(keyMode) && control.lane == -1;
  case LogicalControlKind::Start:
  case LogicalControlKind::Select:
    return control.lane == -1;
  }
  return false;
}

std::size_t controlSlot(const LogicalControl &control) noexcept {
  switch (control.kind) {
  case LogicalControlKind::Lane:
    return static_cast<std::size_t>(control.player * 64 + control.lane);
  case LogicalControlKind::ScratchClockwise:
    return static_cast<std::size_t>(256 + control.player * 2);
  case LogicalControlKind::ScratchCounterClockwise:
    return static_cast<std::size_t>(257 + control.player * 2);
  case LogicalControlKind::Start:
    return static_cast<std::size_t>(300 + control.player);
  case LogicalControlKind::Select:
    return static_cast<std::size_t>(304 + control.player);
  }
  return 319;
}

struct PendingScratchHandoff {
  std::int64_t songTimeMicros = 0;
  LogicalControlKind released = LogicalControlKind::ScratchClockwise;
  bool active = false;
};

ReplayPlaybackIssue validateInput(
    std::span<const InputTransition> input, int keyMode,
    ReplayTimeBounds bounds, const ReplayLimits &limits) noexcept {
  if (!withinReplayCountLimit(input.size(), limits.maxInputTransitions)) {
    return ReplayPlaybackIssue::InputCount;
  }
  std::array<bool, 320> states{};
  std::array<std::optional<LogicalControlKind>, 3> activeScratch{};
  std::array<PendingScratchHandoff, 3> pending{};
  std::int64_t previous = 0;
  bool hasPrevious = false;

  for (const auto &transition : input) {
    if (!bounds.contains(transition.songTimeMicros, limits)) {
      return ReplayPlaybackIssue::InputTime;
    }
    if (hasPrevious && transition.songTimeMicros < previous) {
      return ReplayPlaybackIssue::InputOrder;
    }
    previous = transition.songTimeMicros;
    hasPrevious = true;

    for (std::size_t player = 1; player < pending.size(); ++player) {
      if (pending[player].active &&
          transition.songTimeMicros > pending[player].songTimeMicros) {
        return ReplayPlaybackIssue::ScratchHandoff;
      }
    }
    if (!validControl(transition.control, keyMode)) {
      return ReplayPlaybackIssue::InputControl;
    }
    const std::size_t slot = controlSlot(transition.control);
    if (states[slot] == transition.pressed) {
      return ReplayPlaybackIssue::RedundantInput;
    }
    states[slot] = transition.pressed;

    const int player = transition.control.player;
    if (transition.replayOnly) {
      if (!scratchKind(transition.control.kind)) {
        return ReplayPlaybackIssue::ScratchHandoff;
      }
      auto &active = activeScratch[static_cast<std::size_t>(player)];
      auto &handoff = pending[static_cast<std::size_t>(player)];
      if (!transition.pressed) {
        if (handoff.active || !active ||
            *active != transition.control.kind) {
          return ReplayPlaybackIssue::ScratchHandoff;
        }
        handoff = {.songTimeMicros = transition.songTimeMicros,
                   .released = transition.control.kind,
                   .active = true};
      } else {
        if (!handoff.active ||
            handoff.songTimeMicros != transition.songTimeMicros ||
            handoff.released == transition.control.kind) {
          return ReplayPlaybackIssue::ScratchHandoff;
        }
        active = transition.control.kind;
        handoff = {};
      }
      continue;
    }

    if (scratchKind(transition.control.kind)) {
      auto &active = activeScratch[static_cast<std::size_t>(player)];
      if (pending[static_cast<std::size_t>(player)].active) {
        return ReplayPlaybackIssue::ScratchHandoff;
      }
      if (transition.pressed) {
        active = transition.control.kind;
      } else if (active && *active == transition.control.kind) {
        active.reset();
      }
    }
  }

  for (const auto &handoff : pending) {
    if (handoff.active) {
      return ReplayPlaybackIssue::ScratchHandoff;
    }
  }
  return ReplayPlaybackIssue::None;
}

bool validTouchAction(ReplayTouchAction action) noexcept {
  switch (action) {
  case ReplayTouchAction::Down:
  case ReplayTouchAction::Move:
  case ReplayTouchAction::Up:
  case ReplayTouchAction::Cancel:
    return true;
  }
  return false;
}

ReplayPlaybackIssue validateTouch(
    std::span<const ReplayTouchSample> samples, ReplayTimeBounds bounds,
    const ReplayLimits &limits) noexcept {
  if (!withinReplayCountLimit(samples.size(), limits.maxTouchSamples)) {
    return ReplayPlaybackIssue::TouchCount;
  }
  std::int64_t previous = 0;
  bool hasPrevious = false;
  for (const auto &sample : samples) {
    if (!bounds.contains(sample.songTimeMicros, limits)) {
      return ReplayPlaybackIssue::TouchTime;
    }
    if (hasPrevious && sample.songTimeMicros < previous) {
      return ReplayPlaybackIssue::TouchOrder;
    }
    previous = sample.songTimeMicros;
    hasPrevious = true;
    if (!validTouchAction(sample.action)) {
      return ReplayPlaybackIssue::TouchAction;
    }
    if (!std::isfinite(sample.x) || !std::isfinite(sample.y) ||
        sample.x < 0.0F || sample.x > 1.0F || sample.y < 0.0F ||
        sample.y > 1.0F) {
      return ReplayPlaybackIssue::TouchCoordinate;
    }
  }
  return ReplayPlaybackIssue::None;
}

ReplayPlaybackIssue validateLaneCover(
    std::span<const ReplayLaneCoverEvent> events, ReplayTimeBounds bounds,
    const ReplayLimits &limits) noexcept {
  if (!withinReplayCountLimit(events.size(), limits.maxLaneCoverEvents)) {
    return ReplayPlaybackIssue::LaneCoverCount;
  }
  std::int64_t previous = 0;
  bool hasPrevious = false;
  for (const auto &event : events) {
    if (!bounds.contains(event.songTimeMicros, limits)) {
      return ReplayPlaybackIssue::LaneCoverTime;
    }
    if (hasPrevious && event.songTimeMicros < previous) {
      return ReplayPlaybackIssue::LaneCoverOrder;
    }
    previous = event.songTimeMicros;
    hasPrevious = true;
    if (event.noteStartPositionPercent < 0 ||
        event.noteStartPositionPercent > 100) {
      return ReplayPlaybackIssue::LaneCoverPercent;
    }
  }
  return ReplayPlaybackIssue::None;
}

} // namespace

ReplayPlaybackValidation validateReplayPlayback(
    const ReplayPlaybackData &data, ReplaySetupSource source,
    ReplayTimeBounds timeBounds, const ReplayLimits &limits) {
  const auto setup = validateReplaySetup(data.setup, source, limits);
  if (!setup.valid()) {
    return {.issue = ReplayPlaybackIssue::Setup,
            .setupIssue = setup.issue};
  }
  if (!timeBounds.valid()) {
    return invalid(ReplayPlaybackIssue::TimeBounds);
  }
  if (const auto issue = validateInput(data.input, data.setup.chart.keyMode,
                                       timeBounds, limits);
      issue != ReplayPlaybackIssue::None) {
    return invalid(issue);
  }
  if (const auto issue = validateTouch(data.touchSamples, timeBounds, limits);
      issue != ReplayPlaybackIssue::None) {
    return invalid(issue);
  }
  if (const auto issue =
          validateLaneCover(data.laneCoverEvents, timeBounds, limits);
      issue != ReplayPlaybackIssue::None) {
    return invalid(issue);
  }
  return {};
}

ReplayPlaybackValidation validateCourseReplayPlayback(
    const CourseReplayPlaybackData &data,
    std::span<const ReplaySetupSource> sources,
    std::span<const ReplayTimeBounds> timeBounds,
    const ReplayLimits &limits) {
  if (!limits.valid() || data.stages.empty() ||
      !withinReplayCountLimit(data.stages.size(), limits.maxCourseStages)) {
    return invalid(ReplayPlaybackIssue::CourseStageCount);
  }
  if (sources.size() != data.stages.size() ||
      timeBounds.size() != data.stages.size() ||
      data.restMicrosAfterStage.size() != data.stages.size()) {
    return invalid(ReplayPlaybackIssue::CourseShape);
  }
  for (std::size_t index = 0; index < data.stages.size(); ++index) {
    auto stage = validateReplayPlayback(data.stages[index], sources[index],
                                        timeBounds[index], limits);
    if (!stage.valid()) {
      stage.stageIndex = index;
      return stage;
    }
    if (!validCourseRestMicros(data.restMicrosAfterStage[index], limits)) {
      return {.issue = ReplayPlaybackIssue::CourseRest,
              .stageIndex = index};
    }
  }
  return {};
}

} // namespace replay
```

Append this exact source registration after `ReplaySetup.cpp` in
`src/CMakeLists.txt`:

```cmake
target_sources(main PRIVATE replay/ReplayPlayback.cpp)
```

Do not include `ReplayPlayback.h` from an existing runtime file.

- [ ] **Step 5: Run the focused playback suite and commit**

Run:

```bash
cmake --build cmake-build-debug --target replay_playback_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^replay_playback_tests$'
git diff --check
```

Expected: raw playback closure tests and the desktop application build pass, with no recorder, codec, repository, or scene integration.

Commit:

```bash
git add CMakeLists.txt src/CMakeLists.txt src/replay/ReplayPlayback.h src/replay/ReplayPlayback.cpp tests/replay_playback_tests.cpp
git commit -m "test: define raw replay playback invariants"
```

---

### Task 5: Characterize the Schema-v10 Legacy Summary Boundary

**Files:**
- Modify: `tests/replay_repository_tests.cpp:1-130`
- Modify: `tests/replay_repository_tests.cpp:7990-8089`

**Interfaces:**
- Consumes: existing `ReplayRepository::EnsureSchema`, `queryInt`, `tableExists`, and `columnExists` test helpers.
- Produces: a characterization test only; it adds no production interface and does not query legacy detail rows for derived facts.

- [ ] **Step 1: Add a failing characterization before changing any schema code**

Add this function near the end of the anonymous namespace in `tests/replay_repository_tests.cpp`:

```cpp
void testSchema10LegacySummaryBoundaryIsHeaderOnly(
    const std::filesystem::path &root) {
  static_assert(ReplayRepository::kCurrentSchemaVersion == 10,
                "move this characterization to a frozen v10 fixture before "
                "advancing the production schema");
  const auto path = root / "schema10-summary-boundary" / "replay.db";
  ReplayRepository repository(path);
  assert(repository.EnsureSchema());
  auto db = openDatabase(path);
  assert(queryInt(db.get(), "PRAGMA user_version") == 10);

  constexpr std::array chartHeaderFacts{
      "id",
      "chart_path",
      "chart_md5",
      "chart_sha256",
      "chart_title",
      "chart_artist",
      "ln_mode",
      "final_score",
      "max_combo",
      "final_gauge",
      "clear_type",
      "created_at",
      "ruleset_version",
      "eligibility",
      "provenance_json",
  };
  for (const char *column : chartHeaderFacts) {
    assert(columnExists(db.get(), "replays", column));
  }

  constexpr std::array courseHeaderFacts{
      "id",
      "course_id",
      "course_key",
      "course_name",
      "course_group_name",
      "constraint_json",
      "final_score",
      "max_combo",
      "final_gauge",
      "clear_type",
      "completed_charts",
      "total_charts",
      "created_at",
      "ruleset_version",
      "eligibility",
      "provenance_json",
  };
  for (const char *column : courseHeaderFacts) {
    assert(columnExists(db.get(), "course_replays", column));
  }

  constexpr std::array eventDerivedFacts{
      "max_score",
      "p_great",
      "great",
      "good",
      "bad",
      "poor",
      "k_poor",
      "fast",
      "slow",
      "gauge_history_json",
      "judgement_history_json",
      "timing_history_json",
  };
  for (const char *column : eventDerivedFacts) {
    assert(!columnExists(db.get(), "replays", column));
    assert(!columnExists(db.get(), "course_replays", column));
  }

  assert(tableExists(db.get(), "replay_events"));
  assert(tableExists(db.get(), "replay_touch_samples"));
  assert(tableExists(db.get(), "replay_lane_cover_events"));
  assert(tableExists(db.get(), "course_replay_stages"));
  for (const char *column :
       {"judgement", "diff_micros", "gauge", "gauge_type", "combo",
        "score"}) {
    assert(columnExists(db.get(), "replay_events", column));
  }
}
```

Call it immediately before `testExistingListLimits(root);` in `main()`:

```cpp
  testSchema10LegacySummaryBoundaryIsHeaderOnly(root);
  testExistingListLimits(root);
```

To prove the test is meaningful before accepting it, temporarily add `"max_score"` to `chartHeaderFacts`, build, and run:

```bash
cmake --build cmake-build-debug --target replay_repository_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^replay_repository_tests$'
```

Expected: FAIL at `columnExists(db.get(), "replays", "max_score")`.

- [ ] **Step 2: Restore the exact header-only contract and run it against unchanged develop schema code**

Remove the temporary `"max_score"` entry from `chartHeaderFacts`. Do not modify `ReplayRepositorySchema.cpp`, `ReplayRepositoryRecords.cpp`, `ReplayRepository.h`, or the schema version.

Run:

```bash
cmake --build cmake-build-debug --target replay_repository_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^replay_repository_tests$'
git diff -- src/repositories src/ReplayData.h
```

Expected: the repository test passes, and the final `git diff` command prints nothing. The test now records that judgement totals, timing, gauge history, and maximum score cannot be preserved as legacy summary facts without reading replay detail.

- [ ] **Step 3: Commit the characterization**

```bash
git add tests/replay_repository_tests.cpp
git commit -m "test: characterize legacy replay header boundary"
```

---

### Task 6: Enforce Replay Contract Decoupling and Pass the Slice Gate

**Files:**
- Create: `tests/replay_contract_boundary_tests.cpp`
- Modify: `CMakeLists.txt:611-619`
- Modify: `CMakeLists.txt:2773-2845`

**Interfaces:**
- Consumes: `replay::ReplaySetup`, `replay::ReplayPlaybackData`, `replay::CourseReplayPlaybackData`, `replay::ReplayCapabilities`, and the repository source root supplied by CMake.
- Produces: a permanent audit that prevents playback values from acquiring result/persistence fields, keeps setup free of raw collections, and keeps capability policy free of repositories/files/scenes.

- [ ] **Step 1: Write the failing compile/source boundary audit**

Create `tests/replay_contract_boundary_tests.cpp`:

```cpp
#include "replay/ReplayCapabilities.h"
#include "replay/ReplayPlayback.h"
#include "replay/ReplaySetup.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

#ifndef ASOBMASHOW_SOURCE_DIR
#error "ASOBMASHOW_SOURCE_DIR must identify the repository root"
#endif

template <typename T>
concept HasResultFact = requires(T value) { value.finalScore; } ||
                        requires(T value) { value.maxCombo; } ||
                        requires(T value) { value.finalGauge; } ||
                        requires(T value) { value.clearType; } ||
                        requires(T value) { value.createdAt; } ||
                        requires(T value) { value.attemptId; } ||
                        requires(T value) { value.resultFingerprint; };

template <typename T>
concept HasRawReplayCollection = requires(T value) { value.events; } ||
                                 requires(T value) { value.touchSamples; } ||
                                 requires(T value) { value.laneCoverEvents; };

static_assert(!HasResultFact<replay::ReplaySetup>);
static_assert(!HasRawReplayCollection<replay::ReplaySetup>);
static_assert(!HasResultFact<replay::ReplayPlaybackData>);
static_assert(!HasResultFact<replay::CourseReplayPlaybackData>);
static_assert(!HasResultFact<replay::ReplayCapabilities>);

namespace {

int failures = 0;

std::string readText(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void rejectTokens(const std::filesystem::path &path,
                  const std::array<std::string_view, 12> &tokens) {
  const std::string text = readText(path);
  for (std::string_view token : tokens) {
    if (text.contains(token)) {
      std::cerr << "FAIL: " << path.filename().string()
                << " contains forbidden boundary token " << token << '\n';
      ++failures;
    }
  }
}

void testPlaybackSetupBoundary() {
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  constexpr std::array<std::string_view, 12> forbidden{
      "ReplayData",
      "ScoreProvenance",
      "ResultPersistence",
      "IrSubmission",
      "ReplayRepository",
      "sqlite3",
      "attemptId",
      "resultFingerprint",
      "finalScore",
      "maxCombo",
      "finalGauge",
      "clearType",
  };
  rejectTokens(root / "src/replay/ReplaySetup.h", forbidden);
  rejectTokens(root / "src/replay/ReplaySetup.cpp", forbidden);
  rejectTokens(root / "src/replay/ReplayPlayback.h", forbidden);
  rejectTokens(root / "src/replay/ReplayPlayback.cpp", forbidden);
  rejectTokens(root / "src/replay/ReplayLimits.h", forbidden);
}

void testCapabilityPolicyBoundary() {
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  constexpr std::array<std::string_view, 12> forbidden{
      "ReplayData",
      "ScoreProvenance",
      "ResultPersistence",
      "IrOutbox",
      "ReplayRepository",
      "sqlite3",
      "filesystem",
      "fstream",
      "GamePlayScene",
      "ResultScene",
      "ProfileArchive",
      "ReplayFileStore",
  };
  rejectTokens(root / "src/replay/ReplayCapabilities.h", forbidden);
  rejectTokens(root / "src/replay/ReplayCapabilities.cpp", forbidden);
}

} // namespace

int main() {
  testPlaybackSetupBoundary();
  testCapabilityPolicyBoundary();
  if (failures != 0) {
    std::cerr << failures << " replay contract boundary test(s) failed\n";
    return 1;
  }
  std::cout << "replay contract boundary tests passed\n";
  return 0;
}
```

- [ ] **Step 2: Register the audit and prove it catches coupling**

Add near `repository_boundary_tests`:

```cmake
    add_executable(replay_contract_boundary_tests
        tests/replay_contract_boundary_tests.cpp
    )
    target_include_directories(replay_contract_boundary_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(replay_contract_boundary_tests PRIVATE cxx_std_23)
    target_compile_definitions(replay_contract_boundary_tests PRIVATE
        ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
```

Register it beside the existing direct boundary test:

```cmake
    asobmashow_register_test(repository_boundary_tests)
    asobmashow_register_test(replay_contract_boundary_tests)
    asobmashow_register_test(find_bms_download_tests)
```

Temporarily add `#include "../ScoreProvenance.h"` to `ReplaySetup.cpp`, then run:

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target replay_contract_boundary_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^replay_contract_boundary_tests$'
```

Expected: FAIL with `ReplaySetup.cpp contains forbidden boundary token ScoreProvenance`.

- [ ] **Step 3: Remove the injected coupling and run all focused Slice 1 tests**

Remove the temporary include, then run:

```bash
cmake --build cmake-build-debug --target replay_capabilities_tests replay_limits_tests replay_setup_tests replay_playback_tests replay_contract_boundary_tests replay_repository_tests result_record_summary_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(replay_capabilities_tests|replay_limits_tests|replay_setup_tests|replay_playback_tests|replay_contract_boundary_tests|replay_repository_tests|result_record_summary_tests)$'
```

Expected: all listed targets build and all seven tests pass.

- [ ] **Step 4: Audit that the slice contains no runtime or schema cutover**

Run:

```bash
git diff --stat origin/develop...HEAD
git diff origin/develop...HEAD -- src/repositories src/ReplayData.h src/ResultPersistenceModel.h src/ResultPersistenceCoordinator.cpp src/ResultRecallBuilder.cpp src/ir src/scene src/ProfileArchive.cpp
git grep -n 'ReplayCapabilities\|ReplaySetup\|ReplayPlayback\|ReplayLimits' -- src ':!src/replay'
```

Expected:

- The first command shows only the approved design, this plan, matrix documentation, focused contract files/tests, CMake registrations, and the single schema-v10 characterization addition.
- The second command prints nothing.
- The third command prints only `src/CMakeLists.txt` source registration; no runtime source includes a new replay contract header.

- [ ] **Step 5: Run the full configured test and desktop build gate**

Run:

```bash
ctest --test-dir cmake-build-debug --output-on-failure
cmake --build cmake-build-debug --target main -j 6
git diff --check
git status --short
```

Expected: all configured CTest tests pass, `main` builds, `git diff --check` is silent, and status lists only the new audit/CMake changes for this task.

- [ ] **Step 6: Commit the audit and confirm the branch is clean**

```bash
git add CMakeLists.txt tests/replay_contract_boundary_tests.cpp
git commit -m "test: enforce replay contract boundaries"
git status --short --branch
```

Expected: the branch is ahead of `origin/develop` with no uncommitted files.

- [ ] **Step 7: Request one review for the completed slice**

Ask the reviewer to evaluate only these questions:

1. Does `capabilitiesFor` cover every documented origin/state without granting replay authority to a result or IR snapshot?
2. Can a producer create input, touch, lane-cover, course, timestamp, count, or rest data that `validateReplayPlayback` rejects?
3. Are scratch ownership handoffs and every timed collection governed by the same pre-roll and monotonic-order contract?
4. Does `ReplaySetup` contain every setup fact needed later while containing no result, provenance, IR, database, or raw-event fact?
5. Does schema-v10 characterization preserve only independently stored header facts and explicitly exclude event-derived detail?
6. Did any runtime call site, schema version, table, or persistence behavior change in Slice 1?

Do not start Slice 2 until verified P1/P2 findings against these six questions are resolved with a boundary-level test.

---

## Slice 1 Exit Criteria

- `replay::capabilitiesFor` is the sole executable capability matrix, but no runtime consumer uses it yet.
- `replay::kReplayLimits` owns all pinned resource bounds, and pre-roll/completion/order/rest edge tests pass.
- `ReplaySetup` and chart identity agreement are pure, validated, and decoupled from replay results/provenance/IR/persistence.
- `validateReplayPlayback` is the sole raw playback validator and directly covers pre-roll, ordering, logical controls, both scratch directions, replay-only scratch handoff, supplemental tracks, and course shape.
- The schema-v10 test proves which chart/course header facts can be copied later without reading raw replay detail.
- No schema, repository, gameplay, result, IR, profile, or UI production behavior differs from `develop`.
- Focused tests, full CTest, and the desktop `main` build pass before review.
