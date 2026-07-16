# Start Lane Indicators Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add enabled-by-default, lane-colored starting indicators with a two-real-second normal-play phase, practice count-in integration, signed replay preparation input, and matching replay video export.

**Architecture:** A new `PreparationPlan` unit combines existing metronome/count-in planning with first-playable-lane discovery and a signed playback origin. `GamePlayScene` and `ReplayVideoExporter` share that plan, while `BMSRenderer` receives only cue lanes and current visibility. Pure geometry and clock conversion helpers keep lane-cover placement and export timing independently testable.

**Tech Stack:** C++23, bgfx `SimpleBatchRenderer`, SDL-based gameplay, nlohmann JSON settings, libsndfile offline audio, CMake/CTest.

## Global Constraints

- `startLaneIndicatorsEnabled` defaults to `true`, including older JSON and legacy settings files that omit it.
- Normal play shows the cue for exactly `2,000,000` real microseconds at every supported playback rate.
- Practice adds no time; the cue uses the existing count-in and ends at the configured practice start.
- The cue setting remains independent of `prepMetronomeEnabled`.
- Cue input may animate lane beams, touch visualization, and floating lane cover, but may not judge notes.
- Normal notes and long-note heads qualify; invisible notes, landmines, long-note tails, and malformed entries do not.
- Triangles render after notes and before the lane cover, with a visible gap until the cover leaves insufficient space.
- Interactive replay, course replay, single replay export, and course replay export use signed preparation time and the same preparation plan.
- Chart preview playback remains unchanged.
- Do not edit `src/bms_parser.hpp` or `src/bms_parser.cpp`.

---

## File Structure

- Create `src/PreparationPlan.h` and `src/PreparationPlan.cpp`: lane discovery, normal/practice preparation planning, signed playback origin, visibility, and chart/real clock conversion.
- Create `src/scene/play/StartLaneIndicatorGeometry.h`: pure white/blue/red role selection and lane-cover-aware triangle placement.
- Create `src/audio/PrepMetronomeSound.h` and `src/audio/PrepMetronomeSound.cpp`: shared generated accent/regular click PCM for live playback and export.
- Create `tests/preparation_plan_tests.cpp` and `tests/start_lane_indicator_geometry_tests.cpp`: focused non-renderer tests.
- Modify settings, gameplay, renderer, replay exporter, offline chart audio, their focused tests, and CMake registration only where listed below.

### Task 1: Shared Preparation Plan and Lane Selection

**Files:**
- Create: `src/PreparationPlan.h`
- Create: `src/PreparationPlan.cpp`
- Create: `tests/preparation_plan_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `prep_metronome::buildPlan`, `prep_metronome::buildPracticeCountInPlan`, `audio::PlaybackRate`, and parsed `bms_parser::Chart` objects after play-option application.
- Produces: `preparation::Plan`, `preparation::buildNormalPlan`, `preparation::buildPracticePlan`, `preparation::firstPlayableLanes`, `Plan::indicatorVisibleAt`, `Plan::chartTimeAtRealTime`, and `Plan::realTimeAtChartTime`.

- [ ] **Step 1: Add the failing preparation-plan tests and CMake target**

Create `tests/preparation_plan_tests.cpp` with the following behavior-level test body. The chart helpers must assign both `Lane` and `Timeline` and place notes into the timeline lane slot.

```cpp
#include "../src/PreparationPlan.h"

#include <iostream>
#include <optional>
#include <vector>

namespace {
int failures = 0;
#define CHECK(value, label)                                                    \
  do {                                                                         \
    if (!(value)) {                                                            \
      std::cerr << "FAIL: " << label << '\n';                                \
      ++failures;                                                              \
    }                                                                          \
  } while (false)

bms_parser::TimeLine *addTimeline(bms_parser::Chart &chart, long long timing) {
  if (chart.Measures.empty()) {
    chart.Measures.push_back(new bms_parser::Measure());
  }
  auto *timeline = new bms_parser::TimeLine(16, false);
  timeline->Timing = timing;
  chart.Measures.front()->TimeLines.push_back(timeline);
  return timeline;
}

bms_parser::Note *addNote(bms_parser::TimeLine &timeline, int lane) {
  auto *note = new bms_parser::Note(1);
  note->Lane = lane;
  note->Timeline = &timeline;
  timeline.Notes[static_cast<std::size_t>(lane)] = note;
  return note;
}

bms_parser::LongNote *addLongNote(bms_parser::TimeLine &timeline, int lane,
                                  bool tail) {
  auto *note = new bms_parser::LongNote(
      1, bms_parser::LongNoteType::ChargeNote);
  note->Lane = lane;
  note->Timeline = &timeline;
  if (tail) {
    auto *head = new bms_parser::LongNote(
        1, bms_parser::LongNoteType::ChargeNote);
    head->Tail = note;
    note->Head = head;
  }
  timeline.Notes[static_cast<std::size_t>(lane)] = note;
  return note;
}
} // namespace

int main() {
  const audio::PlaybackRate normal{.percent = 100};
  const audio::PlaybackRate half{.percent = 50};
  const audio::PlaybackRate doubleSpeed{.percent = 200};

  bms_parser::Chart chart;
  chart.Meta.Bpm = 120.0;
  chart.Meta.GuessedBeatsPerMeasure = 4;
  addTimeline(chart, 0);
  auto *firstPlayable = addTimeline(chart, 1'000'000);
  addNote(*firstPlayable, 1);
  addNote(*firstPlayable, 3);
  auto *later = addTimeline(chart, 2'000'000);
  addNote(*later, 5);

  CHECK(preparation::firstPlayableLanes(chart, 0, std::nullopt) ==
            std::vector<int>({1, 3}),
        "only the earliest playable chord is selected");
  CHECK(preparation::firstPlayableLanes(chart, 1'500'000, 3'000'000) ==
            std::vector<int>({5}),
        "range start selects the first later timeline");
  CHECK(preparation::firstPlayableLanes(chart, 2'500'000, 3'000'000).empty(),
        "empty active range has no cue lanes");

  for (const auto rate : {half, normal, doubleSpeed}) {
    const auto plan = preparation::buildNormalPlan(
        chart, true, false, 0, 0, std::nullopt, rate);
    CHECK(plan.laneIndicator.enabled(), "normal cue is enabled");
    CHECK(plan.laneIndicator.endTimeMicros == 0,
          "metronome-off cue ends at the playback anchor");
    CHECK(plan.realTimeAtChartTime(plan.laneIndicator.endTimeMicros) ==
              2'000'000,
          "cue is exactly two real seconds at every playback rate");
    CHECK(plan.indicatorVisibleAt(plan.playbackStartTimeMicros),
          "cue is visible at playback start");
    CHECK(!plan.indicatorVisibleAt(plan.laneIndicator.endTimeMicros),
          "cue hides at its exclusive end");
  }

  const auto metronomePlan = preparation::buildNormalPlan(
      chart, true, true, 0, 0, std::nullopt, normal);
  CHECK(metronomePlan.laneIndicator.endTimeMicros ==
            metronomePlan.metronome.startTimeMicros,
        "cue ends at the first metronome click");
  CHECK(metronomePlan.playbackStartTimeMicros == -4'000'000,
        "120 BPM count-in follows the two-second cue");

  const auto disabled = preparation::buildNormalPlan(
      chart, false, true, 0, 0, std::nullopt, normal);
  CHECK(!disabled.laneIndicator.enabled(), "setting disables the cue");
  CHECK(disabled.playbackStartTimeMicros == disabled.metronome.startTimeMicros,
        "disabled cue preserves existing metronome timing");

  const auto practice = preparation::buildPracticePlan(
      chart, true, 1'000'000, 3'000'000, 4, normal);
  CHECK(practice.laneIndicator.lanes == std::vector<int>({1, 3}),
        "practice selects its first in-range chord");
  CHECK(practice.laneIndicator.startTimeMicros ==
            practice.metronome.startTimeMicros,
        "practice cue starts with the existing count-in");
  CHECK(practice.laneIndicator.endTimeMicros == 1'000'000,
        "practice cue ends at practice start");
  CHECK(practice.playbackStartTimeMicros == practice.metronome.startTimeMicros,
        "practice adds no lead-in");

  bms_parser::Chart filtered;
  filtered.Meta.Bpm = 120.0;
  auto *filteredTimeline = addTimeline(filtered, 0);
  addLongNote(*filteredTimeline, 2, true);
  auto *mine = new bms_parser::LandmineNote(10.0f);
  mine->Lane = 4;
  mine->Timeline = filteredTimeline;
  filteredTimeline->Notes[4] = mine;
  auto *invisible = new bms_parser::Note(1);
  invisible->Lane = 5;
  invisible->Timeline = filteredTimeline;
  filteredTimeline->InvisibleNotes[5] = invisible;
  auto *malformed = new bms_parser::Note(1);
  malformed->Lane = 7;
  filteredTimeline->Notes[7] = malformed;
  auto *headTimeline = addTimeline(filtered, 500'000);
  addLongNote(*headTimeline, 6, false);
  filtered.Measures.insert(filtered.Measures.begin(), nullptr);
  CHECK(preparation::firstPlayableLanes(filtered, 0, std::nullopt) ==
            std::vector<int>({6}),
        "tails and mines are ignored while long-note heads qualify");

  bms_parser::Chart empty;
  empty.Meta.Bpm = 120.0;
  addTimeline(empty, 0);
  const auto emptyPlan = preparation::buildNormalPlan(
      empty, true, false, 0, 0, std::nullopt, normal);
  CHECK(!emptyPlan.laneIndicator.enabled() &&
            emptyPlan.playbackStartTimeMicros == 0,
        "a chart without playable notes adds no cue delay");

  return failures == 0 ? 0 : 1;
}
```

Register `preparation_plan_tests` with `src/PreparationPlan.cpp`,
`src/PrepMetronome.cpp`, and `src/bms_parser.cpp`, and add it to the existing
CTest registration list. Add `PreparationPlan.cpp` to `src/CMakeLists.txt`.

- [ ] **Step 2: Run the new target and verify RED**

```bash
cmake --build cmake-build-debug --target preparation_plan_tests -j 6
```

Expected: compilation fails because `PreparationPlan.h` and the
`preparation::*` interfaces do not exist.

- [ ] **Step 3: Implement the minimal shared plan**

Create `src/PreparationPlan.h`:

```cpp
#pragma once

#include "PrepMetronome.h"
#include "audio/PlaybackRate.h"
#include "bms_parser.hpp"

#include <optional>
#include <vector>

namespace preparation {
inline constexpr long long kStartLaneIndicatorRealMicros = 2'000'000LL;

struct StartLaneIndicatorPlan {
  std::vector<int> lanes;
  long long startTimeMicros = 0;
  long long endTimeMicros = 0;
  [[nodiscard]] bool enabled() const;
  [[nodiscard]] bool visibleAt(long long chartTimeMicros) const;
};

struct Plan {
  prep_metronome::PrepMetronomePlan metronome;
  StartLaneIndicatorPlan laneIndicator;
  audio::PlaybackRate playback;
  long long playbackStartTimeMicros = 0;
  [[nodiscard]] bool indicatorVisibleAt(long long chartTimeMicros) const;
  [[nodiscard]] long long chartTimeAtRealTime(long long realTimeMicros) const;
  [[nodiscard]] long long realTimeAtChartTime(long long chartTimeMicros) const;
};

std::vector<int>
firstPlayableLanes(const bms_parser::Chart &chart, long long rangeStartMicros,
                   std::optional<long long> rangeEndMicros);
Plan buildNormalPlan(const bms_parser::Chart &chart, bool indicatorEnabled,
                     bool metronomeEnabled, long long playbackAnchorMicros,
                     long long noteRangeStartMicros,
                     std::optional<long long> noteRangeEndMicros,
                     audio::PlaybackRate playback);
Plan buildPracticePlan(const bms_parser::Chart &chart, bool indicatorEnabled,
                       long long startMicros, long long endMicros,
                       int countInBeats, audio::PlaybackRate playback);
} // namespace preparation
```

Implement `src/PreparationPlan.cpp` by walking measures/timelines in parsed
order, rejecting `nullptr`, landmines, long-note tails, notes without their
owning timeline, negative lanes, and out-of-range timings. Deduplicate and sort
the lanes found on the first qualifying timeline. Implement clock conversion:

```cpp
long long Plan::chartTimeAtRealTime(long long realTimeMicros) const {
  return playbackStartTimeMicros + playback.chartMicrosFromReal(realTimeMicros);
}
long long Plan::realTimeAtChartTime(long long chartTimeMicros) const {
  return playback.realMicrosFromChart(chartTimeMicros -
                                      playbackStartTimeMicros);
}
```

For normal planning, build the metronome first, choose its start or the audio
anchor as the indicator end, and subtract
`playback.chartMicrosFromReal(kStartLaneIndicatorRealMicros)` only when the
setting is enabled and qualifying lanes exist. For practice, reuse
`buildPracticeCountInPlan`; enable the cue only when that count-in is enabled
and qualifying lanes exist.

- [ ] **Step 4: Run focused plan and existing metronome tests**

```bash
cmake --build cmake-build-debug --target preparation_plan_tests prep_metronome_tests -j 6
./cmake-build-debug/preparation_plan_tests
./cmake-build-debug/prep_metronome_tests
```

Expected: both executables exit `0` with no failure output.

- [ ] **Step 5: Commit the shared plan**

```bash
git add CMakeLists.txt src/CMakeLists.txt src/PreparationPlan.h \
  src/PreparationPlan.cpp tests/preparation_plan_tests.cpp
git commit -m "feat(gameplay): plan start lane preparation cues"
```

### Task 2: Enabled-by-Default Setting and Visual Settings Control

**Files:**
- Modify: `src/AppSettings.h`
- Modify: `src/AppSettings.cpp`
- Modify: `src/AppSettingsStore.cpp`
- Modify: `src/scene/SettingsScene.h`
- Modify: `src/scene/SettingsScene.cpp`
- Modify: `src/scene/SettingsSceneLayout.cpp`
- Modify: `src/scene/SettingsSceneControls.cpp`
- Modify: `tests/app_settings_store_tests.cpp`
- Modify: `tests/fixtures/settings/settings-v0.json`
- Modify: `tests/fixtures/settings/settings-v1.json`
- Modify: `tests/fixtures/settings/legacy-full.cfg`
- Modify: `tests/fixtures/profiles/legacy-settings.cfg`

**Interfaces:**
- Consumes: existing `AppSettingsStore` optional-field loading and SettingsScene button/card patterns.
- Produces: `AppSettings::startLaneIndicatorsEnabled`, JSON key `startLaneIndicatorsEnabled`, legacy key `start_lane_indicators_enabled`, and the **Start Lane Indicators** Visual-tab toggle.

- [ ] **Step 1: Write failing persistence/default tests**

In `makeDistinctSettings()`, set:

```cpp
value.startLaneIndicatorsEnabled = false;
```

Extend `testJsonRoundTripIncludesAudioAndVideo()`:

```cpp
expect(readFile(path).find("\"startLaneIndicatorsEnabled\": false") !=
           std::string::npos,
       "saved JSON includes the start lane indicator setting");
```

Extend the minimal older-settings test:

```cpp
expect(legacy.settings.startLaneIndicatorsEnabled,
       "settings without the field default start lane indicators on");
```

Add `"startLaneIndicatorsEnabled": false` to both JSON fixtures and
`start_lane_indicators_enabled=0` to both legacy fixtures so the complete
fixture continues to equal `makeDistinctSettings()`.

- [ ] **Step 2: Run settings tests and verify RED**

```bash
cmake --build cmake-build-debug --target app_settings_store_tests -j 6
```

Expected: compilation fails because `startLaneIndicatorsEnabled` is missing.

- [ ] **Step 3: Persist and parse the setting**

Add to `AppSettings.h`:

```cpp
bool startLaneIndicatorsEnabled = true;
```

Add to `settingsToJson` and `settingsFromJson`:

```cpp
{"startLaneIndicatorsEnabled", settings.startLaneIndicatorsEnabled},
readValue(document, "startLaneIndicatorsEnabled",
          settings.startLaneIndicatorsEnabled, diagnostics);
```

Add this legacy branch in `AppSettings::parseLegacyCfg`:

```cpp
} else if (key == "start_lane_indicators_enabled") {
  bool parsed = settings.startLaneIndicatorsEnabled;
  if (parseBool(value, parsed)) {
    settings.startLaneIndicatorsEnabled = parsed;
  }
```

Keep `AppSettingsStore::kCurrentSchemaVersion` at `1`; optional-field loading
provides the compatible default.

- [ ] **Step 4: Add the Visual-tab control**

Add `TextView *startLaneIndicatorsModeText` and
`Button *startLaneIndicatorsModeButton` to `SettingsScene.h`; reset both to
`nullptr` wherever the other control pointers are cleared.

In `refreshSettingsText()` add:

```cpp
const std::string startLaneIndicatorsLabel =
    context.settings.startLaneIndicatorsEnabled ? "Shown" : "Hidden";
if (startLaneIndicatorsModeText != nullptr) {
  startLaneIndicatorsModeText->setText(startLaneIndicatorsLabel);
}
applySemanticButtonStyle(
    startLaneIndicatorsModeButton, startLaneIndicatorsModeText,
    context.settings.startLaneIndicatorsEnabled ? SettingsButtonTone::Success
                                                : SettingsButtonTone::Info);
```

In `buildVisualTab()`, add after **Invisible Notes**:

```cpp
auto *startLaneIndicatorControls = new View();
startLaneIndicatorControls->setFlexDirection(FlexDirection::Column);
startLaneIndicatorControls->setGap(metrics.compact ? 12.0f : 16.0f);
startLaneIndicatorControls->setAlignItems(YGAlignFlexStart);
startLaneIndicatorsModeText =
    makeText("", metrics.bodyTextSize + 6, ui_theme::textPrimary(),
             TextView::CENTER, TextView::MIDDLE);
startLaneIndicatorsModeButton = makeControlButton(
    metrics.actionButtonWidth, metrics.actionButtonHeight,
    startLaneIndicatorsModeText);
startLaneIndicatorsModeButton->setOnClickListener([this]() {
  context.settings.startLaneIndicatorsEnabled =
      !context.settings.startLaneIndicatorsEnabled;
  persistSettings();
});
startLaneIndicatorControls->addView(startLaneIndicatorsModeButton);
cardsColumn->addView(makeCard(
    metrics, "Start Lane Indicators",
    "Show the lanes used by the first playable chord.",
    startLaneIndicatorControls, metrics.modeCardHeight, metrics.cardsWidth));
```

- [ ] **Step 5: Run settings tests**

```bash
cmake --build cmake-build-debug --target app_settings_store_tests -j 6
./cmake-build-debug/app_settings_store_tests
```

Expected: exit `0` and print the existing settings-test success message.

- [ ] **Step 6: Commit settings and UI**

```bash
git add src/AppSettings.h src/AppSettings.cpp src/AppSettingsStore.cpp \
  src/scene/SettingsScene.h src/scene/SettingsScene.cpp \
  src/scene/SettingsSceneLayout.cpp src/scene/SettingsSceneControls.cpp \
  tests/app_settings_store_tests.cpp tests/fixtures/settings/settings-v0.json \
  tests/fixtures/settings/settings-v1.json \
  tests/fixtures/settings/legacy-full.cfg \
  tests/fixtures/profiles/legacy-settings.cfg
git commit -m "feat(settings): add start lane indicator toggle"
```

### Task 3: Triangle Geometry and Renderer Depth Ordering

**Files:**
- Create: `src/scene/play/StartLaneIndicatorGeometry.h`
- Create: `tests/start_lane_indicator_geometry_tests.cpp`
- Modify: `src/rendering/SimpleBatchRenderer.h`
- Modify: `src/rendering/SimpleBatchRenderer.cpp`
- Modify: `src/scene/play/BMSRenderer.h`
- Modify: `src/scene/play/BMSRenderer.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: renderer lane order, `laneToX`, `noteRenderWidth`, `judgeY`, `noteVisibleUpperBound`, and existing symmetric key coloring.
- Produces: `start_lane_indicator::ColorRole`, `start_lane_indicator::placeTriangle`, `SimpleBatchRenderer::addTriangle`, `BMSRenderer::setStartLaneIndicators`, and `BMSRenderer::setStartLaneIndicatorsVisible`.

- [ ] **Step 1: Add failing pure geometry tests**

Create `tests/start_lane_indicator_geometry_tests.cpp`:

```cpp
#include "../src/scene/play/StartLaneIndicatorGeometry.h"

#include <cmath>
#include <iostream>

int main() {
  using namespace start_lane_indicator;
  int failures = 0;
  auto check = [&](bool value, const char *label) {
    if (!value) {
      std::cerr << "FAIL: " << label << '\n';
      ++failures;
    }
  };
  check(colorRoleForKey(0, 7) == ColorRole::White, "7K edge is white");
  check(colorRoleForKey(1, 7) == ColorRole::Blue, "7K next key is blue");
  check(colorRoleForKey(3, 7) == ColorRole::Blue, "7K center is blue");
  check(colorRoleForScratch() == ColorRole::Red, "scratch is red");
  const auto roomy = placeTriangle(2.0f, 1.0f, 0.0f, 6.0f);
  check(std::fabs(roomy.baseY - (6.0f - kCoverGap)) < 0.0001f,
        "triangle base keeps the cover gap");
  check(roomy.tipY < roomy.baseY, "triangle points toward the judge line");
  check(!roomy.overlapsCover, "roomy triangle stays outside cover");
  const auto covered = placeTriangle(2.0f, 1.0f, 0.0f, 0.1f);
  check(covered.baseY > 0.1f, "triangle stops before crossing judge line");
  check(covered.overlapsCover, "long cover occludes the triangle");
  return failures == 0 ? 0 : 1;
}
```

Register the header-only target and add it to CTest.

- [ ] **Step 2: Run the geometry target and verify RED**

```bash
cmake --build cmake-build-debug --target start_lane_indicator_geometry_tests -j 6
```

Expected: compilation fails because `StartLaneIndicatorGeometry.h` is missing.

- [ ] **Step 3: Implement pure color and placement helpers**

Create `src/scene/play/StartLaneIndicatorGeometry.h`:

```cpp
#pragma once
#include <algorithm>
#include <cstddef>

namespace start_lane_indicator {
enum class ColorRole { White, Blue, Red };
inline constexpr float kWidthLaneRatio = 0.46f;
inline constexpr float kHeightLaneRatio = 0.40f;
inline constexpr float kCoverGap = 0.08f;
struct Triangle {
  float leftX = 0.0f;
  float rightX = 0.0f;
  float baseY = 0.0f;
  float tipX = 0.0f;
  float tipY = 0.0f;
  bool overlapsCover = false;
};
inline ColorRole colorRoleForKey(std::size_t position,
                                 std::size_t keyCount) {
  if (keyCount == 0 || position >= keyCount) return ColorRole::White;
  const std::size_t mirrored =
      std::min(position, keyCount - position - 1);
  return (mirrored & 1U) == 0 ? ColorRole::White : ColorRole::Blue;
}
inline ColorRole colorRoleForScratch() { return ColorRole::Red; }
inline Triangle placeTriangle(float laneLeftX, float laneWidth, float judgeY,
                              float coverEdgeY) {
  const float width = laneWidth * kWidthLaneRatio;
  const float height = laneWidth * kHeightLaneRatio;
  const float left = laneLeftX + (laneWidth - width) * 0.5f;
  const float desiredBase = coverEdgeY - kCoverGap;
  const float baseY = std::max(desiredBase, judgeY + height);
  return {.leftX = left,
          .rightX = left + width,
          .baseY = baseY,
          .tipX = laneLeftX + laneWidth * 0.5f,
          .tipY = baseY - height,
          .overlapsCover = baseY >= coverEdgeY};
}
} // namespace start_lane_indicator
```

- [ ] **Step 4: Add the batch triangle primitive**

Declare and implement:

```cpp
void addTriangle(float x0, float y0, float x1, float y1, float x2, float y2,
                 uint32_t color);
```

Use three `PosColorVertex` entries and indices `{0, 1, 2}`, flushing first when
the batch limits would be exceeded. Do not add a shader or texture.

- [ ] **Step 5: Wire state and draw order into BMSRenderer**

Add fields and setters:

```cpp
std::vector<int> startLaneIndicatorLanes;
bool startLaneIndicatorsVisible = false;
std::unordered_map<int, start_lane_indicator::ColorRole>
    startLaneIndicatorColorRoles;
void setStartLaneIndicators(std::vector<int> lanes);
void setStartLaneIndicatorsVisible(bool visible);
```

Build color roles beside the existing white/blue/scratch classification.
`drawStartLaneIndicators()` calls `placeTriangle(laneToX(lane),
noteRenderWidth, judgeY, noteVisibleUpperBound)` and maps roles to
`Color(255,255,255)`, `Color(40,130,255)`, and `Color(255,55,65)`.

Add `kDepthStartLaneIndicators = 300`. After lane beams and before the existing
lane-cover pass:

```cpp
simpleBatchRenderer.setSubmitView(rendering::main_view);
simpleBatchRenderer.setSubmitDepth(kDepthStartLaneIndicators);
simpleBatchRenderer.begin();
drawStartLaneIndicators();
simpleBatchRenderer.flush();
```

- [ ] **Step 6: Run geometry tests and compile the renderer**

```bash
cmake --build cmake-build-debug --target start_lane_indicator_geometry_tests main -j 6
./cmake-build-debug/start_lane_indicator_geometry_tests
```

Expected: geometry tests exit `0`; `main` compiles successfully.

- [ ] **Step 7: Commit renderer support**

```bash
git add CMakeLists.txt src/rendering/SimpleBatchRenderer.h \
  src/rendering/SimpleBatchRenderer.cpp src/scene/play/BMSRenderer.h \
  src/scene/play/BMSRenderer.cpp \
  src/scene/play/StartLaneIndicatorGeometry.h \
  tests/start_lane_indicator_geometry_tests.cpp
git commit -m "feat(rendering): draw covered start lane triangles"
```

### Task 4: Gameplay Hold, Preparation Input, and Replay Ordering

**Files:**
- Modify: `src/scene/play/GamePlayScene.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `tests/preparation_plan_tests.cpp`
- Modify: `tests/replay_repository_tests.cpp`

**Interfaces:**
- Consumes: `preparation::Plan`, BMSRenderer indicator setters, replay no-judgement events, Jukebox scheduling, and existing touch/lane-cover recording.
- Produces: one shared live/replay clock boundary, visual-only cue lane input, signed lane-cover events, and a preparation-start initial lane-cover snapshot.

- [ ] **Step 1: Extend the plan test with signed replay clock assertions**

```cpp
const auto replayAnchor = preparation::buildNormalPlan(
    chart, true, false, 0, 0, std::nullopt, normal);
CHECK(replayAnchor.playbackStartTimeMicros == -2'000'000,
      "replay preparation begins at signed chart time");
CHECK(replayAnchor.chartTimeAtRealTime(1'000'000) == -1'000'000,
      "replay events retain their signed midpoint timestamp");
```

In the replay repository round-trip fixture, use negative preparation values
for one no-judgement lane event, one touch sample, and one lane-cover event:

```cpp
replay.events.insert(replay.events.begin(),
                     {.action = ReplayEventAction::Press,
                      .lane = 1,
                      .noteTimeMicros = -1,
                      .songTimeMicros = -1'900'000,
                      .judgeTimeMicros = -1'900'000,
                      .judgement = None});
replay.touchSamples.insert(replay.touchSamples.begin(),
                           {.action = ReplayTouchAction::Down,
                            .fingerId = 9,
                            .songTimeMicros = -1'800'000,
                            .x = 0.25f,
                            .y = 0.5f});
replay.laneCoverEvents.insert(
    replay.laneCoverEvents.begin(),
    {.songTimeMicros = -2'000'000,
     .noteStartPositionPercent = 20,
     .resetVisibleTimeReference = false});
```

Assert the loaded replay preserves all three negative values exactly.

- [ ] **Step 2: Run the focused contract test**

```bash
cmake --build cmake-build-debug --target preparation_plan_tests -j 6
./cmake-build-debug/preparation_plan_tests
```

Expected: exit `0`; the pure clock contract is ready for scene wiring.

- [ ] **Step 3: Replace the scene count-in field with the shared plan**

In `GamePlayScene.h` include `PreparationPlan.h`, replace
`practiceCountInPlan` with `preparation::Plan preparationPlan`, and declare:

```cpp
[[nodiscard]] bool preparationIndicatorActive(long long rawSongTimeMicros) const;
void recordPreparationLaneEvent(ReplayEventAction action, int lane,
                                long long songTimeMicros);
```

In `reset()`:

```cpp
if (options.practiceSession != nullptr) {
  const auto &configuration = options.practiceSession->configuration();
  preparationPlan = preparation::buildPracticePlan(
      *chart, context.settings.startLaneIndicatorsEnabled,
      configuration.startMicros, configuration.endMicros,
      configuration.countInBeats, configuration.playback);
} else {
  const bool prepMetronomeEnabled = gameplay_timing::shouldApplyPrepMetronome(
      context.settings.prepMetronomeEnabled, options.practiceLeadInMicros,
      startPositionMicros);
  preparationPlan = preparation::buildNormalPlan(
      *chart, context.settings.startLaneIndicatorsEnabled,
      prepMetronomeEnabled, audioSeekPosition, startPositionMicros,
      std::nullopt, options.playback);
}
renderer->setStartLaneIndicators(preparationPlan.laneIndicator.lanes);
```

Schedule `preparationPlan.metronome` when enabled, play from
`preparationPlan.playbackStartTimeMicros`, and use its clicks in the practice
HUD.

- [ ] **Step 4: Hold judgement while keeping preparation visuals interactive**

Implement:

```cpp
bool GamePlayScene::preparationIndicatorActive(
    long long rawSongTimeMicros) const {
  return preparationPlan.indicatorVisibleAt(rawSongTimeMicros);
}
```

In `pressLane`/`releaseLane`, after pause/state guards but before
`RhythmLaneInputController`, branch while active. Update `lanePressed`, call
the matching renderer beam method with `JudgeResult(None, 0)`, update the debug
label, and record a no-judgement event. Do not search notes or play keysounds.

```cpp
void GamePlayScene::recordPreparationLaneEvent(ReplayEventAction action,
                                               int lane,
                                               long long songTimeMicros) {
  if (!shouldRecordReplay() || state == nullptr || state->isEnding) return;
  recordedReplay.events.push_back({
      .action = action,
      .lane = lane,
      .noteTimeMicros = -1,
      .songTimeMicros = songTimeMicros,
      .judgeTimeMicros = songTimeMicros,
      .judgement = None,
      .gauge = state->currentGauge,
      .gaugeType = state->gaugeType,
      .combo = state->combo,
      .score = state->getScore(),
  });
}
```

In `update()`, process replay lane and cover input before returning while the
cue is active, so gauge, misses, BPM, and timeline progression remain held.

- [ ] **Step 5: Preserve signed lane-cover ordering and renderer visibility**

Remove `std::max(0LL, songTimeMicros)` from
`appendReplayLaneCoverEvent`. Timestamp the initial lane-cover snapshot in
`beginReplayRecording()` at:

```cpp
getGameplayTimeMicros(preparationPlan.playbackStartTimeMicros)
```

In `renderScene()`:

```cpp
const long long rawSongTimeMicros = context.jukebox.getTimeMicros();
renderer->setStartLaneIndicatorsVisible(
    preparationIndicatorActive(rawSongTimeMicros));
long long gameplayTimeMicros = getGameplayTimeMicros(rawSongTimeMicros);
```

- [ ] **Step 6: Run gameplay-focused tests and compile**

```bash
cmake --build cmake-build-debug --target preparation_plan_tests \
  gameplay_practice_input_boundary_tests replay_repository_tests main -j 6
./cmake-build-debug/preparation_plan_tests
./cmake-build-debug/gameplay_practice_input_boundary_tests
./cmake-build-debug/replay_repository_tests
```

Expected: test executables exit `0`; `main` compiles successfully.

- [ ] **Step 7: Commit gameplay/replay behavior**

```bash
git add src/scene/play/GamePlayScene.h src/scene/play/GamePlayScene.cpp \
  tests/preparation_plan_tests.cpp tests/replay_repository_tests.cpp
git commit -m "feat(gameplay): run interactive start lane preparation"
```

### Task 5: Shared Metronome PCM and Replay Export Timeline

**Files:**
- Create: `src/audio/PrepMetronomeSound.h`
- Create: `src/audio/PrepMetronomeSound.cpp`
- Modify: `src/audio/CMakeLists.txt`
- Modify: `src/audio/Jukebox.cpp`
- Modify: `src/audio/ChartAudioRenderer.h`
- Modify: `src/audio/ChartAudioRenderer.cpp`
- Modify: `src/ReplayVideoExporter.cpp`
- Modify: `tests/audio_mix_tests.cpp`
- Modify: `tests/preparation_plan_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `preparation::Plan`, `PrepMetronomePlan::clicks`, replay provenance playback rate, and existing export frame/audio pipelines.
- Produces: `prep_metronome_audio::makeClick`, `chart_audio::RenderOptions::timelineStartMicros`, `chart_audio::RenderOptions::prepMetronomePlan`, and preparation-aware single/course export time mapping.

- [ ] **Step 1: Add failing shared-audio and export-clock tests**

In `tests/audio_mix_tests.cpp`:

```cpp
const auto accent = prep_metronome_audio::makeClick(true, 48'000, 2);
const auto regular = prep_metronome_audio::makeClick(false, 48'000, 2);
require(accent.size() == 4'320 && regular.size() == 4'320,
        "prep clicks are 45 ms stereo at 48 kHz");
require(*std::max_element(accent.begin(), accent.end()) >
            *std::max_element(regular.begin(), regular.end()),
        "accent click is stronger than regular click");
require(chart_audio::outputTimeMicrosFromTimelineStart(
            0, -4'000'000, audio::PlaybackRate{.percent = 100}) == 4'000'000,
        "offline audio shifts chart zero after preparation");
```

In `tests/preparation_plan_tests.cpp`:

```cpp
CHECK(metronomePlan.chartTimeAtRealTime(0) == -4'000'000,
      "export frame zero starts at the cue origin");
CHECK(metronomePlan.chartTimeAtRealTime(2'000'000) == -2'000'000,
      "export reaches the first metronome click after the cue");
CHECK(metronomePlan.chartTimeAtRealTime(4'000'000) == 0,
      "export reaches chart zero after cue and count-in");
```

Add `src/audio/PrepMetronomeSound.cpp` to `audio_mix_tests`.

- [ ] **Step 2: Run audio and plan targets and verify RED**

```bash
cmake --build cmake-build-debug --target audio_mix_tests preparation_plan_tests -j 6
```

Expected: compilation fails because the shared sound and timeline-start helper
do not exist.

- [ ] **Step 3: Extract generated metronome PCM**

Create:

```cpp
// src/audio/PrepMetronomeSound.h
#pragma once
#include <vector>
namespace prep_metronome_audio {
std::vector<short> makeClick(bool accent, int sampleRate, int channels);
}
```

Move the existing 45 ms exponentially decaying sine generator from
`Jukebox.cpp` to `PrepMetronomeSound.cpp`. Use `1760 Hz / 0.65` for accent and
`1100 Hz / 0.5` for regular, reject nonpositive rate/channel inputs, and copy
each sample to every channel. Replace Jukebox generation with calls to this
function.

- [ ] **Step 4: Make offline chart audio preparation-aware**

Add:

```cpp
inline long long outputTimeMicrosFromTimelineStart(
    long long chartTimeMicros, long long timelineStartMicros,
    audio::PlaybackRate playback) {
  return outputTimeMicros(chartTimeMicros - timelineStartMicros, playback);
}
```

Extend `RenderOptions`:

```cpp
long long timelineStartMicros = 0;
const prep_metronome::PrepMetronomePlan *prepMetronomePlan = nullptr;
```

Size the base mix through `chartEndMicros - timelineStartMicros`; mix chart,
replay, and club events at `event.timeMicros - timelineStartMicros`; and mix
generated accent/regular clicks at `click.timeMicros - timelineStartMicros`.

- [ ] **Step 5: Apply preparation to single replay export**

Build once after chart preparation:

```cpp
const preparation::Plan preparationPlan = preparation::buildNormalPlan(
    *chart, context.settings.startLaneIndicatorsEnabled,
    context.settings.prepMetronomeEnabled, 0, 0, std::nullopt,
    replay.provenance.playback);
```

Pass it to audio and video helpers. Supply:

```cpp
.timelineStartMicros = preparationPlan.playbackStartTimeMicros,
.prepMetronomePlan = preparationPlan.metronome.enabled
                         ? &preparationPlan.metronome
                         : nullptr,
```

Calculate gameplay/failure durations with
`preparationPlan.realTimeAtChartTime(chartOrFailureMicros)`. For every frame:

```cpp
const long long songTimeMicros =
    preparationPlan.chartTimeAtRealTime(videoTimeMicros);
const long long visualTimeMicros = songTimeMicros - visualOffsetMicros;
renderer.setStartLaneIndicators(preparationPlan.laneIndicator.lanes);
renderer.setStartLaneIndicatorsVisible(
    preparationPlan.indicatorVisibleAt(songTimeMicros));
```

Process signed replay and lane-cover events before rendering frame zero.

- [ ] **Step 6: Apply one plan per course replay stage**

Add `preparation::Plan preparationPlan` to `CourseReplayVideoStage` and build
it with `course_rules::kRequiredPlaybackRate` for every chart. Include its real
lead-in in stage gameplay/failure/audio durations. Map each stage-local frame:

```cpp
const long long songTimeMicros =
    stage.preparationPlan.chartTimeAtRealTime(stageFrameRealMicros);
renderer.setStartLaneIndicators(stage.preparationPlan.laneIndicator.lanes);
renderer.setStartLaneIndicatorsVisible(
    stage.preparationPlan.indicatorVisibleAt(songTimeMicros));
```

Keep result/rest durations unchanged; each concatenated audio segment begins at
its own preparation origin.

- [ ] **Step 7: Run export-related verification**

```bash
cmake --build cmake-build-debug --target audio_mix_tests \
  preparation_plan_tests replay_repository_tests main -j 6
./cmake-build-debug/audio_mix_tests
./cmake-build-debug/preparation_plan_tests
./cmake-build-debug/replay_repository_tests
```

Expected: all executables exit `0`; `main` compiles with single and course
export paths.

- [ ] **Step 8: Commit export support**

```bash
git add CMakeLists.txt src/audio/CMakeLists.txt \
  src/audio/PrepMetronomeSound.h src/audio/PrepMetronomeSound.cpp \
  src/audio/Jukebox.cpp src/audio/ChartAudioRenderer.h \
  src/audio/ChartAudioRenderer.cpp src/ReplayVideoExporter.cpp \
  tests/audio_mix_tests.cpp tests/preparation_plan_tests.cpp
git commit -m "feat(replay): export start lane preparation"
```

### Task 6: Full Verification and Manual Smoke Check

**Files:**
- Modify only if verification exposes a defect in files already listed above.

**Interfaces:**
- Consumes: every completed task.
- Produces: verified live/replay/export behavior and a clean working tree.

- [ ] **Step 1: Run the focused CTest set**

```bash
ctest --test-dir cmake-build-debug --output-on-failure \
  -R 'preparation_plan|prep_metronome|start_lane_indicator_geometry|foundation_profile_settings|gameplay_practice_input_boundary|replay_repository|foundation_av_audio_mix'
```

Expected: every selected test passes.

- [ ] **Step 2: Run the repository desktop compile check**

```bash
cmake --build cmake-build-debug --target main -j 6
```

Expected: `Built target main`.

- [ ] **Step 3: Run an interactive smoke check**

Using a chart whose opening is a multi-lane chord, verify:

1. Metronome on: triangles show for two seconds and hide on the first click.
2. Metronome off: triangles show for two seconds and hide at chart start.
3. Cue input animates beams/touches/cover without score or judgement changes.
4. The cover gap remains until a near-maximum cover occludes markers.
5. Retry and replay reproduce signed preparation inputs.
6. Practice uses only its existing count-in and first in-range chord.
7. Hiding the setting removes only the added cue phase.
8. Single and course exports include aligned cue, metronome, chart audio/video.

- [ ] **Step 4: Inspect final diff and status**

```bash
git diff --check
git status --short
git log --oneline -8
```

Expected: no whitespace errors; only intentional changes are present; all
implementation commits appear after the design and plan commits.
