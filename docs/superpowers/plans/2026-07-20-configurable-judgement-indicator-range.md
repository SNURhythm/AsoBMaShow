# Configurable Judgement Indicator Range Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a persisted judgement-indicator display range, defaulting to +/-180 ms and configurable from 1 through 1000 ms, while keeping raw judgement and average timing calculations unchanged.

**Architecture:** Introduce a small header-only range model that owns sanitization, formatting, normalized-position mapping, segment clipping, and raw-average accumulation. `AppSettings` persists the selected milliseconds, `JudgementIndicatorRenderer` snapshots the converted microsecond range for each render, and every live/preview/export call site passes the active profile setting through `BMSRenderer`.

**Tech Stack:** C++23, nlohmann JSON settings persistence, Yoga/SDL settings UI, bgfx batch rendering, CMake/CTest lightweight unit tests.

## Global Constraints

- The range is one symmetric whole-millisecond value; do not add separate FAST and SLOW ranges.
- The default is exactly `180` ms.
- Accept every positive whole-millisecond value through the hard maximum of `1000` ms; the renderer must never divide by zero.
- Individual judgement samples retain their raw timing difference and clamp only their final rendered position.
- The average excludes the same judgements as today, averages raw unclamped timing differences, and clamps only its final rendered position.
- Judgement-window background segments are visually intersected with the configured range; gameplay judgement windows are never mutated.
- Keep live gameplay, settings preview, single-stage replay export, and course replay export on the same setting.
- Do not change judgement, replay, score-verification, or timing-statistics semantics.
- Use `cmake --build cmake-build-debug --target main -j 6` for the repository-required desktop compile check.

---

## File Structure

- Create `src/JudgementIndicatorRange.h`: pure range rules shared by settings, UI, renderer, and tests.
- Create `tests/judgement_indicator_range_tests.cpp`: focused unit coverage for range sanitization, formatting, position mapping, raw averaging, and segment clipping.
- Modify `CMakeLists.txt`: build and register the new lightweight test target.
- Modify `src/scene/play/JudgementIndicatorRenderer.h`: store an atomic configured range and accept it in renderer configuration.
- Modify `src/scene/play/JudgementIndicatorRenderer.cpp`: remove window-derived scaling and render against one per-frame range snapshot.
- Modify `src/scene/play/BMSRenderer.h` and `src/scene/play/BMSRenderer.cpp`: expose the range in the public judgement-indicator configuration seam.
- Modify `src/AppSettings.h`, `src/AppSettings.cpp`, and `src/AppSettingsStore.cpp`: define, sanitize, load, and save the profile setting.
- Modify `tests/app_settings_store_tests.cpp`: cover defaulting, sanitization, and JSON round trips.
- Modify `src/scene/SettingsSceneShared.h`: centralize editable clamping and label formatting calls.
- Modify `src/scene/SettingsScene.h`, `src/scene/SettingsScene.cpp`, `src/scene/SettingsSceneLayout.cpp`, and `src/scene/SettingsSceneControls.cpp`: add summary and detailed range controls and keep their view state synchronized.
- Modify `src/scene/play/GamePlayScene.cpp`: apply the active range to live play.
- Modify `src/ReplayVideoExporter.cpp`: apply the active range to single-stage and course exports.

---

### Task 1: Pure range model and renderer mapping

**Files:**
- Create: `src/JudgementIndicatorRange.h`
- Create: `tests/judgement_indicator_range_tests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/scene/play/JudgementIndicatorRenderer.h`
- Modify: `src/scene/play/JudgementIndicatorRenderer.cpp`
- Modify: `src/scene/play/BMSRenderer.cpp`

**Interfaces:**
- Produces: `judgement_indicator::sanitizeStoredRangeMilliseconds(int) -> int`
- Produces: `judgement_indicator::clampEditableRangeMilliseconds(int) -> int`
- Produces: `judgement_indicator::rangeMicros(int) -> std::int64_t`
- Produces: `judgement_indicator::formatRangeLabel(int) -> std::string`
- Produces: `judgement_indicator::normalizedOffset(std::int64_t, std::int64_t) -> float`
- Produces: `judgement_indicator::clipSegment(std::int64_t, std::int64_t, std::int64_t) -> std::optional<Segment>`
- Produces: `judgement_indicator::RawAverageAccumulator`
- Consumes: existing `JudgeResult::Diff`, judgement-window maps, and renderer layout geometry.

- [ ] **Step 1: Register a test target and write the failing pure-model tests**

Add this target beside the other lightweight gameplay/model targets in `CMakeLists.txt` and register it with `asobmashow_register_test`:

```cmake
    add_executable(judgement_indicator_range_tests
        tests/judgement_indicator_range_tests.cpp
    )
    target_include_directories(judgement_indicator_range_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(judgement_indicator_range_tests PRIVATE cxx_std_23)
```

```cmake
    asobmashow_register_test(judgement_indicator_range_tests)
```

Create `tests/judgement_indicator_range_tests.cpp`:

```cpp
#include "../src/JudgementIndicatorRange.h"

#include <cmath>
#include <iostream>
#include <string>

namespace {
int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void expectNear(float actual, float expected, const std::string &message) {
  expect(std::abs(actual - expected) < 0.0001f, message);
}

void testRangeRules() {
  using namespace judgement_indicator;
  expect(sanitizeStoredRangeMilliseconds(-1) == 180,
         "negative stored range uses the default");
  expect(sanitizeStoredRangeMilliseconds(0) == 180,
         "zero stored range uses the default");
  expect(sanitizeStoredRangeMilliseconds(1) == 1,
         "one millisecond is accepted");
  expect(sanitizeStoredRangeMilliseconds(1000) == 1000,
         "the hard cap is accepted");
  expect(sanitizeStoredRangeMilliseconds(1001) == 1000,
         "stored range is capped at 1000 ms");
  expect(clampEditableRangeMilliseconds(-10) == 1,
         "interactive edits clamp to one millisecond");
  expect(clampEditableRangeMilliseconds(1001) == 1000,
         "interactive edits clamp to the hard cap");
  expect(rangeMicros(180) == 180000,
         "milliseconds convert to microseconds");
  expect(rangeMicros(0) == 180000,
         "invalid renderer input uses the default range");
  expect(formatRangeLabel(180) == "+/-180 ms",
         "range label displays a symmetric extent");
}

void testPositionMapping() {
  using namespace judgement_indicator;
  expectNear(normalizedOffset(0, 180000), 0.0f, "zero remains centered");
  expectNear(normalizedOffset(-180000, 180000), -1.0f,
             "early boundary maps left");
  expectNear(normalizedOffset(180000, 180000), 1.0f,
             "late boundary maps right");
  expectNear(normalizedOffset(-300000, 180000), -1.0f,
             "early outlier clamps left");
  expectNear(normalizedOffset(300000, 180000), 1.0f,
             "late outlier clamps right");
}

void testRawAverageThenPositionClamp() {
  using namespace judgement_indicator;
  RawAverageAccumulator average;
  average.add(300000);
  average.add(0);
  expect(average.count() == 2, "average tracks included samples");
  expect(average.value() == 150000,
         "average uses raw values before display clamping");
  expectNear(normalizedOffset(average.value(), 180000), 5.0f / 6.0f,
             "raw average maps inside the configured range");
  average.add(300000);
  expect(average.value() == 200000,
         "raw average can exceed the configured range");
  expectNear(normalizedOffset(average.value(), 180000), 1.0f,
             "only the final average marker position clamps");
}

void testSegmentClipping() {
  using namespace judgement_indicator;
  const auto partial = clipSegment(-200000, -120000, 180000);
  expect(partial.has_value(), "partially visible BAD segment remains");
  expect(partial && partial->startMicros == -180000 &&
             partial->endMicros == -120000,
         "partially visible segment clips at the bar edge");
  expect(!clipSegment(-400000, -200000, 180000).has_value(),
         "fully hidden early segment is omitted");
  expect(!clipSegment(200000, 400000, 180000).has_value(),
         "fully hidden late segment is omitted");
}
} // namespace

int main() {
  testRangeRules();
  testPositionMapping();
  testRawAverageThenPositionClamp();
  testSegmentClipping();
  if (failures != 0) {
    std::cerr << failures << " judgement indicator range assertion(s) failed\n";
    return 1;
  }
  std::cout << "judgement indicator range tests passed\n";
  return 0;
}
```

- [ ] **Step 2: Run the test target to verify the missing model fails**

Run:

```bash
cmake --build cmake-build-debug --target judgement_indicator_range_tests -j 6
```

Expected: compilation fails because `src/JudgementIndicatorRange.h` does not exist.

- [ ] **Step 3: Implement the complete header-only range model**

Create `src/JudgementIndicatorRange.h`:

```cpp
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace judgement_indicator {
inline constexpr int kDefaultRangeMilliseconds = 180;
inline constexpr int kMinRangeMilliseconds = 1;
inline constexpr int kMaxRangeMilliseconds = 1000;

[[nodiscard]] constexpr int
sanitizeStoredRangeMilliseconds(int value) noexcept {
  if (value <= 0) {
    return kDefaultRangeMilliseconds;
  }
  return std::min(value, kMaxRangeMilliseconds);
}

[[nodiscard]] constexpr int
clampEditableRangeMilliseconds(int value) noexcept {
  return std::clamp(value, kMinRangeMilliseconds, kMaxRangeMilliseconds);
}

[[nodiscard]] constexpr std::int64_t rangeMicros(int milliseconds) noexcept {
  return static_cast<std::int64_t>(
             sanitizeStoredRangeMilliseconds(milliseconds)) *
         1000LL;
}

[[nodiscard]] inline std::string formatRangeLabel(int milliseconds) {
  return "+/-" +
         std::to_string(clampEditableRangeMilliseconds(milliseconds)) +
         " ms";
}

[[nodiscard]] constexpr float normalizedOffset(
    std::int64_t diffMicros, std::int64_t displayRangeMicros) noexcept {
  const std::int64_t safeRange = std::max<std::int64_t>(1, displayRangeMicros);
  const float raw = static_cast<float>(diffMicros) /
                    static_cast<float>(safeRange);
  return std::clamp(raw, -1.0f, 1.0f);
}

struct Segment {
  std::int64_t startMicros = 0;
  std::int64_t endMicros = 0;
};

[[nodiscard]] constexpr std::optional<Segment>
clipSegment(std::int64_t startMicros, std::int64_t endMicros,
            std::int64_t displayRangeMicros) noexcept {
  const std::int64_t safeRange = std::max<std::int64_t>(1, displayRangeMicros);
  const Segment clipped{
      .startMicros = std::max(startMicros, -safeRange),
      .endMicros = std::min(endMicros, safeRange),
  };
  if (clipped.endMicros <= clipped.startMicros) {
    return std::nullopt;
  }
  return clipped;
}

class RawAverageAccumulator {
public:
  void add(std::int64_t diffMicros) noexcept {
    sumMicros_ += diffMicros;
    ++count_;
  }

  [[nodiscard]] std::size_t count() const noexcept { return count_; }

  [[nodiscard]] std::int64_t value() const noexcept {
    return count_ == 0 ? 0
                       : sumMicros_ / static_cast<std::int64_t>(count_);
  }

private:
  std::int64_t sumMicros_ = 0;
  std::size_t count_ = 0;
};
} // namespace judgement_indicator
```

- [ ] **Step 4: Run the pure-model tests to verify they pass**

Run:

```bash
cmake --build cmake-build-debug --target judgement_indicator_range_tests -j 6 && ./cmake-build-debug/judgement_indicator_range_tests
```

Expected: `judgement indicator range tests passed`.

- [ ] **Step 5: Replace window-derived renderer scaling with configured scaling**

In `src/scene/play/JudgementIndicatorRenderer.h`, include the model, add the range argument, make the range atomic, and pass the per-frame range explicitly through mapping and clipping:

```cpp
#include "../../JudgementIndicatorRange.h"
```

```cpp
  void configure(bool enabled, float y, float widthScale, bool hudMode,
                 int rangeMilliseconds);
```

```cpp
  [[nodiscard]] float offsetToX(long long diffMicros,
                                long long displayRangeMicros,
                                const Layout &layout) const;
  void drawSegment(rendering::SimpleBatchRenderer &batch,
                   long long startMicros, long long endMicros,
                   long long displayRangeMicros, const Layout &layout,
                   float barY, Color color) const;
```

```cpp
  std::atomic<long long> rangeMicros{
      judgement_indicator::rangeMicros(
          judgement_indicator::kDefaultRangeMilliseconds)};
```

In `src/scene/play/JudgementIndicatorRenderer.cpp`:

- remove `kDefaultRangeMicros`, `indicatorRangeFromWindows`, and the constructor initializer that derives `rangeMicros` from KPoor;
- store `judgement_indicator::rangeMicros(rangeMilliseconds)` in `configure`;
- load `const long long currentRangeMicros = rangeMicros.load(std::memory_order_relaxed);` once near the start of `render`;
- replace `averageSum` and `averageCount` with `judgement_indicator::RawAverageAccumulator average`, call `average.add(sample.diffMicros)` only after the existing KPoor exclusion, and stop at `average.count() >= kAverageSampleCount`;
- pass `currentRangeMicros` to every `drawSegment` and `offsetToX` call;
- compute the marker position with the raw average: `offsetToX(average.value(), currentRangeMicros, indicatorLayout)`.

The constructor and range update become:

```cpp
JudgementIndicatorRenderer::JudgementIndicatorRenderer(
    const std::map<Judgement, std::pair<long long, long long>> &timingWindows)
    : timingWindows(timingWindows) {}
```

```cpp
  rangeMicros.store(judgement_indicator::rangeMicros(rangeMilliseconds),
                    std::memory_order_relaxed);
```

Use this complete average loop in `render`:

```cpp
  judgement_indicator::RawAverageAccumulator average;
  for (uint64_t sequence = nextSequence; sequence > firstSequence;) {
    --sequence;
    Sample sample;
    if (!readSample(sequence, sample)) {
      continue;
    }
    if (sample.judgement == Kpoor) {
      continue;
    }
    average.add(sample.diffMicros);
    if (average.count() >= kAverageSampleCount) {
      break;
    }
  }
```

Replace the background calls with this complete configured-range sequence:

```cpp
  drawSegment(batch, -currentRangeMicros, timingWindowEarly(Bad),
              currentRangeMicros, indicatorLayout, barY,
              judgementColor(Poor, 118));
  drawSegment(batch, timingWindowEarly(Bad), timingWindowEarly(Good),
              currentRangeMicros, indicatorLayout, barY,
              judgementColor(Bad, 126));
  drawSegment(batch, timingWindowEarly(Good), timingWindowEarly(Great),
              currentRangeMicros, indicatorLayout, barY,
              judgementColor(Good, 134));
  drawSegment(batch, timingWindowEarly(Great), timingWindowEarly(PGreat),
              currentRangeMicros, indicatorLayout, barY,
              judgementColor(Great, 142));
  drawSegment(batch, timingWindowEarly(PGreat), timingWindowLate(PGreat),
              currentRangeMicros, indicatorLayout, barY,
              judgementColor(PGreat, 170));
  drawSegment(batch, timingWindowLate(PGreat), timingWindowLate(Great),
              currentRangeMicros, indicatorLayout, barY,
              judgementColor(Great, 142));
  drawSegment(batch, timingWindowLate(Great), timingWindowLate(Good),
              currentRangeMicros, indicatorLayout, barY,
              judgementColor(Good, 134));
  drawSegment(batch, timingWindowLate(Good), timingWindowLate(Bad),
              currentRangeMicros, indicatorLayout, barY,
              judgementColor(Bad, 126));
  drawSegment(batch, timingWindowLate(Bad), currentRangeMicros,
              currentRangeMicros, indicatorLayout, barY,
              judgementColor(Poor, 118));
```

Replace the average render guard with:

```cpp
  if (average.count() > 0) {
    const float x =
        offsetToX(average.value(), currentRangeMicros, indicatorLayout);
    const float averageWidth = indicatorLayout.markerWidth * 2.4f;
    const float averagePad = currentHudMode ? 3.0f : 0.04f;
    addRect(batch, averageWidth,
            indicatorLayout.markerHeight + averagePad * 2.0f,
            x - averageWidth * 0.5f, markerY - averagePad,
            Color(0, 0, 0, 205));
    addRect(batch, averageWidth * 0.44f,
            indicatorLayout.markerHeight + averagePad,
            x - averageWidth * 0.22f, markerY - averagePad * 0.5f,
            Color(255, 245, 140, 240));
  }
```

Use this mapping implementation:

```cpp
float JudgementIndicatorRenderer::offsetToX(
    long long diffMicros, long long displayRangeMicros,
    const Layout &layout) const {
  const float normalized = judgement_indicator::normalizedOffset(
      diffMicros, displayRangeMicros);
  return layout.x + layout.width * 0.5f + normalized * layout.width * 0.5f;
}
```

Use this segment implementation:

```cpp
void JudgementIndicatorRenderer::drawSegment(
    rendering::SimpleBatchRenderer &batch, long long startMicros,
    long long endMicros, long long displayRangeMicros, const Layout &layout,
    float barY, Color color) const {
  const auto clipped = judgement_indicator::clipSegment(
      startMicros, endMicros, displayRangeMicros);
  if (!clipped.has_value()) {
    return;
  }
  const float x0 = offsetToX(clipped->startMicros, displayRangeMicros, layout);
  const float x1 = offsetToX(clipped->endMicros, displayRangeMicros, layout);
  if (x1 <= x0) {
    return;
  }
  addRect(batch, x1 - x0, layout.barHeight, x0, barY, color);
}
```

Until Task 3 adds the public setting parameter, keep `BMSRenderer::setJudgementIndicatorConfig` source-compatible by supplying the default in `src/scene/play/BMSRenderer.cpp`:

```cpp
  judgementIndicator.configure(
      enabled, y, widthScale, hudMode,
      judgement_indicator::kDefaultRangeMilliseconds);
```

- [ ] **Step 6: Build the model test and desktop application**

Run:

```bash
cmake --build cmake-build-debug --target judgement_indicator_range_tests main -j 6
./cmake-build-debug/judgement_indicator_range_tests
```

Expected: both targets build and the range test prints `judgement indicator range tests passed`.

- [ ] **Step 7: Commit the pure model and renderer behavior**

```bash
git add CMakeLists.txt src/JudgementIndicatorRange.h \
  src/scene/play/JudgementIndicatorRenderer.h \
  src/scene/play/JudgementIndicatorRenderer.cpp \
  src/scene/play/BMSRenderer.cpp \
  tests/judgement_indicator_range_tests.cpp
git commit -m "feat: clamp judgement indicator display range"
```

---

### Task 2: Persist the profile range setting

**Files:**
- Modify: `src/AppSettings.h`
- Modify: `src/AppSettings.cpp`
- Modify: `src/AppSettingsStore.cpp`
- Modify: `tests/app_settings_store_tests.cpp`

**Interfaces:**
- Consumes: Task 1's range constants and `sanitizeStoredRangeMilliseconds`.
- Produces: `AppSettings::judgementIndicatorRangeMilliseconds` with a sanitized domain of 1 through 1000 and a default of 180.
- Produces: JSON key `judgementIndicatorRangeMilliseconds`.

- [ ] **Step 1: Write failing persistence and sanitization assertions**

In `testJsonRoundTripIncludesAudioAndVideo`, set and assert the non-default value:

```cpp
  expected.judgementIndicatorRangeMilliseconds = 333;
```

```cpp
  expect(readFile(path).find(
             "\"judgementIndicatorRangeMilliseconds\": 333") !=
             std::string::npos,
         "saved JSON includes the judgement indicator range");
```

Add this test before `testVersionFixturesAndNoRewrite`:

```cpp
void testJudgementIndicatorRangeDefaultsAndSanitization() {
  AppSettings defaults;
  defaults.sanitize();
  expect(defaults.judgementIndicatorRangeMilliseconds == 180,
         "judgement indicator range defaults to 180 ms");

  AppSettings invalid;
  invalid.judgementIndicatorRangeMilliseconds = 0;
  invalid.sanitize();
  expect(invalid.judgementIndicatorRangeMilliseconds == 180,
         "non-positive stored range uses the default");

  AppSettings excessive;
  excessive.judgementIndicatorRangeMilliseconds = 1001;
  excessive.sanitize();
  expect(excessive.judgementIndicatorRangeMilliseconds == 1000,
         "stored range clamps to the 1000 ms hard cap");

  TempDirectory temp;
  const auto path = temp.path() / "legacy-range-settings.json";
  writeFile(path, R"({"schemaVersion":3,"audioOffsetMs":12})");
  const auto legacy = AppSettingsStore::Load(path);
  expect(legacy.status == AppSettingsLoadStatus::Loaded,
         "settings written before range configuration still load");
  expect(legacy.settings.judgementIndicatorRangeMilliseconds == 180,
         "settings without the range field use 180 ms");

  const auto malformedPath = temp.path() / "malformed-range-settings.json";
  writeFile(malformedPath,
            R"({"schemaVersion":3,"judgementIndicatorRangeMilliseconds":"wide"})");
  const auto malformed = AppSettingsStore::Load(malformedPath);
  expect(malformed.status == AppSettingsLoadStatus::Loaded,
         "malformed range does not invalidate the settings document");
  expect(malformed.settings.judgementIndicatorRangeMilliseconds == 180,
         "malformed range falls back to 180 ms");
  expect(hasDiagnostic(malformed.diagnostics,
                       "judgementIndicatorRangeMilliseconds",
                       "expected integer"),
         "malformed range emits a setting diagnostic");
}
```

Call it from `main()` immediately after `testJsonRoundTripIncludesAudioAndVideo()`:

```cpp
  testJudgementIndicatorRangeDefaultsAndSanitization();
```

- [ ] **Step 2: Run the settings test to verify the missing field fails**

Run:

```bash
cmake --build cmake-build-debug --target app_settings_store_tests -j 6
```

Expected: compilation fails because `AppSettings` has no `judgementIndicatorRangeMilliseconds` member.

- [ ] **Step 3: Add, sanitize, load, and save the setting**

In `src/AppSettings.h`, include `JudgementIndicatorRange.h`, alias its public limits, and add the field beside the other indicator settings:

```cpp
#include "JudgementIndicatorRange.h"
```

```cpp
  static constexpr int kMinJudgementIndicatorRangeMilliseconds =
      judgement_indicator::kMinRangeMilliseconds;
  static constexpr int kMaxJudgementIndicatorRangeMilliseconds =
      judgement_indicator::kMaxRangeMilliseconds;
  static constexpr int kDefaultJudgementIndicatorRangeMilliseconds =
      judgement_indicator::kDefaultRangeMilliseconds;
```

```cpp
  int judgementIndicatorRangeMilliseconds =
      kDefaultJudgementIndicatorRangeMilliseconds;
```

In `AppSettings::sanitize()` in `src/AppSettings.cpp`, place this beside the existing indicator Y/width sanitization:

```cpp
  judgementIndicatorRangeMilliseconds =
      judgement_indicator::sanitizeStoredRangeMilliseconds(
          judgementIndicatorRangeMilliseconds);
```

In `settingsToJson` in `src/AppSettingsStore.cpp`, add:

```cpp
      {"judgementIndicatorRangeMilliseconds",
       settings.judgementIndicatorRangeMilliseconds},
```

In `settingsFromJson`, add:

```cpp
  readValue(document, "judgementIndicatorRangeMilliseconds",
            settings.judgementIndicatorRangeMilliseconds, diagnostics);
```

Do not bump `schemaVersion`: the missing field intentionally defaults through the in-class initializer.

- [ ] **Step 4: Run settings and range tests**

Run:

```bash
cmake --build cmake-build-debug --target app_settings_store_tests judgement_indicator_range_tests -j 6
./cmake-build-debug/app_settings_store_tests
./cmake-build-debug/judgement_indicator_range_tests
```

Expected: both test executables report that their assertions passed.

- [ ] **Step 5: Commit persistence**

```bash
git add src/AppSettings.h src/AppSettings.cpp src/AppSettingsStore.cpp \
  tests/app_settings_store_tests.cpp
git commit -m "feat: persist judgement indicator range"
```

---

### Task 3: Add settings controls and propagate the range everywhere

**Files:**
- Modify: `src/scene/SettingsSceneShared.h`
- Modify: `src/scene/SettingsScene.h`
- Modify: `src/scene/SettingsScene.cpp`
- Modify: `src/scene/SettingsSceneLayout.cpp`
- Modify: `src/scene/SettingsSceneControls.cpp`
- Modify: `src/scene/play/BMSRenderer.h`
- Modify: `src/scene/play/BMSRenderer.cpp`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/ReplayVideoExporter.cpp`

**Interfaces:**
- Consumes: `AppSettings::judgementIndicatorRangeMilliseconds` and Task 1's editable clamp/format functions.
- Produces: `BMSRenderer::setJudgementIndicatorConfig(bool, float, float, bool, int)`.
- Produces: summary value `+/-N ms` and a detailed numeric editor with -10, -1, +1, +10, and Reset actions.

- [ ] **Step 1: Add shared UI conversions and scene state declarations**

In `src/scene/SettingsSceneShared.h`, add:

```cpp
static int clampJudgementIndicatorRangeMilliseconds(int value) {
  return judgement_indicator::clampEditableRangeMilliseconds(value);
}

static std::string formatJudgementIndicatorRangeLabel(int value) {
  return judgement_indicator::formatRangeLabel(value);
}
```

In `src/scene/SettingsScene.h`, add the view pointers beside the existing indicator Y/width members:

```cpp
  TextView *summaryJudgementIndicatorRangeValueText = nullptr;
  TextInputBox *judgementIndicatorRangeInput = nullptr;
```

Add the controller declarations beside the other indicator sync/commit methods:

```cpp
  void syncJudgementIndicatorRangeInputText(bool force = false);
  void commitJudgementIndicatorRangeInput();
```

Set both new pointers to `nullptr` in every existing view-reset block in `src/scene/SettingsScene.cpp` and `src/scene/SettingsSceneLayout.cpp`.

- [ ] **Step 2: Add compact-summary and detailed controls**

In the compact preview/summary indicator section of `src/scene/SettingsSceneLayout.cpp`, insert an `Indicator Range` summary row after `Indicator Width`:

```cpp
    previewControls->addView(makeSummaryRow(
        metrics, "Indicator Range",
        &summaryJudgementIndicatorRangeValueText));
    auto updateIndicatorRange = [this](int deltaMilliseconds) {
      context.settings.judgementIndicatorRangeMilliseconds =
          clampJudgementIndicatorRangeMilliseconds(
              context.settings.judgementIndicatorRangeMilliseconds +
              deltaMilliseconds);
      persistSettings();
    };
    auto *minusIndicatorRange =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-10 ms");
    minusIndicatorRange->setOnClickListener(
        [updateIndicatorRange]() { updateIndicatorRange(-10); });
    auto *plusIndicatorRange =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+10 ms");
    plusIndicatorRange->setOnClickListener(
        [updateIndicatorRange]() { updateIndicatorRange(10); });
    auto *resetIndicatorRange = makeResetButton(metrics);
    resetIndicatorRange->setOnClickListener([this]() {
      context.settings.judgementIndicatorRangeMilliseconds =
          AppSettings::kDefaultJudgementIndicatorRangeMilliseconds;
      persistSettings();
    });
    previewControls->addView(makePreviewStepRow(
        minusIndicatorRange, plusIndicatorRange, resetIndicatorRange));
```

In the detailed Judgement Indicator card, insert this block after the Width controls:

```cpp
  judgementIndicatorControls->addView(makeText(
      "Range (ms)", metrics.bodyTextSize, ui_theme::textSecondary()));
  auto *judgementIndicatorRangeControls = new View();
  judgementIndicatorRangeControls->setFlexDirection(FlexDirection::Row);
  judgementIndicatorRangeControls->setFlexWrap(YGWrapWrap);
  judgementIndicatorRangeControls->setGap(metrics.compact ? 8.0f : 12.0f);
  judgementIndicatorRangeControls->setAlignItems(YGAlignFlexStart);
  auto updateJudgementIndicatorRange = [this](int deltaMilliseconds) {
    context.settings.judgementIndicatorRangeMilliseconds =
        clampJudgementIndicatorRangeMilliseconds(
            context.settings.judgementIndicatorRangeMilliseconds +
            deltaMilliseconds);
    persistSettings();
    syncJudgementIndicatorRangeInputText(true);
  };

  auto *minusIndicatorRangeLarge =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-10");
  minusIndicatorRangeLarge->setOnClickListener(
      [updateJudgementIndicatorRange]() {
        updateJudgementIndicatorRange(-10);
      });
  judgementIndicatorRangeControls->addView(minusIndicatorRangeLarge);
  auto *minusIndicatorRangeSmall =
      makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-1");
  minusIndicatorRangeSmall->setOnClickListener(
      [updateJudgementIndicatorRange]() {
        updateJudgementIndicatorRange(-1);
      });
  judgementIndicatorRangeControls->addView(minusIndicatorRangeSmall);
  judgementIndicatorRangeInput = makeNumericInput(metrics);
  judgementIndicatorRangeInput->onEditingFinished(
      [this](const std::string &) { commitJudgementIndicatorRangeInput(); });
  judgementIndicatorRangeControls->addView(
      makeInputFrame(metrics, judgementIndicatorRangeInput));
  auto *plusIndicatorRangeSmall =
      makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+1");
  plusIndicatorRangeSmall->setOnClickListener(
      [updateJudgementIndicatorRange]() {
        updateJudgementIndicatorRange(1);
      });
  judgementIndicatorRangeControls->addView(plusIndicatorRangeSmall);
  auto *plusIndicatorRangeLarge =
      makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+10");
  plusIndicatorRangeLarge->setOnClickListener(
      [updateJudgementIndicatorRange]() {
        updateJudgementIndicatorRange(10);
      });
  judgementIndicatorRangeControls->addView(plusIndicatorRangeLarge);
  auto *resetIndicatorRange = makeResetButton(metrics);
  resetIndicatorRange->setOnClickListener([this]() {
    context.settings.judgementIndicatorRangeMilliseconds =
        AppSettings::kDefaultJudgementIndicatorRangeMilliseconds;
    persistSettings();
    syncJudgementIndicatorRangeInputText(true);
  });
  judgementIndicatorRangeControls->addView(resetIndicatorRange);
  judgementIndicatorControls->addView(judgementIndicatorRangeControls);
```

Update the card description to:

```cpp
      metrics, "Judgement Indicator",
      "Set position, size, timing range, and render mode.",
```

- [ ] **Step 3: Synchronize labels and numeric input**

In `SettingsScene::syncControlLabels` in `src/scene/SettingsSceneControls.cpp`, compute and assign the summary label:

```cpp
  const std::string judgementIndicatorRangeLabel =
      formatJudgementIndicatorRangeLabel(
          context.settings.judgementIndicatorRangeMilliseconds);
```

```cpp
  if (summaryJudgementIndicatorRangeValueText != nullptr) {
    summaryJudgementIndicatorRangeValueText->setText(
        judgementIndicatorRangeLabel);
  }
```

Call the new input synchronizer beside the existing indicator synchronizers:

```cpp
  syncJudgementIndicatorRangeInputText();
```

Implement both methods:

```cpp
void SettingsScene::syncJudgementIndicatorRangeInputText(bool force) {
  if (judgementIndicatorRangeInput == nullptr) {
    return;
  }
  if (!force && judgementIndicatorRangeInput->getSelected()) {
    return;
  }
  judgementIndicatorRangeInput->setEditingText(std::to_string(
      context.settings.judgementIndicatorRangeMilliseconds));
}

void SettingsScene::commitJudgementIndicatorRangeInput() {
  if (judgementIndicatorRangeInput == nullptr) {
    return;
  }
  const std::string rawText = judgementIndicatorRangeInput->getText();
  if (rawText.empty()) {
    syncJudgementIndicatorRangeInputText(true);
    return;
  }
  try {
    context.settings.judgementIndicatorRangeMilliseconds =
        clampJudgementIndicatorRangeMilliseconds(std::stoi(rawText));
    persistSettings();
    syncJudgementIndicatorRangeInputText(true);
  } catch (const std::exception &) {
    syncJudgementIndicatorRangeInputText(true);
  }
}
```

- [ ] **Step 4: Make the renderer API require and apply the configured range**

Change `BMSRenderer`'s declaration and definition to:

```cpp
  void setJudgementIndicatorConfig(bool enabled, float y, float widthScale,
                                   bool hudMode, int rangeMilliseconds);
```

```cpp
void BMSRenderer::setJudgementIndicatorConfig(
    bool enabled, float y, float widthScale, bool hudMode,
    int rangeMilliseconds) {
  judgementIndicator.configure(enabled, y, widthScale, hudMode,
                               rangeMilliseconds);
}
```

Pass `context.settings.judgementIndicatorRangeMilliseconds` as the fifth argument in both live call sites:

```cpp
      context.settings.judgementIndicatorRenderMode ==
          AppSettings::JudgementIndicatorRenderMode::Hud2D,
      context.settings.judgementIndicatorRangeMilliseconds);
```

Those call sites are:

- `src/scene/play/GamePlayScene.cpp` live gameplay initialization;
- `src/scene/SettingsScene.cpp` settings preview refresh.

Pass `settings.judgementIndicatorRangeMilliseconds` as the fifth argument in both `src/ReplayVideoExporter.cpp` call sites:

```cpp
                                       judgementIndicatorHudMode,
                                       settings.judgementIndicatorRangeMilliseconds);
```

The two exporter call sites cover single-stage replay export and course-stage replay export. With no default argument on `BMSRenderer::setJudgementIndicatorConfig`, the desktop build is the compile-time audit that every presentation path supplies the range.

- [ ] **Step 5: Run focused tests and the required desktop build**

Run:

```bash
cmake --build cmake-build-debug --target \
  judgement_indicator_range_tests app_settings_store_tests main -j 6
./cmake-build-debug/judgement_indicator_range_tests
./cmake-build-debug/app_settings_store_tests
```

Expected: all three targets build; both test executables report success.

- [ ] **Step 6: Run the full configured CTest suite**

Run:

```bash
ctest --test-dir cmake-build-debug --output-on-failure
```

Expected: every configured test passes. If a pre-existing unrelated failure appears, capture its exact test name and output before deciding whether it is in scope.

- [ ] **Step 7: Review the final diff and commit the UI/call-site integration**

Run:

```bash
git diff --check
git status --short
```

Expected: `git diff --check` emits no output, and status lists only the files named in Task 3.

Commit:

```bash
git add src/scene/SettingsSceneShared.h src/scene/SettingsScene.h \
  src/scene/SettingsScene.cpp src/scene/SettingsSceneLayout.cpp \
  src/scene/SettingsSceneControls.cpp src/scene/play/BMSRenderer.h \
  src/scene/play/BMSRenderer.cpp src/scene/play/GamePlayScene.cpp \
  src/ReplayVideoExporter.cpp
git commit -m "feat: configure judgement indicator timing range"
```

---

## Final Verification Checklist

- [ ] A missing stored field resolves to 180 ms without a schema-version bump.
- [ ] The UI accepts every integer from 1 through 1000 and caps larger edits at 1000.
- [ ] Reset restores 180 ms in both summary and detailed settings flows.
- [ ] LR2 KPoor no longer forces a +/-1000 ms indicator unless the user selects 1000 ms.
- [ ] A +300 ms individual judgement at a 180 ms range pins to the right edge.
- [ ] Raw +300 ms and 0 ms samples average to +150 ms before position mapping.
- [ ] Background window colors stop cleanly at the configured edges.
- [ ] Live play, preview, single replay export, and course replay export all pass the profile range.
- [ ] `cmake --build cmake-build-debug --target main -j 6` succeeds.
- [ ] `ctest --test-dir cmake-build-debug --output-on-failure` succeeds.
