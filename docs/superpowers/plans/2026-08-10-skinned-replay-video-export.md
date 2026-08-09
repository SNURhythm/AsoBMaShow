# Skinned Replay Video Export Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Export normal and course replay gameplay frames through the selected Beatoraja gameplay skin, failing before output work when that skin cannot be used.

**Architecture:** Move selected-skin acquisition/session construction out of `GamePlayScene` into a reusable scene-level factory with explicit BuiltIn, Ready, and Failed outcomes.  Build replay gameplay frames with the existing `PlayfieldVisualStateStore`, `PlayfieldProjection`, and `PlayfieldPresentationCoordinator`, so that the existing coordinator remains the only skin-evaluation/BGA-composition path.  A replay-specific presentation bridge is shared by normal and course loops; it owns the state snapshot and delegates all skin session construction to the common factory.

**Tech Stack:** C++23, LuaJIT/sol2 gameplay-skin runtime, bgfx, FFmpeg replay export, CMake/CTest, existing unsigned iOS release verifier.

## Global Constraints

- The pinned Beatoraja checkout at `c2ed5db1a46145ed10790c3872f717e95b59db9d` is the sole compatibility source of truth; refresh the relevant source before each compatibility-sensitive change.
- Preserve direct, live `Documents/Skins` selected-root Lua I/O. Do not add a private snapshot, overlay, render freeze, quota, or automatic rescan.
- No selected skin means existing built-in replay export. A selected skin that is unavailable, cannot create a session, or fails a gameplay frame aborts export; it must never fall back to built-in mid-video.
- Apply the behavior to normal and course gameplay stages. Keep normal and course result screens built-in.
- Reuse `PlaySkinSession` and `PlayfieldPresentationCoordinator`; do not duplicate Lua evaluation, skin BGA placement, replay event reduction, or session construction in normal/course loops.
- Preflight selected skin sessions before audio rendering, work-directory creation, encoder allocation, MP4 creation, or Photos-library authorization.
- Preserve unrelated user changes. Do not deploy/upload. Use desktop incremental builds with `cmake --build cmake-build-debug --target main -j 12`; redirect verbose output to `/tmp` and print only errors/a concise tail.
- Run `scripts/ios_release_verify.sh` after the desktop suite for the final native checkpoint; do not use a clean rebuild unless verification itself requires one.

---

## File Structure

- `src/scene/play/GameplaySkinSessionFactory.h/.cpp` — reusable, owning selected-skin acquisition/session construction plus diagnostic-history recording. It depends on narrow service references rather than `ApplicationContext` so gameplay and exporter share one implementation.
- `src/scene/play/ReplayPlayfieldPresentation.h/.cpp` — replay-only adapter that owns a visual-state store, projection, built-in presentation/coordinator, and optional session. It converts existing replay updates into one snapshot/projection/frame submission path shared by normal and course exports.
- `src/scene/play/GamePlayScene.cpp` — replace the local session-construction duplicate with the factory.
- `src/ReplayVideoExporter.cpp` — preflight and retain stage presentation adapters; replace direct gameplay `BMSRenderer::render` calls with adapter frame submission. Keep result-rendering code untouched.
- `src/scene/play/CMakeLists.txt`, `src/CMakeLists.txt`, `CMakeLists.txt` — compile the shared code for `main` and focused tests.
- `tests/gameplay_skin_session_factory_tests.cpp` — factory behavior and exact diagnostic-history recording using injectable lifecycle/session seams.
- `tests/replay_playfield_presentation_tests.cpp` — replay adapter snapshot/event/frame behavior and skin failure semantics using coordinator test seams.
- `tests/replay_contract_boundary_tests.cpp` — source-level guard that normal/course export preflight occurs before output work and both gameplay loops use the shared adapter.

---

### Task 1: Extract selected-skin session construction

**Files:**
- Create: `src/scene/play/GameplaySkinSessionFactory.h`
- Create: `src/scene/play/GameplaySkinSessionFactory.cpp`
- Modify: `src/scene/play/GamePlayScene.cpp:120-157,1432-1543`
- Modify: `src/scene/play/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Test: `tests/gameplay_skin_session_factory_tests.cpp`

**Interfaces:**

```cpp
struct GameplaySkinSessionServices {
  skin::AcquireGameplaySkinForNextChart acquire;
  const skin::SkinStorageRoots *storageRoots = nullptr;
  skin::SkinResourcePreparationService *resourcePreparation = nullptr;
  std::shared_ptr<skin::SkinLiveResourceCounters> liveResourceCounters;
  skin::SkinConfigurationWriteQueue *configurationWrites = nullptr;
  skin::SkinDiagnosticHistory *diagnosticHistory = nullptr;
};

struct GameplaySkinSessionInput {
  int keyMode = 0;
  const PlayfieldChartVisualModel *chartModel = nullptr;
  const PlayfieldVisualState *initialState = nullptr;
  const PlayfieldProjectionResult *initialProjection = nullptr;
  skin::UiLogicalRect safeUiBounds;
};

enum class GameplaySkinSessionDisposition : std::uint8_t {
  BuiltIn, Ready, Failed
};

struct GameplaySkinSessionResult {
  GameplaySkinSessionDisposition disposition =
      GameplaySkinSessionDisposition::BuiltIn;
  std::unique_ptr<skin::PlaySkinSession> session;
  std::optional<PresentationFailure> failure;
};

[[nodiscard]] GameplaySkinSessionResult createGameplaySkinSession(
    GameplaySkinSessionServices services, GameplaySkinSessionInput input);
```

`BuiltIn` is returned only when the lifecycle callback reports no selected
activation. `Failed` always contains the first error diagnostic plus the exact
entry/revision/configuration identity when lifecycle/session construction
provides it. `Ready` always contains one owning session. The factory appends
every construction diagnostic to `SkinDiagnosticHistory` with
`SkinDiagnosticPhase::Session`; its history helper must preserve source path,
line, column, severity, and session identity exactly as the former
`GamePlayScene` helper did.

- [ ] **Step 1: Write the failing factory tests**

Create a minimal injectable test seam for the lifecycle acquisition and session
constructor, then add these tests before implementation:

```cpp
expect(createGameplaySkinSession(noSelectionServices(), validInput()).disposition ==
       GameplaySkinSessionDisposition::BuiltIn,
       "no selection keeps built-in presentation available");

const auto failed = createGameplaySkinSession(failedAcquireServices(
    diagnostic("skin.lifecycle.activation_unavailable", "not ready")),
    validInput());
expect(failed.disposition == GameplaySkinSessionDisposition::Failed &&
       failed.failure &&
       failed.failure->diagnostic.code == "skin.lifecycle.activation_unavailable",
       "factory preserves lifecycle diagnostic");
expect(historyRecords().back().phase == skin::SkinDiagnosticPhase::Session,
       "factory records failed acquisition");

const auto ready = createGameplaySkinSession(readyServices(), validInput());
expect(ready.disposition == GameplaySkinSessionDisposition::Ready &&
       ready.session != nullptr,
       "factory transfers the owning session exactly once");
```

- [ ] **Step 2: Run the focused test and confirm RED**

Run:

```sh
cmake --build cmake-build-debug --target gameplay_skin_session_factory_tests -j 12 \
  > /tmp/gameplay-skin-session-factory-red.log 2>&1
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^gameplay_skin_session_factory_tests$'
```

Expected: the target/test fails because the factory header, result types, and
factory function do not exist.

- [ ] **Step 3: Implement the factory and remove the gameplay duplicate**

Implement the declared input/services/result in the new files. Move the
history-record construction from `GamePlayScene.cpp` into a non-throwing
factory-local helper. Move the complete acquisition/session context currently
in `GamePlayScene::acquireGameplaySkinForAttempt` into
`createGameplaySkinSession`; retain the existing `BgfxSkinTextureDevice`,
activation request ownership, viewport, serial, profile, resource service,
live counter, and configuration queue semantics.

Make `GamePlayScene::acquireGameplaySkinForAttempt` only:

```cpp
const auto result = createGameplaySkinSession(gameplaySkinSessionServices(context), {
    .keyMode = chart->Meta.KeyMode,
    .chartModel = &playfieldChartVisualModel,
    .initialState = &capturedPlayfieldVisualState,
    .initialProjection = &capturedPlayfieldProjection,
    .safeUiBounds = gameplaySkinSafeUiBounds(),
});
if (result.disposition == GameplaySkinSessionDisposition::Failed) {
  showPlaybackInitializationFailure(gameplaySkinFailureMessage(
      result.failure->diagnostic));
  return;
}
if (result.disposition == GameplaySkinSessionDisposition::Ready) {
  coordinator->installSkinSession(std::move(result.session));
}
```

Do not call package store or validator APIs from the scene/exporter; use only
the lifecycle callback. Keep exception conversion inside the factory so both
consumers receive the same diagnostic.

- [ ] **Step 4: Run the focused tests and gameplay regression tests**

Run:

```sh
cmake --build cmake-build-debug --target gameplay_skin_session_factory_tests gameplay_skin_integration_tests gameplay_skin_lifecycle_tests -j 12 \
  > /tmp/gameplay-skin-session-factory-green.log 2>&1
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(gameplay_skin_session_factory_tests|gameplay_skin_integration_tests|gameplay_skin_lifecycle_tests)$'
```

Expected: all selected tests pass; normal gameplay retains no-selection
built-in behavior and uses the shared factory for selected skins.

- [ ] **Step 5: Commit the isolated factory slice**

```sh
git add src/scene/play/GameplaySkinSessionFactory.h \
  src/scene/play/GameplaySkinSessionFactory.cpp \
  src/scene/play/GamePlayScene.cpp src/scene/play/CMakeLists.txt \
  CMakeLists.txt tests/gameplay_skin_session_factory_tests.cpp
git commit -m "refactor: share gameplay skin session construction"
```

### Task 2: Add a coordinator-backed replay gameplay presentation adapter

**Files:**
- Create: `src/scene/play/ReplayPlayfieldPresentation.h`
- Create: `src/scene/play/ReplayPlayfieldPresentation.cpp`
- Modify: `src/scene/play/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Test: `tests/replay_playfield_presentation_tests.cpp`

**Interfaces:**

```cpp
struct ReplayPlayfieldPresentationCreateInfo {
  bms_parser::Chart &chart;
  std::map<Judgement, std::pair<long long, long long>> timingWindows;
  const PlayfieldPresentationConfig &configuration;
  const AppSettings &settings;
  audio::PlaybackRate playback;
  IGameplayBgaSubmitter &bga;
  GameplaySkinSessionServices skinServices;
  GameplaySkinSessionInput skinInput;
  std::function<void(const PresentationFailure &)> recordFailure;
};

struct ReplayPlayfieldPresentationCreateResult {
  std::unique_ptr<ReplayPlayfieldPresentation> presentation;
  std::optional<PresentationFailure> failure;
};

class ReplayPlayfieldPresentation final {
public:
  static ReplayPlayfieldPresentationCreateResult create(
      ReplayPlayfieldPresentationCreateInfo);
  void applyReplayEvent(const ReplayEvent &, const PlayfieldJudgeEventClock &,
                        bool recordTimingSample);
  void applyAuthorityUpdate(const PlayfieldAuthorityUpdate &);
  [[nodiscard]] PresentationFrameResult renderFrame(
      RenderContext &, PlayfieldFrameClock,
      const PlayfieldProjectionRequest &);
  [[nodiscard]] BMSRenderer &builtInRenderer() noexcept;
};
```

The adapter owns `PlayfieldChartVisualModel`, `PlayfieldVisualStateStore`,
`PlayfieldPresentationEventFanout`, `PlayfieldProjection`, and a
`PlayfieldPresentationCoordinator`. `create` first captures an initial state
and projection, invokes Task 1's factory, then installs the returned skin
session only for `Ready`. A `Failed` result has no adapter. `renderFrame`
captures exactly one state/projection pair and calls coordinator
`prepareFrame` then `render` once; it returns the coordinator result without
converting a skin failure to built-in output.

- [ ] **Step 1: Write the failing replay-adapter tests**

Use the existing coordinator and session testing seams. Add tests that observe
the real adapter behavior:

```cpp
auto created = ReplayPlayfieldPresentation::create(validSkinCreateInfo());
expect(created.presentation != nullptr && !created.failure,
       "valid selected skin creates one coordinator-backed adapter");

created.presentation->applyReplayEvent(judgeEvent, judgeClock, true);
const auto frame = created.presentation->renderFrame(
    context, {.serial = 7, .visualTimeMicros = 300, .gameplayTimeMicros = 200,
              .bgaTimeMicros = 200}, projectionRequest);
expect(fakeSkin->preparedSerial == 7 && fakeSkin->renderCalls == 1,
       "skin receives the shared replay snapshot exactly once");
expect(fakeBuiltIn->renderCalls == 0 &&
       frame.submittedMode == PresentationMode::Skin,
       "successful replay frame does not draw built-in gameplay");

fakeSkin->failPrepare = true;
const auto failed = created.presentation->renderFrame(context, nextClock,
                                                       projectionRequest);
expect(failed.outcome == PresentationFrameOutcome::CriticalFailure &&
       failed.failure && !failed.failure->diagnostic.code.empty(),
       "skin failure is exposed to export without a built-in replacement");
```

Also assert `applyReplayEvent` delivers lane press/release/judge events to the
state store and coordinator fan-out exactly once, retains replay touch data,
and uses the `PlayfieldJudgeEventClock` supplied by the exporter rather than a
render-wall-clock value.

- [ ] **Step 2: Run the adapter test and confirm RED**

Run:

```sh
cmake --build cmake-build-debug --target replay_playfield_presentation_tests -j 12 \
  > /tmp/replay-playfield-presentation-red.log 2>&1
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^replay_playfield_presentation_tests$'
```

Expected: compilation fails because `ReplayPlayfieldPresentation` does not
exist.

- [ ] **Step 3: Implement the adapter using existing coordinator primitives**

Create the visual model with `buildPlayfieldChartVisualModel`, initialize the
state store/configuration and built-in `BMSRenderer` using
`createBuiltInPlayfieldPresentation`, retain the concrete renderer pointer
before transferring ownership into `PlayfieldPresentationCoordinator`, and
construct the event fan-out with the coordinator as presentation sink.

Move the existing shared replay reducer body from
`applyReplayEventForVideo(BMSRenderer &, ...)` into
`ReplayPlayfieldPresentation::applyReplayEvent`. It must update the one state
store and forward each presentation event through the fan-out; do not leave
parallel normal/course reducer variants. Preserve the existing gauge,
pacemaker, BPM, lane-cover, invisible-note, touch, ghost, lane-indicator, and
judgement-counter inputs by representing them in the existing
`PlayfieldPresentationConfig`/`PlayfieldAuthorityUpdate` before each capture.

On `PresentationFrameOutcome::CriticalFailure` or a result failure, return the
exact `PresentationFailure`; do not invoke a second renderer path. The caller
will abort export and clean output.

- [ ] **Step 4: Run adapter/coordinator/session regressions**

Run:

```sh
cmake --build cmake-build-debug --target replay_playfield_presentation_tests playfield_presentation_coordinator_tests play_skin_session_tests -j 12 \
  > /tmp/replay-playfield-presentation-green.log 2>&1
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(replay_playfield_presentation_tests|playfield_presentation_coordinator_tests|play_skin_session_tests)$'
```

Expected: all selected tests pass and prove that the adapter does not issue a
separate Lua/BGA submission path.

- [ ] **Step 5: Commit the adapter slice**

```sh
git add src/scene/play/ReplayPlayfieldPresentation.h \
  src/scene/play/ReplayPlayfieldPresentation.cpp src/scene/play/CMakeLists.txt \
  CMakeLists.txt tests/replay_playfield_presentation_tests.cpp
git commit -m "feat: add coordinator-backed replay skin presentation"
```

### Task 3: Preflight and wire normal replay export

**Files:**
- Modify: `src/ReplayVideoExporter.cpp:2616-3278,4072-4169`
- Modify: `tests/replay_contract_boundary_tests.cpp`
- Test: `tests/replay_playfield_presentation_tests.cpp`

**Interfaces:**

Add a file-local `PreparedReplayGameplayPresentation` that owns a
`ReplayPlayfieldPresentation`, and a file-local helper:

```cpp
[[nodiscard]] std::optional<ReplayVideoExportResult>
preflightReplayGameplayPresentation(
    ApplicationContext &, bms_parser::Chart &, const ReplayData &,
    const AppSettings &, const preparation::Plan &,
    const ReplayVideoExportOptions &, PreparedReplayGameplayPresentation &,
    ReplayVideoExportLog *);
```

It returns `std::nullopt` only when the prepared adapter is ready. Otherwise it
returns `{.success = false, .message = diagnostic.message}` where the message
contains the authoritative diagnostic code in brackets; it must not create an
output file or invoke audio/render setup.

- [ ] **Step 1: Write RED boundary and behavior tests**

Extend `replay_contract_boundary_tests.cpp` with these two local helpers before
adding the precise source-order checks:

```cpp
void requireOrdered(const std::filesystem::path &path, std::string_view first,
                    std::string_view second, std::string_view authority) {
  const auto text = readText(path);
  const auto firstAt = text.find(first);
  const auto secondAt = text.find(second);
  if (firstAt == std::string::npos || secondAt == std::string::npos ||
      firstAt >= secondAt) {
    std::cerr << "FAIL: " << authority << '\\n';
    ++failures;
  }
}

void requireAbsentBetween(const std::filesystem::path &path,
                          std::string_view first, std::string_view last,
                          std::string_view forbidden,
                          std::string_view authority) {
  const auto text = readText(path);
  const auto firstAt = text.find(first);
  const auto lastAt = text.find(last, firstAt);
  if (firstAt == std::string::npos || lastAt == std::string::npos ||
      text.find(forbidden, firstAt) < lastAt) {
    std::cerr << "FAIL: " << authority << '\\n';
    ++failures;
  }
}
```

Then add:

```cpp
requireOrdered(exporter, "preflightReplayGameplayPresentation(",
               "writeReplayAudioTrack(",
               "skin preflight occurs before normal replay audio work");
requireOrdered(exporter, "preflightReplayGameplayPresentation(",
               "renderReplayVideoToMp4(",
               "skin preflight occurs before normal MP4 rendering");
requireToken(exporter, "ReplayPlayfieldPresentation",
             "normal replay uses the coordinator-backed presentation adapter");
requireAbsentBetween(exporter, "renderReplayVideoToMp4(",
                     "ReplayVideoExportResult renderCourseReplayVideoToMp4(",
                     "renderer.render(renderContext, frameTiming.visualTimeMicros",
                     "normal replay no longer directly renders gameplay");
```

Add an adapter integration test with an unavailable selected skin that asserts
the preflight result has `success == false`, includes
`skin.lifecycle.activation_unavailable`, and leaves its fake audio/MP4 work
counters at zero.

- [ ] **Step 2: Run RED tests**

Run:

```sh
cmake --build cmake-build-debug --target replay_contract_boundary_tests replay_playfield_presentation_tests -j 12 \
  > /tmp/replay-skin-normal-red.log 2>&1
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(replay_contract_boundary_tests|replay_playfield_presentation_tests)$'
```

Expected: the new source/behavior assertions fail because normal replay still
begins audio work and renders gameplay directly.

- [ ] **Step 3: Implement normal replay preflight and adapter frame submission**

At the top-level normal-export entry, resolve the replay chart/model/config and
call preflight before `writeReplayAudioTrack`, directory creation, output-path
creation, or iOS Photos authorization. Retain the adapter through the encoding
call. Replace each normal gameplay-frame sequence that calls
`BMSRenderer::render` with:

```cpp
const auto presentationFrame = prepared.presentation->renderFrame(
    renderContext, {.serial = ++presentationSerial,
                    .visualTimeMicros = frameTiming.visualTimeMicros,
                    .gameplayTimeMicros = frameTiming.gameplayTimeMicros,
                    .replayTouchTimeMicros = frameTiming.gameplayTimeMicros,
                    .bgaTimeMicros = frameTiming.bgaTimeMicros},
    projectionRequest);
if (presentationFrame.outcome == PresentationFrameOutcome::CriticalFailure ||
    presentationFrame.failure) {
  errorMessage = replaySkinExportFailureMessage(*presentationFrame.failure);
  return false;
}
```

Use `presentationFrame.bgaCompositeMode` and its prepared frame as returned by
the coordinator; do not call `Jukebox::submitFullscreen` or `BlurPass` a
second time for a successful skin frame. Preserve the established direct BGA
submission only for a `BuiltIn` adapter. Leave the built-in result-screen loop
and its fullscreen BGA behavior unchanged.

- [ ] **Step 4: Run normal replay tests**

Run:

```sh
cmake --build cmake-build-debug --target replay_contract_boundary_tests replay_playfield_presentation_tests main -j 12 \
  > /tmp/replay-skin-normal-green.log 2>&1
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(replay_contract_boundary_tests|replay_playfield_presentation_tests|playfield_presentation_coordinator_tests)$'
```

Expected: focused tests pass; an unavailable selected skin ends before output
work, no selected skin keeps built-in export, and a successful skin frame has
one coordinator submission.

- [ ] **Step 5: Commit normal replay wiring**

```sh
git add src/ReplayVideoExporter.cpp tests/replay_contract_boundary_tests.cpp \
  tests/replay_playfield_presentation_tests.cpp
git commit -m "feat: render replay video with gameplay skins"
```

### Task 4: Preflight and wire course replay stages

**Files:**
- Modify: `src/ReplayVideoExporter.cpp:3280-4077,4189-4423`
- Modify: `tests/replay_contract_boundary_tests.cpp`
- Test: `tests/replay_playfield_presentation_tests.cpp`

**Interfaces:**

Use one `PreparedReplayGameplayPresentation` per `CourseReplayVideoStage`:

```cpp
struct CourseReplayVideoStage {
  // existing fields ...
  std::optional<PreparedReplayGameplayPresentation> gameplayPresentation;
};

[[nodiscard]] std::optional<ReplayVideoExportResult>
preflightCourseReplayGameplayPresentations(
    ApplicationContext &, std::vector<CourseReplayVideoStage> &,
    const AppSettings &, const ReplayVideoExportOptions &, ReplayVideoExportLog *);
```

The helper visits stages in their encoded order and retains each successful
presentation. It returns on the first selected-skin failure before
`writeReplayAudioTrack` or `writeCourseReplayAudioTrack` runs.

- [ ] **Step 1: Write RED course tests**

Add these source/behavior assertions:

```cpp
requireOrdered(exporter, "preflightCourseReplayGameplayPresentations(",
               "writeReplayAudioTrack(",
               "every course skin is preflighted before stage audio");
requireOrdered(exporter, "preflightCourseReplayGameplayPresentations(",
               "writeCourseReplayAudioTrack(",
               "every course skin is preflighted before mux audio");
requireToken(exporter, "stage.gameplayPresentation",
             "course stages retain preflighted presentation ownership");

const auto failure = preflightCourseFor(
    {readyStage(7), unavailableStage(14)});
expect(!failure.success && contains(failure.message,
                                    "skin.lifecycle.activation_unavailable"),
       "a later course stage fails before output work");
expect(fakeAudioWork.calls == 0 && fakeEncoderWork.calls == 0,
       "course failure has no partial media output");
```

- [ ] **Step 2: Run RED course tests**

Run:

```sh
cmake --build cmake-build-debug --target replay_contract_boundary_tests replay_playfield_presentation_tests -j 12 \
  > /tmp/replay-skin-course-red.log 2>&1
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(replay_contract_boundary_tests|replay_playfield_presentation_tests)$'
```

Expected: tests fail because course stages are not preflighted/retained and
still render direct `BMSRenderer` gameplay.

- [ ] **Step 3: Implement course preflight and shared stage adapter use**

After course charts/replays are materialized but before Photos authorization,
output-directory/work-directory creation, or any stage audio is rendered,
build and retain each stage adapter with that stage's keymode, preparation
plan, initial gauge/state, and export-size safe bounds. A stage with no
selected skin receives a built-in adapter. If any selected stage fails, return
its exact diagnostic without creating temporary media state or starting
audio/video work.

In `renderCourseReplayVideoToMp4`, replace the stage's direct gameplay
`BMSRenderer::render` calls with its retained adapter's `renderFrame`. Reuse
the same failure-to-cleanup path as normal export. Do not apply the adapter to
the stage result or final course-result loops.

- [ ] **Step 4: Run course and full replay focused tests**

Run:

```sh
cmake --build cmake-build-debug --target replay_contract_boundary_tests replay_playfield_presentation_tests video_frame_layout_tests main -j 12 \
  > /tmp/replay-skin-course-green.log 2>&1
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(replay_contract_boundary_tests|replay_playfield_presentation_tests|video_frame_layout_tests|gameplay_bga_target_tests)$'
```

Expected: normal and course tests pass, selected-skin failures create no media
output, and successful stages share the coordinator path.

- [ ] **Step 5: Commit course replay wiring**

```sh
git add src/ReplayVideoExporter.cpp tests/replay_contract_boundary_tests.cpp \
  tests/replay_playfield_presentation_tests.cpp
git commit -m "feat: support gameplay skins in course replay export"
```

### Task 5: End-to-end verification and compatibility record

**Files:**
- Modify: `docs/todo.md` only if implementation discovers an unavailable
  Beatoraja gameplay property; list the exact upstream source/symbol and leave
  it as an explicit later item.
- Modify: `docs/skin-compat/modernchic-scuro-4.6-acceptance.md` only if its
  stated export behavior changes; otherwise do not broaden acceptance scope.

- [ ] **Step 1: Run the complete desktop skin/replay suite**

Run:

```sh
cmake --build cmake-build-debug --target main -j 12 \
  > /tmp/replay-skin-desktop-build.log 2>&1
ctest --test-dir cmake-build-debug --output-on-failure \
  > /tmp/replay-skin-desktop-ctest.log 2>&1
```

Expected: all tests pass. If either command fails, print its error lines and
the final 120 lines of the log, trace the first failure to its source, add a
new RED regression, and fix only that proven cause.

- [ ] **Step 2: Run the native release verification**

Run:

```sh
scripts/ios_release_verify.sh > /tmp/replay-skin-ios-verify.log 2>&1
```

Expected: release-critical CTests pass, unsigned arm64 iOS build succeeds,
and the iOS artifact audit passes. Do not run a deploy/upload command.

- [ ] **Step 3: Record only genuine remaining compatibility gaps**

If a replay-export skin exposes an unsupported property proven by the pinned
Beatoraja source, add one `docs/todo.md` bullet containing its upstream path,
class/symbol, observed export symptom, and the missing AsoBMaShow interface.
Do not record a gap for a behavior unsupported by Beatoraja or for a local
test-fixture assumption.

- [ ] **Step 4: Commit the verification/documents slice only when changed**

```sh
git add docs/todo.md docs/skin-compat/modernchic-scuro-4.6-acceptance.md
git commit -m "docs: record replay skin export compatibility"
```

Run this command only if one of the two listed files changed. Otherwise leave
the working tree untouched after verification.
