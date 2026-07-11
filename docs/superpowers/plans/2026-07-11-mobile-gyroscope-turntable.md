# Mobile Gyroscope Turntable Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:subagent-driven-development` and
> `superpowers:test-driven-development`. Platform tasks run in the isolated
> worktrees named below and return reviewed commits to the integration branch.

**Goal:** Expose the built-in motion sensors on iPhone, iPad, Android phones,
and Android tablets as one compass-corrected, bindable turntable controller
whose zero is reset at every gameplay or practice attempt.

**Architecture:** A portable C++ state machine converts corrected yaw plus
world-vertical gyroscope rate into a full-scale signed axis. A portable backend
core owns publication, sensor health, calibration status, and retry timing.
Thin Core Motion and Android SensorManager adapters feed that core through the
existing `IInputBackend`/`InputDeviceRegistry` pipeline. The active input
profile owns step-angle and release-delay settings. Internet ranking is not
part of this implementation.

**Tech stack:** C++23, SDL2, CMake/CTest, Objective-C++/Core Motion, Java/JNI
Android sensors, nlohmann JSON, custom View/Yoga settings UI.

**Design:**
`docs/superpowers/specs/2026-07-11-mobile-gyroscope-turntable-design.md`

## Global constraints

- Integration branch: `feature/mobile-gyroscope-turntable`.
- Use `.worktrees/mobile-gyro-settings`, `.worktrees/mobile-gyro-ios`, and
  `.worktrees/mobile-gyro-android` after Tasks 1–4 establish shared APIs.
- Worktree branches are `feature/mobile-gyro-settings`,
  `feature/mobile-gyro-ios`, and `feature/mobile-gyro-android`.
- Stable device ID is exactly `builtin:gyroscope-turntable`; axis index is 0.
- Append enum values; never renumber existing profile or score categories.
- Implement every behavior red-first, then run the focused CTest/JVM test
  before committing it.
- Do not edit `src/bms_parser.*`, touch IR/network ranking, or alter existing
  keyboard/controller/touch/MIDI behavior.
- Add every new `src` path to the iOS target `membershipExceptions`.
- Build-only verification is allowed; never invoke a deploy/upload command.

---

### Task 0: Establish the isolated execution layout and baseline

**Files:**

- Modify only if needed: `.gitignore`

- [ ] **Step 1: Verify repository/worktree identity and ignore safety**

Run:

```bash
GIT_DIR=$(cd "$(git rev-parse --git-dir)" && pwd -P)
GIT_COMMON=$(cd "$(git rev-parse --git-common-dir)" && pwd -P)
test -n "$GIT_DIR" && test -n "$GIT_COMMON"
git rev-parse --show-superproject-working-tree
git check-ignore -q .worktrees
```

If `.worktrees` is not ignored, add only `/.worktrees/` to `.gitignore`, run
`git diff --check`, and commit `chore: ignore local agent worktrees`.

- [ ] **Step 2: Verify the clean integration baseline**

Run:

```bash
cmake --build cmake-build-debug --target main -j 6
ctest --test-dir cmake-build-debug --output-on-failure
git status --short --branch
```

Expected: `main` builds, all currently registered CTests pass, and the feature
branch is clean. Record any pre-existing failure before feature work.

Tasks 1–4 run sequentially on the integration branch because they establish
the contracts consumed by every platform worktree.

---

### Task 1: Portable motion-to-turntable state machine

**Files:**

- Create: `src/input/GyroscopeTurntable.h`
- Create: `src/input/GyroscopeTurntable.cpp`
- Create: `tests/gyroscope_turntable_tests.cpp`
- Modify: `src/input/InputTypes.h`
- Modify: `src/input/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Public interface:**

```cpp
namespace input {
inline constexpr std::string_view kGyroscopeTurntableStableId =
    "builtin:gyroscope-turntable";
inline constexpr std::string_view kGyroscopeTurntableDisplayName =
    "Gyroscope Turntable";
inline constexpr int kGyroscopeTurntableAxis = 0;

struct GyroscopeTurntableConfig {
  static constexpr int kDefaultStepAngleDegrees = 3;
  static constexpr int kMinStepAngleDegrees = 1;
  static constexpr int kMaxStepAngleDegrees = 45;
  static constexpr int kDefaultReleaseDelayMs = 200;
  static constexpr int kMinReleaseDelayMs = 50;
  static constexpr int kMaxReleaseDelayMs = 1000;

  int stepAngleDegrees = kDefaultStepAngleDegrees;
  int releaseDelayMs = kDefaultReleaseDelayMs;
  auto operator<=>(const GyroscopeTurntableConfig &) const = default;
  void sanitize(std::vector<std::string> &diagnostics);
};

struct GyroscopeMotionSample {
  double headingDegrees = 0.0;
  double clockwiseRateDegreesPerSecond = 0.0;
  double sensorTimestampSeconds = 0.0;
  std::uint64_t accuracyGeneration = 0;
  bool usableAccuracy = false;
  bool discontinuity = false;
};

class GyroscopeTurntable {
public:
  explicit GyroscopeTurntable(GyroscopeTurntableConfig config = {});
  std::optional<float> observe(const GyroscopeMotionSample &sample,
                               std::uint64_t monotonicNowMicros);
  std::optional<float> advance(std::uint64_t monotonicNowMicros);
  std::optional<float> configure(GyroscopeTurntableConfig config);
  std::optional<float> reset();
  [[nodiscard]] float value() const;
  [[nodiscard]] const GyroscopeTurntableConfig &config() const;
};
} // namespace input
```

Append `Gyroscope` to `DeviceClass`. Add an `InputDeviceStatus` enum with
`Ready`, `Calibrating`, `Disconnected`, and `Retrying`, and add a defaulted
status field to `InputDeviceSnapshot` without changing current field meanings.

- [ ] **Step 1: Write the complete state-machine test table**

Use a small fixture that advances sensor seconds and monotonic microseconds
independently. Cover:

- first usable sample and explicit reset establish a baseline without output;
- 3-degree clockwise/counter-clockwise activation and sub-step accumulation;
- 359→0 and 0→359 shortest-path wrap;
- whole-step consumption with a retained remainder;
- direction change clears the prior remainder and a full opposite step returns
  the opposite value immediately;
- same-direction steps refresh the deadline without a duplicate transition;
- release occurs on the first `advance()` at/after 200 ms and clears remainder;
- equal sensor timestamps are ignored while `advance()` still releases;
- stationary corrected-heading drift, including a gradual 20-degree drift,
  creates no input below the 0.5 dps gyroscope floor;
- matching yaw/gyro motion uses yaw, while sign/magnitude disagreement falls
  back to gyro delta without emitting compass correction;
- non-finite fields, old timestamps, gaps above 250 ms, rates above 1080 dps,
  explicit discontinuity, unusable accuracy, and accuracy-generation changes
  release/reseed without a scratch;
- config ranges clamp independently and `configure()`/`reset()` release active
  output and discard the baseline.

- [ ] **Step 2: Register the test and verify RED**

Add `gyroscope_turntable_tests` and CTest name
`foundation_input_gyroscope`. Build it before creating production files.

```bash
cmake --build cmake-build-debug --target gyroscope_turntable_tests -j 6
```

Expected: compilation fails because `GyroscopeTurntable.h` is absent.

- [ ] **Step 3: Implement the minimum portable algorithm**

Normalize heading deltas to `[-180, 180]`. Trapezoidally integrate adjacent
gyro rates. Treat both adjacent rates below `0.5` dps as stationary. During
motion, accept corrected-yaw delta only when sign agrees and the magnitude
difference is no greater than `max(0.75, abs(gyroDelta) * 0.5)`; otherwise use
gyro delta. Return an `optional<float>` only when the public axis changes.
Configuration/reset always clear prior sample, accumulator, direction, and
deadline even if no zero event is required.

- [ ] **Step 4: Verify GREEN and commit**

```bash
cmake --build cmake-build-debug --target gyroscope_turntable_tests -j 6
ctest --test-dir cmake-build-debug \
  -R '^foundation_input_gyroscope$' --output-on-failure
git diff --check
git add src/input/GyroscopeTurntable.h src/input/GyroscopeTurntable.cpp \
  src/input/InputTypes.h src/input/CMakeLists.txt CMakeLists.txt \
  tests/gyroscope_turntable_tests.cpp \
  ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat(input): add gyroscope turntable state machine"
```

---

### Task 2: Profile schema and score provenance vocabulary

**Files:**

- Modify: `src/input/InputProfile.h`
- Modify: `src/input/InputProfile.cpp`
- Modify: `src/input/InputProfileStore.cpp`
- Modify: `tests/input_profile_tests.cpp`
- Modify: `tests/fixtures/input/input-future.json`
- Modify: `src/ScoreProvenance.h`
- Modify: `src/ScoreProvenance.cpp`
- Modify: `src/scene/play/GamePlayStartOptions.h`
- Modify: `tests/score_provenance_tests.cpp`

**Schema contract:**

```cpp
struct InputProfile {
  static constexpr int kSchemaVersion = 2;
  int schemaVersion = kSchemaVersion;
  input::GyroscopeTurntableConfig gyroscopeTurntable;
  std::vector<input::InputBinding> bindings;
  // ...
};

enum class InputDeviceCategory : int {
  Keyboard = 0,
  GameController = 1,
  Joystick = 2,
  Touch = 3,
  Midi = 4,
  Unknown = 5,
  Gyroscope = 6,
};
```

`ScoreProvenance::kSchemaVersion` becomes 2. JSON uses device class
`"gyroscope"`, score category `"gyroscope"`, and:

```json
"gyroscopeTurntable": {
  "stepAngleDegrees": 3,
  "releaseDelayMs": 200
}
```

- [ ] **Step 1: Add failing profile migration/persistence tests**

Cover v1 default migration, v2 round trip, a Gyroscope axis binding, independent
fallback for missing/wrong-type fields, independent range clamping, current
schema serialization, and v3 rejection. Preserve the existing v0 migration
test. Change `tests/fixtures/input/input-future.json` to schema 3 only after the
RED assertion is established.

- [ ] **Step 2: Add failing provenance compatibility tests**

Assert every old enum's exact integer, `Gyroscope == 6`, canonicalized
Gyroscope serialization, v1 decode migrated to an in-memory schema 2 object,
schema 2 round trip, and v3 rejection.

```bash
cmake --build cmake-build-debug \
  --target input_profile_tests score_provenance_tests -j 6
ctest --test-dir cmake-build-debug \
  -R '^(foundation_input_profile|foundation_provenance_contract)$' \
  --output-on-failure
```

Expected: new schema/category assertions fail.

- [ ] **Step 3: Implement schema 2 with field-local recovery**

`InputProfile::sanitize()` calls `gyroscopeTurntable.sanitize()` and reports
the real current schema version instead of the existing hard-coded `1`.
`InputProfileStore::load()` accepts schemas 0, 1, and 2; only schema 2 attempts
to parse the config object. A missing or non-integer member resets that member
alone and appends a diagnostic. Typed out-of-range values are left for normal
sanitization. Save always emits schema 2 and both config members.

`deserializeScoreProvenance()` accepts schema 1 or 2, rejects values below 1
or above 2, and returns a current-schema object after v1 migration. Map
`DeviceClass::Gyroscope` in `GamePlayStartOptions.h`; do not touch resolver or
replay code.

- [ ] **Step 4: Verify GREEN and commit**

```bash
cmake --build cmake-build-debug \
  --target input_profile_tests score_provenance_tests -j 6
ctest --test-dir cmake-build-debug \
  -R '^(foundation_input_profile|foundation_provenance_contract)$' \
  --output-on-failure
git diff --check
git add src/input/InputProfile.h src/input/InputProfile.cpp \
  src/input/InputProfileStore.cpp tests/input_profile_tests.cpp \
  tests/fixtures/input/input-future.json src/ScoreProvenance.h \
  src/ScoreProvenance.cpp src/scene/play/GamePlayStartOptions.h \
  tests/score_provenance_tests.cpp
git commit -m "feat(input): persist gyroscope turntable settings"
```

---

### Task 3: Portable backend supervisor and registry contract

**Files:**

- Create: `src/input/GyroscopeInputBackendCore.h`
- Create: `src/input/GyroscopeInputBackendCore.cpp`
- Create: `src/input/GyroscopeInputBackendFactory.h`
- Create: `src/input/GyroscopeInputBackendFactory.cpp`
- Create: `src/input/GyroscopePlatformBackends.h`
- Create: `src/input/UnsupportedGyroscopeInputBackend.cpp`
- Create: `tests/gyroscope_input_backend_core_tests.cpp`
- Modify: `src/input/IInputBackend.h`
- Modify: `src/input/InputDeviceRegistry.h`
- Modify: `src/input/InputDeviceRegistry.cpp`
- Modify: `src/input/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `tests/input_device_registry_tests.cpp`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Backend-core interface:**

```cpp
namespace input {
enum class GyroscopeSensorCommand { None, Start, Stop };

class GyroscopeInputBackendCore {
public:
  explicit GyroscopeInputBackendCore(InputBackendSink sink);
  void start(bool supported, std::uint64_t nowMicros);
  void stop(std::uint64_t nowMicros);
  void setForeground(bool foreground, std::uint64_t nowMicros);
  void sensorStartSucceeded(std::uint64_t nowMicros);
  void sensorStartFailed(std::uint64_t nowMicros);
  void observe(const GyroscopeMotionSample &sample,
               std::uint64_t nowMicros);
  void pump(std::uint64_t nowMicros);
  void configure(GyroscopeTurntableConfig config,
                 std::uint64_t nowMicros);
  void resetSession(std::uint64_t nowMicros);
  [[nodiscard]] GyroscopeSensorCommand takeCommand();
};
} // namespace input
```

The caller serializes core access. `start(true)` requests native sensor start;
`sensorStartSucceeded()` publishes one connected, Calibrating, one-axis
snapshot. A usable fresh sample changes it to Ready. Missing first/fresh sample
for 1 second releases output, publishes disconnected/retrying state, requests
Stop, and schedules Start after 2 seconds while foregrounded. Background reset
releases without claiming hardware removal and requests Stop. Repeated lifecycle
calls and commands are idempotent.

Add no-op hooks to `IInputBackend`:

```cpp
virtual void configureGyroscopeTurntable(
    input::GyroscopeTurntableConfig) {}
virtual void resetGyroscopeTurntableSession() {}
```

Add matching registry methods. Each fans out to all backends, then invokes
`dispatchPending()` so a zero-axis event reaches existing subscribers before
the method returns and without pumping unrelated async backends.

- [ ] **Step 1: Write failing core-supervisor tests**

Use a fake sink and explicit clock. Cover unsupported absence, one-axis
snapshot ordering, Calibrating→Ready, output event fields, calibration loss,
one-second stall, Stop then two-second retry Start, reconnect baseline,
background release/reseed, duplicate lifecycle calls, config release, and
session release.

- [ ] **Step 2: Write failing registry fan-out tests**

Extend `FakeBackend` to record config/reset calls and publish a zero axis event.
Prove all backends receive each call, the subscribed listener sees zero before
the registry call returns, and backend `pump()` counters do not change.

- [ ] **Step 3: Verify RED**

```bash
cmake --build cmake-build-debug \
  --target gyroscope_input_backend_core_tests input_device_registry_tests -j 6
```

Expected: missing backend-core and registry interfaces fail compilation.

- [ ] **Step 4: Implement the portable core/factory**

Mirror `MidiInputBackendFactory.*` and `MidiPlatformBackends.h`. The desktop
unsupported backend starts successfully and publishes no device. Add an
`asobmashow_add_gyroscope_backend(target)` CMake helper and use it for `main`,
`input_capture_controller_tests`, and `input_device_registry_tests`, all of
which compile the registry default constructor. Declare both platform factories
and select the future `IOSGyroscopeInputBackend.mm` or
`AndroidGyroscopeInputBackend.cpp` inside the helper now; those branches are
not evaluated by the desktop build. Discover/link CoreMotion inside the helper
under `if(IOS)`. This freezes platform build wiring before parallel work starts.

- [ ] **Step 5: Verify GREEN and commit**

```bash
cmake --build cmake-build-debug \
  --target gyroscope_input_backend_core_tests input_device_registry_tests \
           input_capture_controller_tests main -j 6
ctest --test-dir cmake-build-debug \
  -R '^(foundation_input_(gyroscope_backend|registry|capture))$' \
  --output-on-failure
git diff --check
git add src/input/GyroscopeInputBackendCore.* \
  src/input/GyroscopeInputBackendFactory.* \
  src/input/GyroscopePlatformBackends.h \
  src/input/UnsupportedGyroscopeInputBackend.cpp src/input/IInputBackend.h \
  src/input/InputDeviceRegistry.* src/input/CMakeLists.txt CMakeLists.txt \
  tests/gyroscope_input_backend_core_tests.cpp \
  tests/input_device_registry_tests.cpp \
  ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat(input): register gyroscope backend lifecycle"
```

---

### Task 4: Active-profile application and gameplay attempt reset

**Files:**

- Modify: `src/context.h`
- Modify: `src/input/InputCaptureController.h`
- Modify: `src/input/InputCaptureController.cpp`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `tests/input_capture_controller_tests.cpp`
- Modify: `tests/profile_switch_tests.cpp`

**Runtime rules:**

- Apply the loaded active profile config once during `ApplicationContext`
  startup.
- In `saveActiveInputProfile`, atomically save first. Only after success, and
  only when the candidate config differs from `context.inputProfile`, configure
  the registry. Ordinary binding edits must not reset gyro state.
- Successful target-profile and rollback loads assign the profile and configure
  the registry together; failed loads retain the prior runtime config.
- Add `InputCaptureController::updateGyroscopeTurntableConfig(config)` using its
  existing transactional `persist(nextProfile)` path so Settings receives the
  same failure state and only updates the referenced profile after a successful
  save.
- Make `context.inputDeviceRegistry.resetGyroscopeTurntableSession()` the first
  statement of `GamePlayScene::reset()`, before `ownedState.reset()`. Never
  reset the complete logical-input pipeline.

- [ ] **Step 1: Add RED transactional controller/profile-switch tests**

Prove a config update failure preserves both referenced profile and registry
config; success updates both exactly once; unchanged binding saves do not
reconfigure; target switch and rollback restore the proper config. The focused
review must verify that the registry reset is literally the first statement of
`GamePlayScene::reset()`; the `main` build is its compile gate.

- [ ] **Step 2: Implement the runtime rules**

Keep `InputCaptureController::persist()` ordering unchanged: callback first,
then profile assignment. Use the save callback to apply runtime config after
disk success, so both binding and settings callers share one atomic boundary.

- [ ] **Step 3: Verify and commit**

```bash
cmake --build cmake-build-debug \
  --target input_capture_controller_tests profile_switch_tests main -j 6
ctest --test-dir cmake-build-debug \
  -R '^(foundation_input_capture|foundation_profile_switch)$' \
  --output-on-failure
git diff --check
git add src/context.h src/input/InputCaptureController.* \
  src/scene/play/GamePlayScene.cpp tests/input_capture_controller_tests.cpp \
  tests/profile_switch_tests.cpp
git commit -m "feat(input): reset gyroscope per gameplay attempt"
```

---

### Task 5: Create platform worktrees and dispatch independent work

Starting from the clean Task 4 commit:

```bash
git worktree add .worktrees/mobile-gyro-settings \
  -b feature/mobile-gyro-settings HEAD
git worktree add .worktrees/mobile-gyro-ios \
  -b feature/mobile-gyro-ios HEAD
git worktree add .worktrees/mobile-gyro-android \
  -b feature/mobile-gyro-android HEAD
git worktree list
```

Dispatch Tasks 6, 7, and 8 concurrently. Each worker must use only its assigned
worktree, run its focused verification, self-review `git diff --check`, commit,
and return the commit hash. Workers must not merge, cherry-pick, or edit the
integration checkout.

---

### Task 6: Device-specific Settings card

**Worktree:** `.worktrees/mobile-gyro-settings`

**Files:**

- Modify: `src/scene/SettingsScene.h`
- Modify: `src/scene/SettingsScene.cpp`
- Modify: `src/scene/SettingsSceneInput.cpp`
- Modify: `tests/input_capture_controller_tests.cpp`
- Modify: `tests/view_layout_tests.cpp`

**Behavior:**

- Add `Gyroscope` to `deviceClassLabel()`.
- Label gyroscope axis 0 as `Turntable`, preserving its `+`/`-` suffix.
- Include gyroscope config, device status, and settings errors in
  `inputViewSignature()` so a device/status/save transition rebuilds only the
  Input view.
- When selected device ID is `builtin:gyroscope-turntable`, insert a dedicated
  card directly after Binding scope. It contains current Ready/Calibrating/
  Disconnected/Retrying status, live Turntable axis value, integer Step angle
  (`°`) and Release delay (`ms`) editors.
- Stack numeric editors on compact layouts. Commit with
  `InputCaptureController::updateGyroscopeTurntableConfig`; an invalid integer
  or failed save leaves the old values/runtime untouched and reports `Not
  saved:` inside this card.
- Do not create a modal or append a transient notification card.

- [ ] **Step 1: Add RED controller and compact-layout/presentation tests**

Controller tests cover failed/successful config persistence. Layout tests cover
stacking at phone width by extending `SettingsSceneInputLayout.h` with:

```cpp
struct GyroscopeSettingsLayout {
  bool stackEditors = false;
  int editorWidth = 0;
};

constexpr GyroscopeSettingsLayout
resolveGyroscopeSettingsLayout(int availableWidth, bool compact);
```

Also cover semantic Turntable labels and all four status strings through
header-level presentation helpers in that file. Do not create a second
settings state owner.

- [ ] **Step 2: Implement the card and verify GREEN**

```bash
cmake --build cmake-build-debug \
  --target input_capture_controller_tests view_layout_tests main -j 6
ctest --test-dir cmake-build-debug \
  -R '^(foundation_input_capture|view_layout_tests)$' \
  --output-on-failure
git diff --check
git add src/scene/SettingsScene.h src/scene/SettingsScene.cpp \
  src/scene/SettingsSceneInput.cpp src/scene/SettingsSceneInputLayout.h \
  tests/input_capture_controller_tests.cpp tests/view_layout_tests.cpp
git commit -m "feat(settings): configure gyroscope turntable"
```

Return the commit hash and focused test output.

---

### Task 7: Core Motion backend for iPhone and iPad

**Worktree:** `.worktrees/mobile-gyro-ios`

**Files:**

- Create: `src/input/IOSGyroscopeInputBackend.mm`
- Create: `src/input/IOSGyroscopeMotionAdapter.h`
- Create: `tests/ios_gyroscope_motion_adapter_tests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow/Info.plist`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Implementation contract:**

- Simulator reports unsupported and publishes no device.
- Own one `CMMotionManager`; prefer
  `CMAttitudeReferenceFrameXArbitraryCorrectedZVertical`, fall back to
  `CMAttitudeReferenceFrameXMagneticNorthZVertical`, and reject other frames.
- Poll only newer `deviceMotion` samples from `pump()`.
- Convert `attitude.yaw` radians to degrees. Compute clockwise world-vertical
  rate as the dot product of `rotationRate` and `gravity`, converted to dps;
  validate the sign on hardware and keep clockwise positive.
- Accept only magnetic accuracy Medium/High. Increment accuracy generation on
  Medium↔High or usable↔unusable transitions.
- Feed Core Motion's monotonic seconds unchanged. Drive backend-core commands,
  config/reset hooks, and idempotent SDL background/foreground events.
- Link `CoreMotion.framework`, add
  `NSMotionUsageDescription = "AsoBMaShow uses motion sensors to let you rotate
  your iPhone or iPad as a turntable controller."`, and add every new source to
  Xcode membership exceptions.

- [ ] **Step 1: Add a header-only platform-neutral adapter test before native code**

Put radians/degrees, rate projection, and accuracy-generation decisions in
`IOSGyroscopeMotionAdapter.h`, which contains no Objective-C types. Register
`ios_gyroscope_motion_adapter_tests` as
`foundation_input_gyroscope_ios_adapter`. Verify radians/degrees, projection
sign fixtures, usable accuracy changes, and simulator-support policy. Confirm
RED before implementing.

- [ ] **Step 2: Implement and compile-check**

```bash
cmake --build cmake-build-debug \
  --target ios_gyroscope_motion_adapter_tests \
           gyroscope_input_backend_core_tests main -j 6
ctest --test-dir cmake-build-debug \
  -R '^foundation_input_gyroscope_(ios_adapter|backend)$' \
  --output-on-failure
plutil -lint ios/Xcode/AsoBMaShow/AsoBMaShow/Info.plist
plutil -lint ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
scripts/ios_firebase_deploy.sh --build-only
git diff --check
git add src/input/IOSGyroscopeInputBackend.mm \
  src/input/IOSGyroscopeMotionAdapter.h \
  tests/ios_gyroscope_motion_adapter_tests.cpp CMakeLists.txt \
  ios/Xcode/AsoBMaShow/AsoBMaShow/Info.plist \
  ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat(ios): add gyroscope turntable backend"
```

If signing/private build prerequisites prevent the build-only script, also run
the script's fastest available compile path and report the exact external
blocker; do not weaken the project to bypass signing.

Return the commit hash and verification output.

---

### Task 8: SensorManager/JNI backend for Android phones and tablets

**Worktree:** `.worktrees/mobile-gyro-android`

**Files:**

- Create: `android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowGyroscopeTurntableManager.java`
- Create: `android/app/src/main/java/com/snurhythm/asobmashow/GyroscopeSensorPolicy.java`
- Create: `android/app/src/test/java/com/snurhythm/asobmashow/GyroscopeSensorPolicyTest.java`
- Create: `src/input/AndroidGyroscopeInputBackend.cpp`
- Modify: `android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowActivity.java`
- Modify: `android/app/src/main/AndroidManifest.xml`

**Java/JNI contract:**

- `GyroscopeSensorPolicy` is framework-free and owns
  `nativeStarted && activityResumed`, generation increments, usable accuracy,
  heading-confidence boundary, and registration decisions.
- Require both default `TYPE_ROTATION_VECTOR` and `TYPE_GYROSCOPE`. Never fall
  back to game/geomagnetic rotation vectors.
- Register both at `SENSOR_DELAY_GAME` on one `HandlerThread` generation.
- Use `getRotationMatrixFromVector`/`getOrientation`; convert azimuth and
  optional `values[4]` heading accuracy from radians to degrees. Medium/High is
  usable only when a present estimate is finite and ≤15°. Increment generation
  on accuracy tier changes, confidence-boundary crossings, or estimate changes
  ≥5°.
- Project the latest gyroscope vector through the rotation matrix onto world Z
  and negate it so clockwise is positive. Pair only a gyro event no more than
  50 ms older than the rotation-vector event.
- Convert `SensorEvent.timestamp` nanoseconds to monotonic seconds. On API 33+
  propagate `firstEventAfterDiscontinuity`.
- Call JNI with the latest generation-tagged sample. Native code uses the
  `AndroidMidiInputBackend.cpp` callback-gate/drain pattern, keeps only one
  mutex-protected latest sample, rejects stale generations, and consumes it in
  registry `pump()`.
- Activity `onPause()` disables/drains gyro before `super.onPause()` and asks
  native to publish zero before returning. `onResume()` calls `super` first,
  then marks the manager resumed. `onDestroy()` performs final stop.
- Add optional gyroscope, compass, and accelerometer manifest features; add no
  permission.

- [ ] **Step 1: Write framework-free JVM tests and verify RED**

Cover start/resume truth table, idempotence, generation invalidation, Low/
Unreliable rejection, Medium/High and 15-degree boundary, radians conversion,
5-degree material change, ±90/wrap azimuth, and stale-callback rejection.

```bash
ANDROID_HOME=/opt/homebrew/share/android-commandlinetools \
ANDROID_SDK_ROOT=/opt/homebrew/share/android-commandlinetools \
ANDROID_NDK_HOME=/opt/homebrew/share/android-commandlinetools/ndk/28.2.13676358 \
VCPKG_ROOT=/Users/xf/vcpkg JAVA_HOME=$(/usr/libexec/java_home -v 17) \
SDL/android-project/gradlew -p android :app:testPlayDebugUnitTest --no-daemon
```

Expected: tests fail because `GyroscopeSensorPolicy` is absent.

- [ ] **Step 2: Implement Java manager, lifecycle, and JNI backend**

Keep Android framework calls in the manager and all decision logic in the pure
policy. Revoke the native gate before Java stop and wait for active callbacks
before backend destruction.

- [ ] **Step 3: Verify GREEN and commit**

```bash
ANDROID_HOME=/opt/homebrew/share/android-commandlinetools \
ANDROID_SDK_ROOT=/opt/homebrew/share/android-commandlinetools \
ANDROID_NDK_HOME=/opt/homebrew/share/android-commandlinetools/ndk/28.2.13676358 \
VCPKG_ROOT=/Users/xf/vcpkg JAVA_HOME=$(/usr/libexec/java_home -v 17) \
SDL/android-project/gradlew -p android :app:testPlayDebugUnitTest --no-daemon
xmllint --noout android/app/src/main/AndroidManifest.xml
scripts/android_firebase_deploy.sh --build-only --variant playDebug
git diff --check
git add android/app/src/main/java/com/snurhythm/asobmashow/ \
  android/app/src/test/java/com/snurhythm/asobmashow/GyroscopeSensorPolicyTest.java \
  android/app/src/main/AndroidManifest.xml \
  src/input/AndroidGyroscopeInputBackend.cpp
git commit -m "feat(android): add gyroscope turntable backend"
```

Return the commit hash and verification output.

---

### Task 9: Integrate, review, and verify all platforms

**Files:** all files changed by Tasks 1–8.

- [ ] **Step 1: Cherry-pick worktree commits one at a time**

On `feature/mobile-gyroscope-turntable`, verify the changed-file list for each
returned commit, cherry-pick Settings, iOS, then Android, and run the affected
focused tests after every pick. Resolve only integration conflicts; do not
rewrite a platform worker's unrelated files.

- [ ] **Step 2: Run code/spec compliance reviews in parallel**

Assign one reviewer to portable/profile/runtime behavior, one to iOS, and one
to Android/Settings. Address Critical and Important findings test-first in the
owning worktree or a new focused remediation worktree, then cherry-pick the
reviewed fix.

- [ ] **Step 3: Run final automated verification**

```bash
cmake --build cmake-build-debug --target main -j 6
ctest --test-dir cmake-build-debug --output-on-failure

ANDROID_HOME=/opt/homebrew/share/android-commandlinetools \
ANDROID_SDK_ROOT=/opt/homebrew/share/android-commandlinetools \
ANDROID_NDK_HOME=/opt/homebrew/share/android-commandlinetools/ndk/28.2.13676358 \
VCPKG_ROOT=/Users/xf/vcpkg JAVA_HOME=$(/usr/libexec/java_home -v 17) \
SDL/android-project/gradlew -p android :app:testPlayDebugUnitTest --no-daemon

scripts/ios_firebase_deploy.sh --build-only
scripts/android_firebase_deploy.sh --build-only --variant playDebug
plutil -lint ios/Xcode/AsoBMaShow/AsoBMaShow/Info.plist
plutil -lint ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
xmllint --noout android/app/src/main/AndroidManifest.xml
git diff --check
git status --short --branch
```

Expected: all local tests/builds pass, no upload occurs, and the integration
branch is clean. If a build-only check is externally blocked, report its exact
command/output and keep every locally testable gate green.

- [ ] **Step 4: Record physical-hardware acceptance as pending**

Automated completion does not claim sensor behavior on hardware. The handoff
must explicitly list pending tests on one iPhone, iPad, Android phone, and
Android tablet: flat screen-up in both landscape orientations, slow/fast turns,
both directions, multiple wrap crossings, stationary magnetic drift, magnetic
accuracy changes, reversal, retry/practice reset, and background/foreground.

- [ ] **Step 5: Remove only completed feature worktrees**

After every commit is integrated and verified:

```bash
git worktree remove .worktrees/mobile-gyro-settings
git worktree remove .worktrees/mobile-gyro-ios
git worktree remove .worktrees/mobile-gyro-android
git worktree prune
git worktree list
```

Do not delete the three topic branches until the integration handoff is
accepted.
