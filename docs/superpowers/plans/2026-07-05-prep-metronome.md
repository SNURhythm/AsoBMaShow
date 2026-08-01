# Prep Metronome Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an optional one-measure generated metronome count-in before chart playback, using parser-derived BPM/time-signature metadata without shifting chart or replay timestamps.

**Architecture:** The parser computes `MostPrevalentBpm` and `GuessedBeatsPerMeasure` during its existing timing pass. The app builds a small `PrepMetronomePlan` from chart metadata and settings, then asks `Jukebox` to start playback at the plan's start time and schedule generated click sounds alongside normal chart audio.

**Tech Stack:** C++23, existing bms-parser-cpp amalgamation flow, CMake, existing `AudioWrapper`/`Jukebox`, existing settings UI components.

---

### Task 1: Parser Metadata

**Files:**
- Modify: `../bms-parser-cpp/src/Chart.h`
- Modify: `../bms-parser-cpp/src/Parser.cpp`
- Modify: `../bms-parser-cpp/test/main.cpp`
- Update from amalgamation output: `src/bms_parser.hpp`
- Update from amalgamation output: `src/bms_parser.cpp`

- [ ] **Step 1: Write failing parser tests**

Add a `runPrepMetadataTests()` function in `../bms-parser-cpp/test/main.cpp` before `main()`:

```cpp
int runPrepMetadataTests() {
  {
    const std::string content =
        "#TITLE prevalent-bpm\n"
        "#BPM 999\n"
        "#BPM01 120\n"
        "#BPM02 180\n"
        "#00108:01\n"
        "#00208:02\n";
    bms_parser::Chart *chart = nullptr;
    std::atomic_bool cancel = false;
    bms_parser::Parser parser;
    parser.Parse(bytesFromString(content), &chart, false, false, cancel);
    ASSERT_EQ(999.0, chart->Meta.Bpm, "prep_meta_chart_bpm: ");
    ASSERT_EQ(120.0, chart->Meta.MostPrevalentBpm,
              "prep_meta_most_prevalent_bpm: ");
    delete chart;
  }
  {
    const std::string content =
        "#TITLE guessed-beats\n"
        "#BPM 120\n"
        "#00102:0.75\n"
        "#00111:01\n"
        "#00202:0.75\n"
        "#00211:01\n";
    bms_parser::Chart *chart = nullptr;
    std::atomic_bool cancel = false;
    bms_parser::Parser parser;
    parser.Parse(bytesFromString(content), &chart, false, false, cancel);
    ASSERT_EQ(3, chart->Meta.GuessedBeatsPerMeasure,
              "prep_meta_guessed_beats_3_4: ");
    delete chart;
  }
  {
    const std::string content =
        "#TITLE default-beats\n"
        "#BPM 120\n"
        "#00111:01\n";
    bms_parser::Chart *chart = nullptr;
    std::atomic_bool cancel = false;
    bms_parser::Parser parser;
    parser.Parse(bytesFromString(content), &chart, false, false, cancel);
    ASSERT_EQ(4, chart->Meta.GuessedBeatsPerMeasure,
              "prep_meta_guessed_beats_default: ");
    delete chart;
  }
  return 0;
}
```

Call it in `main()` after `runLongNoteTypeTests()`:

```cpp
  if (const int result = runPrepMetadataTests(); result != 0) {
    return result;
  }
```

- [ ] **Step 2: Verify parser tests fail**

Run:

```bash
cd ../bms-parser-cpp && make test
```

Expected: compile fails because `ChartMeta` has no `MostPrevalentBpm` or `GuessedBeatsPerMeasure`.

- [ ] **Step 3: Implement parser metadata**

In `../bms-parser-cpp/src/Chart.h`, add:

```cpp
  double MostPrevalentBpm = 0;
  int GuessedBeatsPerMeasure = 4;
```

In `../bms-parser-cpp/src/Parser.cpp`, add helpers in the anonymous namespace:

```cpp
bool finitePositive(double value) {
  return std::isfinite(value) && value > 0.0;
}

int guessedBeatsForScale(double scale) {
  if (!finitePositive(scale)) {
    return 4;
  }
  const int beats = static_cast<int>(std::lround(scale * 4.0));
  return std::clamp(beats, 1, 16);
}

void addDuration(std::map<double, long long> &durations,
                 std::vector<double> &order, double key,
                 long long durationMicros) {
  if (!finitePositive(key) || durationMicros <= 0) {
    return;
  }
  if (durations.find(key) == durations.end()) {
    order.push_back(key);
  }
  durations[key] += durationMicros;
}

template <typename T>
T mostPrevalentValue(const std::map<T, long long> &durations,
                     const std::vector<T> &order, T fallback) {
  T best = fallback;
  long long bestDuration = 0;
  for (const T value : order) {
    const auto it = durations.find(value);
    if (it != durations.end() && it->second > bestDuration) {
      best = value;
      bestDuration = it->second;
    }
  }
  return bestDuration > 0 ? best : fallback;
}
```

Add `#include <cmath>` if missing.

Inside `Parser::Parse`, create BPM and beat-duration accumulators before the measure loop. During the existing time pass, add each interval and stop duration to BPM durations, add each measure duration to beat-count durations, then assign:

```cpp
  new_chart->Meta.MostPrevalentBpm =
      mostPrevalentValue(bpmDurations, bpmOrder, new_chart->Meta.Bpm);
  new_chart->Meta.GuessedBeatsPerMeasure =
      mostPrevalentValue(beatDurations, beatOrder, 4);
```

- [ ] **Step 4: Verify parser tests pass**

Run:

```bash
cd ../bms-parser-cpp && make test
```

Expected: parser tests pass.

- [ ] **Step 5: Verify amalgamation**

Run:

```bash
cd ../bms-parser-cpp && make test_amalgamation
```

Expected: amalgamation tests pass.

- [ ] **Step 6: Copy generated parser files**

Run:

```bash
cp ../bms-parser-cpp/build/bms_parser.hpp src/bms_parser.hpp
cp ../bms-parser-cpp/build/bms_parser.cpp src/bms_parser.cpp
```

Expected: app parser files contain the new `ChartMeta` fields.

### Task 2: Prep Plan Helper

**Files:**
- Create: `src/PrepMetronome.h`
- Create: `src/PrepMetronome.cpp`
- Create: `tests/prep_metronome_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing app-side tests**

Create `tests/prep_metronome_tests.cpp`:

```cpp
#include "../src/PrepMetronome.h"

#include <cmath>
#include <iostream>

#define ASSERT_EQ(expected, actual, label)                                     \
  if ((expected) != (actual)) {                                                \
    std::cerr << label << " expected " << (expected) << " actual "            \
              << (actual) << std::endl;                                       \
    return 1;                                                                 \
  }

#define ASSERT_TRUE(value, label)                                              \
  if (!(value)) {                                                              \
    std::cerr << label << " expected true" << std::endl;                      \
    return 1;                                                                 \
  }

int main() {
  bms_parser::ChartMeta meta;
  meta.Bpm = 120.0;
  meta.MostPrevalentBpm = 180.0;
  meta.GuessedBeatsPerMeasure = 4;

  auto plan = prep_metronome::buildPlan(meta, true, false, 0);
  ASSERT_TRUE(plan.enabled, "enabled plan");
  ASSERT_EQ(120.0, plan.bpm, "chart bpm used");
  ASSERT_EQ(4, plan.beatsPerMeasure, "beats");
  ASSERT_EQ(-2000000LL, plan.startTimeMicros, "start time");
  ASSERT_EQ(4U, plan.clicks.size(), "click count");
  ASSERT_EQ(-2000000LL, plan.clicks[0].timeMicros, "first click");
  ASSERT_TRUE(plan.clicks[0].accent, "first accent");
  ASSERT_EQ(-500000LL, plan.clicks[3].timeMicros, "last click");

  meta.Bpm = 999.0;
  plan = prep_metronome::buildPlan(meta, true, false, 3000000);
  ASSERT_TRUE(plan.enabled, "insane bpm plan");
  ASSERT_EQ(180.0, plan.bpm, "prevalent bpm used");
  ASSERT_EQ(333333LL, plan.beatIntervalMicros, "rounded beat interval");
  ASSERT_EQ(1666668LL, plan.startTimeMicros, "positive playback anchor");

  plan = prep_metronome::buildPlan(meta, false, false, 0);
  ASSERT_TRUE(!plan.enabled, "disabled setting");

  plan = prep_metronome::buildPlan(meta, true, true, 0);
  ASSERT_TRUE(!plan.enabled, "preview excluded");

  return 0;
}
```

Add a CMake test executable:

```cmake
option(ASOBMASHOW_BUILD_TESTS "Build lightweight unit tests" ON)
if (ASOBMASHOW_BUILD_TESTS)
    add_executable(prep_metronome_tests
        tests/prep_metronome_tests.cpp
        src/PrepMetronome.cpp
        src/bms_parser.cpp
    )
    target_include_directories(prep_metronome_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(prep_metronome_tests PRIVATE cxx_std_23)
endif()
```

In `src/CMakeLists.txt`, add `PrepMetronome.cpp` to the `main` target sources.

- [ ] **Step 2: Verify app-side tests fail**

Run:

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target prep_metronome_tests -j 6
```

Expected: build fails because `src/PrepMetronome.h` does not exist.

- [ ] **Step 3: Implement prep helper**

Create `src/PrepMetronome.h` with `PrepMetronomeClick`, `PrepMetronomePlan`, and:

```cpp
namespace prep_metronome {
bool isSaneBpm(double bpm);
double effectiveBpm(const bms_parser::ChartMeta &meta);
int effectiveBeatsPerMeasure(const bms_parser::ChartMeta &meta);
PrepMetronomePlan buildPlan(const bms_parser::ChartMeta &meta,
                            bool settingEnabled, bool chartPreviewPlayback,
                            long long playbackAnchorMicros);
} // namespace prep_metronome
```

Create `src/PrepMetronome.cpp` to implement sane chart BPM range 30 to 400, BPM fallback to prevalent then 120, beat fallback to 4, and click times from `playbackAnchorMicros - leadInMicros` up to one beat before the anchor. Once chart meta BPM is outside the sane range, prefer any finite positive most-prevalent BPM even if it is also outside the sane range.

- [ ] **Step 4: Verify app-side tests pass**

Run:

```bash
cmake --build cmake-build-debug --target prep_metronome_tests -j 6
./cmake-build-debug/prep_metronome_tests
```

Expected: build succeeds and executable exits 0.

### Task 3: Jukebox Generated Click Scheduling

**Files:**
- Modify: `src/audio/AudioWrapper.h`
- Modify: `src/audio/AudioWrapper.cpp`
- Modify: `src/audio/Jukebox.h`
- Modify: `src/audio/Jukebox.cpp`

- [ ] **Step 1: Add generated sound loading**

Add a public method to `AudioWrapper`:

```cpp
bool loadGeneratedSound(const path_t &path, std::vector<short> pcmData,
                        int channels, int sampleRate);
```

Implement it by calling the existing `loadDecodedSound` with a local `std::atomic_bool cancelled{false}`.

- [ ] **Step 2: Add Jukebox prep scheduling API**

Include `PrepMetronome.h` in `Jukebox.h`.

Change `schedule` to accept:

```cpp
const prep_metronome::PrepMetronomePlan *prepMetronomePlan = nullptr
```

Change `play()` to:

```cpp
void play(long long startMicros = 0);
```

- [ ] **Step 3: Generate click PCM and append prep events**

In `Jukebox.cpp`, add local generated path ids and helpers:

```cpp
const int kPrepMetronomeAccentWav = -100000;
const int kPrepMetronomeRegularWav = -100001;
const path_t kPrepMetronomeAccentPath = PATH("@prep_metronome_accent");
const path_t kPrepMetronomeRegularPath = PATH("@prep_metronome_regular");
```

Generate short stereo PCM clicks with an exponential decay, load them via `AudioWrapper::loadGeneratedSound`, add them to `wavTableAbs`, and append plan clicks to `audioList` before sorting.

- [ ] **Step 4: Make play start from arbitrary song time**

In `Jukebox::play(long long startMicros)`, replace the fixed `audio.seekClock(0)` with `audio.seekClock(startMicros)`, set `audioCursor`, `bmpCursor`, and `bmpLayerCursor` consistently with `startMicros`, and call `playOverlappingAudioAt(startMicros)` before starting the device.

- [ ] **Step 5: Compile Jukebox changes**

Run:

```bash
cmake --build cmake-build-debug --target main -j 6
```

Expected: build succeeds. If a compile error appears, fix the reported call site before continuing.

### Task 4: Settings and Gameplay Wiring

**Files:**
- Modify: `src/AppSettings.h`
- Modify: `src/AppSettings.cpp`
- Modify: `src/scene/SettingsScene.h`
- Modify: `src/scene/SettingsSceneLayout.cpp`
- Modify: `src/scene/SettingsSceneControls.cpp`
- Modify: `src/scene/play/GamePlayScene.cpp`

- [ ] **Step 1: Persist the setting**

Add `bool prepMetronomeEnabled = false;` to `AppSettings`.

Save it as:

```text
prep_metronome_enabled=0
```

Load it using `parseBool`.

- [ ] **Step 2: Add settings UI toggle**

Add `prepMetronomeModeText` and `prepMetronomeModeButton` members.

In the timing tab near `Input Keysounds`, add a button labelled through refresh as `Enabled` or `Disabled`, toggling `context.settings.prepMetronomeEnabled` and calling `persistSettings()`.

Style enabled as success and disabled as info.

- [ ] **Step 3: Wire gameplay reset**

In `GamePlayScene.cpp`, include `PrepMetronome.h`.

During `reset()`:

```cpp
const long long audioSeekPosition = getAudioSeekPositionMicros();
const auto prepPlan = prep_metronome::buildPlan(
    chart->Meta, context.settings.prepMetronomeEnabled, false,
    audioSeekPosition);
context.jukebox.schedule(*chart, options.autoKeySound && !isReplayPlayback(),
                         isCancelled, practiceKeySoundCutoff,
                         prepPlan.enabled ? &prepPlan : nullptr);
context.jukebox.play(prepPlan.enabled ? prepPlan.startTimeMicros
                                      : audioSeekPosition);
```

Remove the old post-play `seek(audioSeekPosition)` block.

- [ ] **Step 4: Verify focused tests still pass**

Run:

```bash
cmake --build cmake-build-debug --target prep_metronome_tests -j 6
./cmake-build-debug/prep_metronome_tests
```

Expected: tests pass.

### Task 5: Verification and Amalgamation

**Files:**
- Modify: `src/bms_parser.hpp`
- Modify: `src/bms_parser.cpp`
- Possibly update: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

- [ ] **Step 1: Run parser full verification**

Run:

```bash
cd ../bms-parser-cpp && make clean && make test && make test_amalgamation
```

Expected: all parser and amalgamation tests pass.

- [ ] **Step 2: Ensure app source membership**

If `src/PrepMetronome.cpp` or `src/PrepMetronome.h` are required by iOS target membership rules, add them to `membershipExceptions` in `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`.

- [ ] **Step 3: Run local desktop compile check**

Run:

```bash
cmake --build cmake-build-debug --target main -j 6
```

Expected: target `main` builds successfully.

- [ ] **Step 4: Review diff**

Run:

```bash
git diff --stat
git diff -- ../bms-parser-cpp/src/Chart.h ../bms-parser-cpp/src/Parser.cpp ../bms-parser-cpp/test/main.cpp src/PrepMetronome.h src/PrepMetronome.cpp src/audio/AudioWrapper.h src/audio/AudioWrapper.cpp src/audio/Jukebox.h src/audio/Jukebox.cpp src/AppSettings.h src/AppSettings.cpp src/scene/SettingsScene.h src/scene/SettingsSceneLayout.cpp src/scene/SettingsSceneControls.cpp src/scene/play/GamePlayScene.cpp CMakeLists.txt src/CMakeLists.txt tests/prep_metronome_tests.cpp
```

Expected: diff only contains prep-metronome feature work and generated parser amalgamation updates.
