# Practice and Timing Analysis Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build section-loop practice, chart-specific presets, rule overrides, normal/practice playback rate, replay-backed timing analytics, and result/replay section handoff.

**Architecture:** Shared practice models and pure services carry one original-chart-time contract through Chart Viewer, gameplay, Result Scene, and replay playback. The audio callback applies pitch-changing rate while its authoritative clock remains in chart time; profile-scoped JSON stores presets, and existing replay provenance stores effective play conditions. Existing scenes remain navigation entry points, with focused views and models extracted from their large source files.

**Tech Stack:** C++23, SDL2, miniaudio callback backend, bgfx, Yoga UI, nlohmann JSON, SQLite provenance JSON, CMake/CTest, Objective-C++ iOS target membership, Android CMake.

## Global Constraints

- Practice positions, replay event times, and section heatmap boundaries use original-chart microseconds.
- Count-in is 0-16 whole beats, default 4, and runs before every loop and restart.
- Playback rate is 50%-200% in 5% steps, default 100%; only pitch-shifting is enabled initially.
- Judge-window scale is 25%-200% in 5% steps, default 100%, with a tagged model that can add custom windows later.
- Starting gauge is optional and clamps to 0%-100% in 1% steps.
- Non-100% normal play is modified/assisted and caps the clear mark at Assisted Easy.
- Practice remains score-ineligible; abandoned partial loops never enter headline analytics.
- Per-chart files live at `profiles/<id>/practice/<sha256>.json` and use atomic replacement.
- Old scores/replays default to 100% PitchShift and 100% judge scale.
- Unsupported playback modes must fail visibly rather than silently changing mode or rate.
- All new `src` files must be added to `membershipExceptions` in `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`.
- Tests are registered with CTest; do not add or run Yoga's library tests.
- Each task ends in its own feature commit; do not squash unrelated tasks together.

---

## File and responsibility map

- `src/audio/PlaybackRate.h`: common playback mode/rate value, validation, and chart/real-time conversion.
- `src/practice/PracticeConfiguration.h/.cpp`: serializable practice rules and range sanitization.
- `src/practice/PracticePresetStore.h/.cpp`: lazy per-chart profile JSON store.
- `src/practice/PracticeSession.h/.cpp`: loop-attempt lifecycle and immutable configuration snapshot.
- `src/practice/PracticeAnalytics.h/.cpp`: pure replay-to-statistics transformation.
- `src/practice/PracticeLaunchRequest.h/.cpp`: result/replay-to-Chart-Viewer handoff value and range merge.
- `src/practice/PracticeResultModel.h/.cpp`: pure aggregate/attempt analytics presentation state.
- `src/scene/PracticePanelView.h/.cpp`: Chart Viewer practice controls and named-preset commands.
- `src/scene/PracticeAnalyticsView.h/.cpp`: histogram/lane/heatmap rendering and section selection.
- `src/scene/ChartViewerScene.*`: chart markers, panel ownership, preset loading, and practice launch.
- `src/scene/play/GamePlayStartOptions.h`: playback/practice session options and provenance capture.
- `src/scene/play/GamePlayScene.*`: loop boundary, restart, gauge/judge application, and result handoff.
- `src/scene/play/Judge.*`: deterministic effective-window scaling.
- `src/audio/AudioMix.*`: rate-aware scheduled activation and fractional PCM consumption.
- `src/audio/AudioWrapper.*`: authoritative rate-scaled chart clock.
- `src/audio/Jukebox.*`: playback-rate session API, seek/restore, and video clock propagation.
- `src/ScoreProvenance.*`: backward-compatible playback/judge provenance.
- `src/ReplayData.h`, `src/ReplayDBHelper.cpp`: existing replay event and provenance persistence consumed by the new services.
- `src/AppSettings.*`, `src/AppSettingsStore.cpp`: normal-play rate selection.
- `src/scene/MainMenuScene.cpp`: normal-play rate controls and start options.
- `src/scene/ResultScene.*`: practice summary, analytics, and section action.
- `src/PlayerProfile.*`, `src/PlayerProfileManager.cpp`, `src/ProfileArchive.cpp`, `src/ProfileExportStaging.cpp`: practice-directory portability and validation.
- `src/practice/CMakeLists.txt`, `src/scene/CMakeLists.txt`, `src/audio/CMakeLists.txt`, `src/CMakeLists.txt`, root `CMakeLists.txt`: production sources and focused CTest targets.

### Task 1: Practice configuration, presets, and profile portability

**Files:**
- Create: `src/audio/PlaybackRate.h`
- Create: `src/practice/PracticeConfiguration.h`
- Create: `src/practice/PracticeConfiguration.cpp`
- Create: `src/practice/PracticePresetStore.h`
- Create: `src/practice/PracticePresetStore.cpp`
- Create: `src/practice/CMakeLists.txt`
- Create: `tests/practice_configuration_tests.cpp`
- Create: `tests/practice_preset_store_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `src/PlayerProfile.h`
- Modify: `src/PlayerProfileManager.cpp`
- Modify: `src/ProfileArchive.cpp`
- Modify: `src/ProfileArchive.h`
- Modify: `src/ProfileExportStaging.cpp`
- Modify: `tests/player_profile_manager_tests.cpp`
- Modify: `tests/profile_archive_tests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Produces: `audio::PlaybackRate`, `practice::Configuration`, `practice::sanitize`, and `practice::PresetStore` used by all later tasks.
- Produces: `PlayerProfilePaths::practiceDirectory` included by profile lifecycle operations.

- [ ] **Step 1: Write failing configuration and preset tests**

Add cases that compile against these exact interfaces:

```cpp
audio::PlaybackRate rate{.percent = 75,
                         .mode = audio::PlaybackMode::PitchShift};
assert(rate.valid());
assert(rate.chartMicrosFromReal(20000) == 15000);
assert(rate.realMicrosFromChart(15000) == 20000);

practice::Configuration input{
    .chartSha256 = "0123456789abcdef0123456789abcdef"
                   "0123456789abcdef0123456789abcdef",
    .startMicros = 8'000'000,
    .endMicros = 2'000'000,
    .loop = true,
    .countInBeats = 99,
    .startingGaugePercent = 120,
    .judge = {.kind = practice::JudgeOverrideKind::Scale,
              .scalePercent = 17},
    .playback = {.percent = 73,
                 .mode = audio::PlaybackMode::PitchShift},
};
const auto sanitized = practice::sanitize(input, 10'000'000);
assert(sanitized.configuration.startMicros == 2'000'000);
assert(sanitized.configuration.endMicros == 8'000'000);
assert(sanitized.configuration.countInBeats == 16);
assert(sanitized.configuration.startingGaugePercent == 100);
assert(sanitized.configuration.judge.scalePercent == 25);
assert(sanitized.configuration.playback.percent == 75);
```

In `practice_preset_store_tests.cpp`, save a last-used configuration and two
named presets, reload them, reject a mismatching hash, inject a failed atomic
replace operation, and verify the previous file remains readable.

- [ ] **Step 2: Register and run the tests to verify failure**

Add `practice_configuration_tests` and `practice_preset_store_tests` targets,
including `AtomicFile.cpp` and `VersionedJson.cpp` for the store target, and
register both through `asobmashow_register_test`.

Run:

```bash
cmake --build cmake-build-debug --target practice_configuration_tests practice_preset_store_tests -j 6
```

Expected: compilation fails because the new headers and implementations do not
exist yet.

- [ ] **Step 3: Implement the values and store**

Define the common values exactly as follows:

```cpp
namespace audio {
enum class PlaybackMode : std::uint8_t { PitchShift = 0, TimeStretch = 1 };
struct PlaybackRate {
  int percent = 100;
  PlaybackMode mode = PlaybackMode::PitchShift;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] bool neutral() const noexcept { return percent == 100; }
  [[nodiscard]] long long chartMicrosFromReal(long long value) const noexcept;
  [[nodiscard]] long long realMicrosFromChart(long long value) const noexcept;
  bool operator==(const PlaybackRate &) const = default;
};
}

namespace practice {
enum class JudgeOverrideKind : std::uint8_t { Scale = 0, Custom = 1 };
struct JudgeOverride {
  JudgeOverrideKind kind = JudgeOverrideKind::Scale;
  int scalePercent = 100;
  bool operator==(const JudgeOverride &) const = default;
};
struct Configuration {
  std::string chartSha256;
  long long startMicros = 0;
  long long endMicros = 0;
  bool loop = false;
  int countInBeats = 4;
  GaugeType gaugeType = GaugeType::Normal;
  std::optional<int> startingGaugePercent;
  JudgeOverride judge;
  audio::PlaybackRate playback;
  bool operator==(const Configuration &) const = default;
};
struct SanitizedConfiguration {
  Configuration configuration;
  std::vector<std::string> diagnostics;
  [[nodiscard]] bool playable() const noexcept;
};
SanitizedConfiguration sanitize(Configuration value,
                                long long chartEndMicros);
}
```

`PresetStore::load(chartSha256, chartEndMicros)` returns neutral defaults on a
missing/malformed file plus diagnostics; `saveLastUsed`, `saveNamed`,
`renameNamed`, and `deleteNamed` serialize schema version 1 through
`versioned_json::saveAtomic`. Normalize SHA-256 to lowercase and use only the
64-character normalized hash as a filename.

```cpp
struct NamedPreset {
  std::string id;
  std::string name;
  Configuration configuration;
};
struct PresetData {
  Configuration lastUsed;
  std::vector<NamedPreset> named;
};
struct PresetLoadResult {
  PresetData data;
  versioned_json::LoadStatus status;
  std::vector<std::string> diagnostics;
};
class PresetStore {
public:
  explicit PresetStore(std::filesystem::path practiceDirectory,
                       const atomic_file::Operations *operations = nullptr);
  PresetLoadResult load(std::string_view chartSha256,
                        long long chartEndMicros) const;
  bool saveLastUsed(std::string_view chartSha256,
                    const Configuration &, std::string &error);
  std::optional<std::string> saveNamed(std::string_view chartSha256,
                                       std::string name,
                                       const Configuration &,
                                       std::string &error);
  bool updateNamed(std::string_view chartSha256, std::string_view presetId,
                   const Configuration &, std::string &error);
  bool renameNamed(std::string_view chartSha256, std::string_view presetId,
                   std::string name, std::string &error);
  bool deleteNamed(std::string_view chartSha256, std::string_view presetId,
                   std::string &error);
};
```

- [ ] **Step 4: Integrate the optional practice directory with profiles**

Add:

```cpp
struct PlayerProfilePaths {
  // existing fields
  std::filesystem::path practiceDirectory;
};
```

Set it to `root / "practice"`. Profile creation creates the directory;
duplication copies validated regular `*.json` files; export/import staging
includes it; deletion remains recursive through the profile root. Validation
allows a missing directory for legacy profiles, rejects links/non-regular
entries, rejects non-hash filenames, and bounds each file to 1 MiB without
parsing every preset during routine activation. Add the new `.cpp` files to
`src/practice/CMakeLists.txt`, `src/CMakeLists.txt`, and iOS membership
exceptions. Raise `ProfileArchiveManifest::kFormatVersion` to 2, add
`practiceSchemaVersion = 1`, and continue accepting v1 archives as profiles
with an empty practice directory.

- [ ] **Step 5: Run focused and profile tests**

```bash
cmake --build cmake-build-debug --target practice_configuration_tests practice_preset_store_tests player_profile_manager_tests profile_archive_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R 'practice_(configuration|preset_store)|foundation_profile_(manager|archive)'
```

Expected: all selected tests pass.

- [ ] **Step 6: Commit the feature**

```bash
git add CMakeLists.txt src/audio/PlaybackRate.h src/practice src/CMakeLists.txt src/PlayerProfile.h src/PlayerProfileManager.cpp src/ProfileArchive.h src/ProfileArchive.cpp src/ProfileExportStaging.cpp tests/practice_configuration_tests.cpp tests/practice_preset_store_tests.cpp tests/player_profile_manager_tests.cpp tests/profile_archive_tests.cpp ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat: add per-chart practice presets"
```

### Task 2: Playback and judge provenance

**Files:**
- Modify: `src/ScoreProvenance.h`
- Modify: `src/ScoreProvenance.cpp`
- Modify: `src/scene/play/GamePlayStartOptions.h`
- Modify: `tests/score_provenance_tests.cpp`
- Modify: `tests/replay_db_helper_tests.cpp`

**Interfaces:**
- Consumes: `audio::PlaybackRate` from Task 1.
- Produces: `ScoreProvenance::playback`, `ScoreProvenance::judgeWindowScalePercent`, `ScoreProvenance::startingGaugePercent`, and replay-start restoration used by Tasks 5-10.

- [ ] **Step 1: Write failing backward-compatibility tests**

Add assertions that a new provenance round trip preserves 75% PitchShift and
80% judge scale, marks the record Modified, and migrates schema 2 JSON to the
neutral defaults:

```cpp
auto input = sampleInput();
input.playback = {.percent = 75,
                  .mode = audio::PlaybackMode::PitchShift};
input.judgeWindowScalePercent = 80;
input.startingGaugePercent = 37;
const ScoreProvenance modified = makeScoreProvenance(input);
assert(modified.eligibility == ScoreEligibility::Modified);
assert(modified.playback.percent == 75);
assert(modified.judgeWindowScalePercent == 80);

auto legacyRoot = nlohmann::json::parse(
    serializeScoreProvenance(sampleVerifiedProvenance()));
legacyRoot["schemaVersion"] = 2;
legacyRoot.erase("playback");
legacyRoot.erase("judgeWindowScalePercent");
legacyRoot.erase("startingGaugePercent");
const auto migrated =
    deserializeScoreProvenance(legacyRoot.dump(), error);
assert(migrated->playback == audio::PlaybackRate{});
assert(migrated->judgeWindowScalePercent == 100);
assert(!migrated->startingGaugePercent.has_value());
```

Include `../yoga/lib/nlohmann/json.hpp` in the test for the migration edit.

Add a replay DB round trip proving the provenance JSON survives save/load
without adding duplicate SQL columns.

- [ ] **Step 2: Run tests to verify failure**

```bash
cmake --build cmake-build-debug --target score_provenance_tests replay_db_helper_tests -j 6
```

Expected: compilation fails on the missing provenance fields.

- [ ] **Step 3: Extend provenance schema and play-start capture**

Raise `ScoreProvenance::kSchemaVersion` to 3 and add:

```cpp
audio::PlaybackRate playback;
int judgeWindowScalePercent = 100;
std::optional<int> startingGaugePercent;
```

Add the same inputs to `ScoreProvenanceBuildInput`. Serialize mode names as
`"pitch-shift"` and `"time-stretch"`. Schema 1/2 migration supplies neutral
defaults. Provenance validation accepts both defined mode values and rejects
out-of-range percentages; the play-start/audio boundary, not serialization,
rejects currently unavailable TimeStretch. Eligibility becomes Modified when
rate, judge scale, or starting gauge is non-neutral.

Add `audio::PlaybackRate playback`, `int judgeWindowScalePercent = 100`, and
`std::optional<int> startingGaugePercent` to `StartOptions`; replay creation
captures them in provenance, and replay start copies them back from
`ReplayData::provenance`.

- [ ] **Step 4: Run focused tests**

```bash
ctest --test-dir cmake-build-debug --output-on-failure -R 'foundation_provenance_contract|replay_db_helper_tests'
```

Expected: both tests pass, including legacy JSON migration.

- [ ] **Step 5: Commit the feature**

```bash
git add src/ScoreProvenance.h src/ScoreProvenance.cpp src/scene/play/GamePlayStartOptions.h tests/score_provenance_tests.cpp tests/replay_db_helper_tests.cpp
git commit -m "feat: record practice playback provenance"
```

### Task 3: Chart Viewer range editor and Practice panel

**Files:**
- Create: `src/scene/PracticePanelView.h`
- Create: `src/scene/PracticePanelView.cpp`
- Create: `tests/chart_practice_range_tests.cpp`
- Modify: `src/scene/ChartViewerScene.h`
- Modify: `src/scene/ChartViewerScene.cpp`
- Modify: `src/scene/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: `practice::Configuration` and `practice::PresetStore`.
- Produces: dual marker APIs on `ChartCanvasView` and a validated configuration callback for gameplay.

- [ ] **Step 1: Extract and test range selection behavior**

Add pure marker behavior to `chart_practice_range_tests.cpp`:

```cpp
practice::RangeSelection selection{.startMicros = 1'000'000,
                                   .endMicros = 5'000'000,
                                   .active = practice::Marker::End};
selection.placeActiveMarker(500'000, 8'000'000);
assert(selection.startMicros == 500'000);
assert(selection.endMicros == 1'000'000);
assert(selection.active == practice::Marker::Start);
```

Run the new target and expect compilation failure on `RangeSelection`.

- [ ] **Step 2: Add dual marker rendering and interaction**

Replace `ChartCanvasView::selectedTimeMicros` with a range selection while
retaining `getSelectedTimeMicros()` as the active marker compatibility helper.
Add these values to `PracticeConfiguration.h`:

```cpp
enum class Marker : std::uint8_t { Start = 0, End = 1 };
struct RangeSelection {
  long long startMicros = 0;
  long long endMicros = 0;
  Marker active = Marker::Start;
  void placeActiveMarker(long long timeMicros, long long chartEndMicros);
  bool operator==(const RangeSelection &) const = default;
};
```

Expose:

```cpp
void setPracticeRange(const practice::RangeSelection &range);
[[nodiscard]] practice::RangeSelection getPracticeRange() const;
void setActivePracticeMarker(practice::Marker marker);
void setPracticeRangeListener(
    std::function<void(const practice::RangeSelection &)> listener);
```

Render start cyan, end amber, and the selected span with a translucent teal
fill clipped per chart column. A tap moves the active marker and applies the
crossing/swap behavior tested above.

- [ ] **Step 3: Implement the focused Practice panel**

`PracticePanelView` accepts a configuration, named preset list, and callbacks:

```cpp
struct PracticePanelCallbacks {
  std::function<void(const practice::Configuration &)> onChanged;
  std::function<void()> onStart;
  std::function<void(std::string)> onSaveAs;
  std::function<void(std::string)> onRename;
  std::function<void()> onUpdateNamed;
  std::function<void()> onDeleteNamed;
};
```

Use existing `Button`, `DropDown`, `TextInputBox`, and the global overlay portal
for popup controls. TimeStretch is shown disabled with an unavailable label.
Start is disabled when `sanitize(...).playable()` is false. Chart Viewer lazily
loads the hash store after parsing, autosaves last-used changes, and routes the
existing Practice button through the panel's current configuration.

- [ ] **Step 4: Build and run range tests**

```bash
cmake --build cmake-build-debug --target chart_practice_range_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R 'chart_practice_range_tests|practice_(configuration|preset_store)'
```

Expected: tests and desktop build pass.

- [ ] **Step 5: Commit the feature**

```bash
git add CMakeLists.txt src/practice/PracticeConfiguration.h src/practice/PracticeConfiguration.cpp src/scene/PracticePanelView.h src/scene/PracticePanelView.cpp src/scene/ChartViewerScene.h src/scene/ChartViewerScene.cpp src/scene/CMakeLists.txt tests/chart_practice_range_tests.cpp ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat: add chart practice range editor"
```

### Task 4: Practice session, looping, count-in, and instant restart

**Files:**
- Create: `src/practice/PracticeSession.h`
- Create: `src/practice/PracticeSession.cpp`
- Create: `tests/practice_session_tests.cpp`
- Modify: `src/PrepMetronome.h`
- Modify: `src/PrepMetronome.cpp`
- Modify: `src/scene/play/GamePlayStartOptions.h`
- Modify: `src/scene/play/GamePlayScene.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/scene/ResultScene.h`
- Modify: `src/scene/ResultScene.cpp`
- Modify: `src/practice/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: validated `practice::Configuration`.
- Produces: `practice::Session`, attempt finalization, and `ResultPracticeOptions::session` used by analytics.

- [ ] **Step 1: Write failing session transition tests**

Exercise the exact lifecycle:

```cpp
practice::Session session(configuration);
session.beginAttempt();
ReplayData completedReplay;
completedReplay.events.push_back({.action = ReplayEventAction::Press,
                                  .lane = 1,
                                  .noteTimeMicros = 1'000'000,
                                  .songTimeMicros = 1'000'000,
                                  .judgeTimeMicros = 1'005'000,
                                  .judgement = Great,
                                  .diffMicros = 5'000});
session.completeAttempt(std::move(completedReplay));
assert(session.completedAttempts().size() == 1);
assert(session.loopNumber() == 2);
session.beginAttempt();
session.abandonAttempt();
assert(session.completedAttempts().size() == 1);
assert(session.abandonedAttemptCount() == 1);
assert(session.shouldLoop());
```

Add count-in plan assertions for four clicks before a 10-second marker at the
tempo active at that marker, including BPM changes and a 75% playback rate.

- [ ] **Step 2: Run tests to verify failure**

```bash
cmake --build cmake-build-debug --target practice_session_tests prep_metronome_tests -j 6
```

Expected: compilation fails on the missing session/count-in APIs.

- [ ] **Step 3: Implement session and count-in planning**

Define:

```cpp
class Session {
public:
  explicit Session(Configuration configuration);
  void beginAttempt();
  void completeAttempt(ReplayData replay);
  void abandonAttempt();
  [[nodiscard]] bool shouldLoop() const;
  [[nodiscard]] int loopNumber() const;
  [[nodiscard]] const std::vector<ReplayData> &completedAttempts() const;
  [[nodiscard]] std::size_t abandonedAttemptCount() const;
  [[nodiscard]] const Configuration &configuration() const;
};
```

Extend prep planning with
`buildPracticeCountInPlan(chart, startMicros, countInBeats, playback)`. Its
click times remain chart-time values; their real spacing derives from the
rate-scaled audio clock.

- [ ] **Step 4: Wire gameplay boundaries and restart**

Add `std::shared_ptr<practice::Session> practiceSession` to `StartOptions` and
derive `practiceMode` from its presence while keeping legacy construction
working during this task. `GamePlayScene::update()` checks original chart time
against `configuration.endMicros` before the all-measures completion path.
On completion it calls `finishReplayRecording()` exactly once, appends the
attempt, then either calls the existing resource-reusing `reset()` for another
count-in or opens Result Scene.

Retry calls `abandonAttempt()` before reset. The pause menu labels/actions are
Resume, Restart Section, Finish Practice, and Exit Without Summary. Result
practice options carry the shared session rather than copying individual
start fields. Add a persistent touch Restart button and HUD text showing loop
number, formatted start/end, playback rate, and remaining count-in beats; hide
both for non-practice gameplay. Preserve the current practice-ghost callback by
publishing the latest completed attempt, never an abandoned partial attempt.

- [ ] **Step 5: Run focused tests and desktop build**

```bash
cmake --build cmake-build-debug --target practice_session_tests prep_metronome_tests logical_gameplay_input_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R 'practice_session_tests|prep_metronome_tests|foundation_input_gameplay'
```

Expected: all selected tests pass.

- [ ] **Step 6: Commit the feature**

```bash
git add CMakeLists.txt src/practice/PracticeSession.h src/practice/PracticeSession.cpp src/practice/CMakeLists.txt src/PrepMetronome.h src/PrepMetronome.cpp src/scene/play/GamePlayStartOptions.h src/scene/play/GamePlayScene.h src/scene/play/GamePlayScene.cpp src/scene/ResultScene.h src/scene/ResultScene.cpp tests/practice_session_tests.cpp tests/prep_metronome_tests.cpp ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat: loop practice sections with count-in"
```

### Task 5: Starting gauge and scalable judge windows

**Files:**
- Create: `tests/practice_rule_override_tests.cpp`
- Modify: `src/scene/play/Judge.h`
- Modify: `src/scene/play/Judge.cpp`
- Modify: `src/scene/play/RhythmState.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/scene/play/GamePlayStartOptions.h`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `practice::JudgeOverride`, playback rate, and optional starting gauge.
- Produces: `Judge::applyWindowScale`, `RhythmState::setStartingGaugePercent`, and deterministic replay restoration.

- [ ] **Step 1: Write failing override tests**

```cpp
 bms_parser::Chart chart;
 chart.Meta.TotalNotes = 100;
Judge judge(1);
judge.applyCourseJudgementConstraint(CourseJudgementConstraint::NoGood);
judge.applyWindowScale(75, 80);
assert(judge.timingWindows.at(PGreat) ==
       std::pair<long long, long long>(-6000, 6000));

RhythmState state(&chart, false);
state.configureGauge(GaugeType::Hard, false);
state.setStartingGaugePercent(37);
assert(state.currentGauge == 37.0f);
assert(state.gaugeValues[gaugeTypeIndex(GaugeType::Hard)] == 37.0f);
```

Also verify auto-shift snapshots receive the configured amount consistently
and a saved practice replay restores its starting amount and exact windows.

- [ ] **Step 2: Run the test to verify failure**

```bash
cmake --build cmake-build-debug --target practice_rule_override_tests -j 6
```

Expected: compilation fails on both missing methods.

- [ ] **Step 3: Implement override ordering and capture**

`Judge::applyWindowScale(playbackRatePercent, judgeScalePercent)` runs after
rank and course constraints. Scale signed early/late values with rounded
integer arithmetic by `playbackRatePercent * judgeScalePercent / 10000`.
Construct the effective judge before capturing provenance.

`RhythmState::setStartingGaugePercent(int)` clamps and updates `currentGauge`
plus every admitted gauge value when auto shift is enabled; otherwise it
updates only the selected gauge. Apply the existing
`StartOptions::startingGaugePercent` after `configureGauge` and before attempt
recording; replay start restores it from provenance.

- [ ] **Step 4: Run focused tests**

```bash
ctest --test-dir cmake-build-debug --output-on-failure -R 'practice_rule_override_tests|foundation_provenance_contract|replay_db_helper_tests'
```

Expected: all selected tests pass.

- [ ] **Step 5: Commit the feature**

```bash
git add CMakeLists.txt src/scene/play/Judge.h src/scene/play/Judge.cpp src/scene/play/RhythmState.h src/scene/play/GamePlayScene.cpp src/scene/play/GamePlayStartOptions.h tests/practice_rule_override_tests.cpp tests/score_provenance_tests.cpp tests/replay_db_helper_tests.cpp
git commit -m "feat: add practice gauge and judge overrides"
```

### Task 6: Pitch-shifting playback rate and authoritative clock

**Files:**
- Modify: `src/audio/AudioMix.h`
- Modify: `src/audio/AudioMix.cpp`
- Modify: `src/audio/AudioWrapper.h`
- Modify: `src/audio/AudioWrapper.cpp`
- Modify: `src/audio/Jukebox.h`
- Modify: `src/audio/Jukebox.cpp`
- Modify: `src/audio/AudioDeviceManager.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `tests/audio_mix_tests.cpp`
- Modify: `tests/audio_wrapper_lifecycle_tests.cpp`
- Modify: `tests/jukebox_restore_tests.cpp`

**Interfaces:**
- Consumes: `audio::PlaybackRate` from Task 1.
- Produces: `Jukebox::setPlaybackRate`, rate-aware seek/snapshot/restore, and chart-time audio clock.

- [ ] **Step 1: Write failing mixer and clock tests**

Add mixer tests using a mono ramp. At 200%, four output frames consume source
frames 0, 2, 4, 6; at 50%, linear interpolation produces positions 0, 0.5, 1,
1.5. Add clock tests proving 48,000 output frames advance chart time by
500,000 microseconds at 50% and 2,000,000 at 200%. Jukebox restore must retain
rate, position, and paused state.

- [ ] **Step 2: Run tests to verify failure**

```bash
cmake --build cmake-build-debug --target audio_mix_tests audio_wrapper_lifecycle_tests jukebox_restore_tests -j 6
```

Expected: tests fail because playback rate is not accepted by mixer or clock.

- [ ] **Step 3: Make active sounds rate-aware**

Replace integer-only active positions with fixed-point positions:

```cpp
struct PlayingSound {
  SoundData *soundData = nullptr;
  audio::Bus bus = audio::Bus::Bgm;
  std::uint64_t sourceFrameQ32 = 0;
  std::uint32_t outputOffsetFrames = 0;
};

void MixActiveSounds(AudioCallbackState &, std::span<float>,
                     std::uint32_t frameCount, int outputChannels,
                     float bgmGain, float keysoundGain,
                     int playbackRatePercent);
```

Use Q32 linear interpolation between adjacent PCM frames and advance by
`percent / 100` source frames per output frame. Convert scheduled chart-time
deltas to output offsets with the inverse rate. Keep seek offsets and sound
durations in original chart time.

- [ ] **Step 4: Scale the authoritative clock and Jukebox lifecycle**

Add an atomic rate percent to `AudioWrapper::UserData`. `beginAudioClockBuffer`
advances chart microseconds by output frames times rate; wall interpolation in
`getTimeMicros()` applies the same rate and clamps to the scaled buffer end.
Expose:

```cpp
bool AudioWrapper::setPlaybackRate(audio::PlaybackRate rate,
                                   std::string &errorMessage);
audio::PlaybackRate AudioWrapper::playbackRate() const;
bool Jukebox::setPlaybackRate(audio::PlaybackRate rate,
                              std::string &errorMessage);
```

Only allow rate mutation while playback is stopped. Extend `PlaybackSnapshot`
with the rate and restore it before position/play state. TimeStretch returns a
clear unsupported-mode error. Keep Jukebox's existing synchronization of the
shared video stopwatch to the audio chart clock, and add a restore test proving
the BGA timeline receives the scaled chart position.

- [ ] **Step 5: Wire gameplay and run audio tests**

Set rate before `schedule()`/`play()` in gameplay reset, restore 100% on scene
cleanup and non-game music entry, and abort before replay recording if rate
application fails.

```bash
ctest --test-dir cmake-build-debug --output-on-failure -R 'foundation_av_audio_mix|foundation_av_audio_wrapper_lifecycle|foundation_av_jukebox_restore'
cmake --build cmake-build-debug --target main -j 6
```

Expected: selected tests and desktop build pass.

- [ ] **Step 6: Commit the feature**

```bash
git add src/audio/AudioMix.h src/audio/AudioMix.cpp src/audio/AudioWrapper.h src/audio/AudioWrapper.cpp src/audio/Jukebox.h src/audio/Jukebox.cpp src/audio/AudioDeviceManager.h src/scene/play/GamePlayScene.cpp tests/audio_mix_tests.cpp tests/audio_wrapper_lifecycle_tests.cpp tests/jukebox_restore_tests.cpp
git commit -m "feat: add pitch-shifting playback rate"
```

### Task 7: Normal-play controls and Assisted Easy clear cap

**Files:**
- Modify: `src/AppSettings.h`
- Modify: `src/AppSettings.cpp`
- Modify: `src/AppSettingsStore.cpp`
- Modify: `src/scene/MainMenuScene.h`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/scene/play/RhythmState.h`
- Modify: `tests/app_settings_store_tests.cpp`
- Modify: `tests/score_provenance_tests.cpp`
- Modify: `tests/logical_gameplay_input_tests.cpp`

**Interfaces:**
- Consumes: rate-aware gameplay and provenance.
- Produces: persistent normal-play rate selection and clear-cap policy.

- [ ] **Step 1: Write failing settings and clear-policy tests**

Round-trip `selectedPlaybackRatePercent = 75` and
`selectedPlaybackMode = PitchShift`, sanitize 73 to 75 and 250 to 200, then
assert:

```cpp
assert(clear_policy::assistClearRequired(audio::PlaybackRate{75}));
assert(!clear_policy::assistClearRequired(audio::PlaybackRate{100}));
assert(clear_policy::capRankForPlayback(
           kClearTypeHardClearRank, audio::PlaybackRate{75}) ==
       kClearTypeAssistedEasyClearRank);
```

- [ ] **Step 2: Run focused tests to verify failure**

```bash
cmake --build cmake-build-debug --target app_settings_store_tests score_provenance_tests logical_gameplay_input_tests -j 6
```

Expected: compilation fails on the settings and clear-policy symbols.

- [ ] **Step 3: Persist and expose normal-play rate**

Add neutral fields to `AppSettings`, JSON read/write, sanitize, equality tests,
and legacy defaults. Add rate decrement/increment and mode dropdown controls to
the existing Main Menu play-option surface. Show `Assisted Easy maximum` beside
any non-100% rate and keep TimeStretch disabled.

Copy the selected rate into normal `StartOptions`. Replay start ignores current
settings and uses replay provenance. Practice uses its configuration value.

- [ ] **Step 4: Apply assisted-clear behavior**

Centralize the rule in a small `clear_policy` helper in `RhythmState.h` and set
`state->setAssistClearMark(existingAssist || !playback.neutral())`. Ensure the
full-combo post-processing path cannot promote a rate-assisted attempt above
Assisted Easy. Keep score/replay writes enabled with Modified eligibility.

- [ ] **Step 5: Run tests and build**

```bash
ctest --test-dir cmake-build-debug --output-on-failure -R 'foundation_profile_settings|foundation_provenance_contract|foundation_input_gameplay'
cmake --build cmake-build-debug --target main -j 6
```

Expected: selected tests and desktop build pass.

- [ ] **Step 6: Commit the feature**

```bash
git add src/AppSettings.h src/AppSettings.cpp src/AppSettingsStore.cpp src/scene/MainMenuScene.h src/scene/MainMenuScene.cpp src/scene/play/GamePlayScene.cpp src/scene/play/RhythmState.h tests/app_settings_store_tests.cpp tests/score_provenance_tests.cpp tests/logical_gameplay_input_tests.cpp
git commit -m "feat: add assisted normal-play rate controls"
```

### Task 8: Pure replay timing analytics

**Files:**
- Create: `src/practice/PracticeAnalytics.h`
- Create: `src/practice/PracticeAnalytics.cpp`
- Create: `tests/practice_analytics_tests.cpp`
- Modify: `src/practice/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: chart measures, replay events, and recorded playback provenance.
- Produces: `practice::analyze` output used by Result Scene and analytics view.

- [ ] **Step 1: Write failing analytics tests**

Build replays containing early/late presses, an LN release, miss, mine, and
unjudged input. Verify 5 ms histogram buckets, signed mean, population standard
deviation, median, lane bias, miss counts, measure boundaries, and conversion
of a -15 ms chart delta at 75% to -20 ms real time. Verify attempts with
different rate or judge scale produce separate compatibility groups.

- [ ] **Step 2: Run the test to verify failure**

```bash
cmake --build cmake-build-debug --target practice_analytics_tests -j 6
```

Expected: compilation fails because `PracticeAnalytics` does not exist.

- [ ] **Step 3: Implement the pure analytics contract**

Define:

```cpp
struct TimingStatistics {
  std::size_t samples = 0;
  std::size_t early = 0;
  std::size_t late = 0;
  std::size_t misses = 0;
  std::optional<double> meanMillis;
  std::optional<double> standardDeviationMillis;
  std::optional<double> medianMillis;
};
struct HistogramBin { int lowerMillis; int upperMillis; std::size_t count; };
struct LaneAnalysis { int lane; TimingStatistics timing; };
struct SectionAnalysis {
  int firstMeasure;
  int lastMeasure;
  long long startMicros;
  long long endMicros;
  TimingStatistics timing;
  double badMissRate = 0.0;
};
struct Analysis {
  TimingStatistics overall;
  std::vector<HistogramBin> histogram;
  std::vector<LaneAnalysis> lanes;
  std::vector<SectionAnalysis> sections;
};
struct TimingConditions {
  audio::PlaybackRate playback;
  int judgeWindowScalePercent = 100;
  std::vector<JudgeWindowProvenance> effectiveJudgeWindows;
  bool operator==(const TimingConditions &) const = default;
};
struct AnalysisGroup {
  TimingConditions conditions;
  std::vector<std::size_t> attemptIndices;
  Analysis aggregate;
};
Analysis analyze(const bms_parser::Chart &, const ReplayData &);
std::vector<AnalysisGroup>
analyzeCompatibleAttempts(const bms_parser::Chart &,
                          std::span<const ReplayData>);
```

Samples include judged Press/Release events with note identity and exclude Miss,
Mine, None, and Kpoor from timing values while counting misses by lane/measure.
Return `std::nullopt` statistics for empty samples, never NaN.

- [ ] **Step 4: Run analytics tests**

```bash
ctest --test-dir cmake-build-debug --output-on-failure -R practice_analytics_tests
```

Expected: all analytics cases pass.

- [ ] **Step 5: Commit the feature**

```bash
git add CMakeLists.txt src/practice/PracticeAnalytics.h src/practice/PracticeAnalytics.cpp src/practice/CMakeLists.txt tests/practice_analytics_tests.cpp ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat: add replay timing analytics"
```

### Task 9: Practice result analytics and multi-attempt summary

**Files:**
- Create: `src/scene/PracticeAnalyticsView.h`
- Create: `src/scene/PracticeAnalyticsView.cpp`
- Create: `src/practice/PracticeResultModel.h`
- Create: `src/practice/PracticeResultModel.cpp`
- Create: `tests/practice_result_model_tests.cpp`
- Modify: `src/scene/ResultScene.h`
- Modify: `src/scene/ResultScene.cpp`
- Modify: `src/scene/CMakeLists.txt`
- Modify: `src/practice/CMakeLists.txt`
- Modify: `src/ReplayVideoExporter.cpp`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: `practice::Session` and `practice::Analysis`.
- Produces: aggregate/attempt result tabs and selected section callback.

- [ ] **Step 1: Write failing result-model tests**

Write tests against `practice::ResultModel` using three completed attempts plus
one abandoned count. Assert aggregate sample count, attempt selection,
compatibility group labels, and heatmap selection snapping to exact measure
start/end times.

- [ ] **Step 2: Run the test to verify failure**

```bash
cmake --build cmake-build-debug --target practice_result_model_tests -j 6
```

Expected: compilation fails on the missing model.

- [ ] **Step 3: Implement analytics view and Result integration**

Define the pure presentation model before constructing the view:

```cpp
class ResultModel {
public:
  ResultModel(const bms_parser::Chart &chart,
              std::span<const ReplayData> completedAttempts,
              std::size_t abandonedAttempts);
  void selectAttempt(std::optional<std::size_t> attemptIndex);
  void selectSection(std::size_t firstSection, std::size_t lastSection);
  [[nodiscard]] const Analysis &displayedAnalysis() const;
  [[nodiscard]] std::optional<RangeSelection> selectedRange() const;
  [[nodiscard]] std::size_t abandonedAttempts() const;
};
```

`PracticeAnalyticsView` accepts `practice::ResultModel` and exposes:

```cpp
enum class PracticeAnalyticsMode { Histogram, Lanes, Sections };
void setAttemptSelection(std::optional<std::size_t> attemptIndex);
void setMode(PracticeAnalyticsMode mode);
void setSectionSelectionListener(
    std::function<void(long long, long long)> listener);
[[nodiscard]] std::optional<practice::RangeSelection>
selectedSection() const;
```

Render signed histogram around zero, lane rows, and measure heatmap with
touch/mouse drag selection. In `ResultScene::init`, use a skin-provided
`timingAnalytics` host when present; otherwise create a full-width host and add
it to `rootLayout` immediately before `addRetryButtons()`/`addCourseButtons()`.
Tabs switch Histogram, Lanes, and Sections; attempt choice
switches Aggregate and each completed loop. Show abandoned loop count without
including it in aggregate statistics. Normal and replay results construct a
single-attempt model from their recorded/retry replay so they expose the same
analytics before Task 10 adds the launch action. Auto-play attempts are labeled
`Auto` and do not display player-bias wording. Visually group adjacent measures
only when required by width while retaining their exact individual boundaries
for drag selection.

Update replay photo/video export only to tolerate the new optional host; timing
analytics export is not required for this feature.

- [ ] **Step 4: Run tests and build**

```bash
cmake --build cmake-build-debug --target practice_result_model_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R 'practice_result_model_tests|practice_analytics_tests'
```

Expected: tests and desktop build pass.

- [ ] **Step 5: Commit the feature**

```bash
git add CMakeLists.txt src/practice/PracticeResultModel.h src/practice/PracticeResultModel.cpp src/practice/CMakeLists.txt src/scene/PracticeAnalyticsView.h src/scene/PracticeAnalyticsView.cpp src/scene/ResultScene.h src/scene/ResultScene.cpp src/scene/CMakeLists.txt src/ReplayVideoExporter.cpp tests/practice_result_model_tests.cpp ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat: show practice timing analysis results"
```

### Task 10: Practice This Section from results and replays

**Files:**
- Create: `src/practice/PracticeLaunchRequest.h`
- Create: `src/practice/PracticeLaunchRequest.cpp`
- Create: `tests/practice_launch_tests.cpp`
- Modify: `src/scene/ResultScene.h`
- Modify: `src/scene/ResultScene.cpp`
- Modify: `src/scene/ChartViewerScene.h`
- Modify: `src/scene/ChartViewerScene.cpp`
- Modify: `src/practice/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: selected analytics range, replay chart metadata, and per-chart last-used configuration.
- Produces: one range-preserving launch path shared by normal result, practice result, and replay result.

- [ ] **Step 1: Write failing handoff tests**

```cpp
practice::Configuration lastUsed;
lastUsed.chartSha256 = "0123456789abcdef0123456789abcdef"
                         "0123456789abcdef0123456789abcdef";
lastUsed.startMicros = 1'000'000;
lastUsed.endMicros = 30'000'000;
lastUsed.playback = {.percent = 75,
                     .mode = audio::PlaybackMode::PitchShift};
lastUsed.judge = {.kind = practice::JudgeOverrideKind::Scale,
                  .scalePercent = 80};
bms_parser::ChartMeta chartMeta;
chartMeta.SHA256 = lastUsed.chartSha256;
chartMeta.TotalLength = 40'000'000;
practice::LaunchRequest request{
    .chartMeta = chartMeta,
    .startMicros = 12'000'000,
    .endMicros = 20'000'000,
    .source = practice::LaunchSource::ReplayResult,
    .replayId = 42,
};
const auto merged = practice::applyLaunchRequest(lastUsed, request,
                                                  chartMeta.TotalLength);
assert(merged.startMicros == 12'000'000);
assert(merged.endMicros == 20'000'000);
assert(merged.playback == lastUsed.playback);
assert(merged.judge == lastUsed.judge);
```

Test normal result, practice aggregate, and replay result sources plus missing
chart path failure.

- [ ] **Step 2: Run the test to verify failure**

```bash
cmake --build cmake-build-debug --target practice_launch_tests -j 6
```

Expected: compilation fails because launch request/merge do not exist.

- [ ] **Step 3: Implement the shared launch request**

Define:

```cpp
enum class LaunchSource { ChartViewer, NormalResult, PracticeResult,
                          ReplayResult };
struct LaunchRequest {
  bms_parser::ChartMeta chartMeta;
  long long startMicros = 0;
  long long endMicros = 0;
  LaunchSource source = LaunchSource::ChartViewer;
  std::optional<int> replayId;
};
std::optional<std::string> validateLaunchRequest(const LaunchRequest &request);
Configuration applyLaunchRequest(const Configuration &lastUsed,
                                 const LaunchRequest &request,
                                 long long chartEndMicros);
```

Extend `ChartViewerScene` construction with an optional request. After chart
parse and preset load, merge only start/end, set the canvas range, show the
Practice panel, and retain all other last-used fields.

- [ ] **Step 4: Add Result and replay actions**

Add `Practice This Section` beside retry/export when analytics has a selected
range. Build `ChartMetaRecord` from Result Scene's chart metadata and difficulty
labels, stop Jukebox, and change directly to Chart Viewer with the request.
Replay Result uses `retryData`/replay provenance and the same path. If the chart
path is unavailable, `validateLaunchRequest` returns `Chart unavailable`; the
button is disabled and displays that status.

- [ ] **Step 5: Run focused and integration tests**

```bash
cmake --build cmake-build-debug --target practice_launch_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R 'practice_launch_tests|practice_result_model_tests|practice_analytics_tests'
```

Expected: tests and desktop build pass.

- [ ] **Step 6: Commit the feature**

```bash
git add CMakeLists.txt src/practice/PracticeLaunchRequest.h src/practice/PracticeLaunchRequest.cpp src/practice/CMakeLists.txt src/scene/ResultScene.h src/scene/ResultScene.cpp src/scene/ChartViewerScene.h src/scene/ChartViewerScene.cpp tests/practice_launch_tests.cpp ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat: practice timing sections from results"
```

### Task 11: Complete-system verification and platform compile gates

**Files:**
- Modify only owning feature files when verification exposes a defect.
- Do not create a miscellaneous cleanup commit containing unrelated changes.

**Interfaces:**
- Consumes: all previous feature commits.
- Produces: evidence that the complete P0 works together on supported targets.

- [ ] **Step 1: Run every application CTest entry except Yoga**

```bash
ctest --test-dir cmake-build-debug --output-on-failure
```

Expected: 100% of registered application tests pass; Yoga tests are absent
from the registered application list.

- [ ] **Step 2: Build desktop and mobile targets**

```bash
cmake --build cmake-build-debug --target main -j 6
scripts/ios_firebase_deploy.sh --build-only
scripts/android_firebase_deploy.sh --build-only
```

Expected: desktop, unsigned/build-only iOS, and Android release compile checks
succeed. These commands do not upload a build.

- [ ] **Step 3: Perform manual acceptance on available devices**

Verify start/end touch placement, range shading, named presets, four-beat count
in on every loop, instant restart without reload, gauge/judge overrides, audible
75% and 150% pitch changes, normal-play Assisted Easy labeling, aggregate and
per-loop analytics, lane/heatmap selection, and practice handoff from normal
and replay results. Confirm 100% playback is behaviorally identical to current
develop.

- [ ] **Step 4: Commit only targeted verification fixes**

For each defect, stage only its owning feature files and tests and use a scoped
message such as:

```bash
git add src/audio/AudioMix.cpp tests/audio_mix_tests.cpp
git commit -m "fix: keep rate mixer position continuous"
```

Repeat with a distinct commit for a different feature; do not combine audio,
practice UI, analytics, or persistence fixes.
