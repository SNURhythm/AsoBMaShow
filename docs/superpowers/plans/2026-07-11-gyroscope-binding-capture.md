# Gyroscope Binding Capture Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make SDL axis and gyroscope binding capture activate at 20% motion, re-arm at 10%, and use matching activation/release defaults without rewriting valid existing profiles.

**Architecture:** Keep SDL normalization and the persisted schema unchanged. Define one canonical `0.20`/`0.10` default pair in the input model, reuse it in profile recovery and missing-field loading, and add axis-only hysteresis inside `InputCaptureController`; every non-axis capture keeps its existing 50% edge.

**Tech Stack:** C++23, SDL2 input events, nlohmann JSON profile persistence, CMake, CTest.

## Global Constraints

- Axis capture activation is inclusive at normalized magnitude `0.20`.
- An active axis direction re-arms at normalized magnitude `0.10` or below; values between `0.10` and `0.20` preserve prior state.
- Non-axis capture remains at the existing normalized `0.50` threshold.
- Canonical binding defaults and recovery values are `activationThreshold = 0.20` and `releaseThreshold = 0.10`.
- Valid explicit thresholds already present in user profiles are never migrated or overwritten.
- Do not change SDL normalization, gameplay resolver hysteresis logic, the input-profile schema version, or serialized field names.
- Positive and negative axis directions retain independent activation state.

---

### Task 1: Canonical binding activation and release defaults

**Files:**
- Modify: `tests/input_profile_tests.cpp:143-371`
- Modify: `src/input/InputTypes.h:53-62`
- Modify: `src/input/InputProfile.cpp:9-73`
- Modify: `src/input/InputProfileStore.cpp:209-228`

**Interfaces:**
- Produces: `input::kDefaultBindingActivationThreshold` as an `inline constexpr float` equal to `0.20F`.
- Produces: `input::kDefaultBindingReleaseThreshold` as an `inline constexpr float` equal to `0.10F`.
- Preserves: `InputProfileStore::load(const std::filesystem::path&)` and the JSON schema.

- [ ] **Step 1: Write failing tests for canonical, repaired, missing, and explicit values**

In `tests/input_profile_tests.cpp`, add exact default assertions immediately after constructing `defaults`:

```cpp
const input::InputBinding canonicalDefaults;
require(canonicalDefaults.activationThreshold == 0.20F &&
            canonicalDefaults.releaseThreshold == 0.10F,
        "new bindings use the sensitive threshold defaults");
for (const auto &binding : defaults.bindings) {
  require(binding.activationThreshold == 0.20F &&
              binding.releaseThreshold == 0.10F,
          "default profile bindings use the canonical thresholds");
}
```

After sanitizing `invalidProfile`, require exact recovery values and add a second finite-but-misordered case:

```cpp
require(invalidProfile.bindings.front().activationThreshold == 0.20F &&
            invalidProfile.bindings.front().releaseThreshold == 0.10F,
        "non-finite thresholds recover to the canonical defaults");

input::InputBinding invalidOrder = defaults.bindings.front();
invalidOrder.deadZone = 0.0F;
invalidOrder.releaseThreshold = 0.9F;
invalidOrder.activationThreshold = 0.2F;
InputProfile invalidOrderProfile{.bindings = {invalidOrder}};
diagnostics.clear();
invalidOrderProfile.sanitize(diagnostics);
require(invalidOrderProfile.bindings.front().activationThreshold == 0.20F &&
            invalidOrderProfile.bindings.front().releaseThreshold == 0.10F,
        "misordered thresholds recover to the canonical defaults");
```

After loading `fixtureResult`, prove its explicit `0.60`/`0.40` values survive. After loading `repairedIdsResult`, whose bindings omit both fields, prove each receives `0.20`/`0.10`:

```cpp
require(fixtureResult.profile.bindings.front().activationThreshold == 0.60F &&
            fixtureResult.profile.bindings.front().releaseThreshold == 0.40F,
        "valid explicit thresholds remain unchanged");

for (const auto &binding : repairedIdsResult.profile.bindings) {
  require(binding.activationThreshold == 0.20F &&
              binding.releaseThreshold == 0.10F,
          "missing threshold fields load with canonical defaults");
}
```

- [ ] **Step 2: Run the focused profile test and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target input_profile_tests -j 6
ctest --test-dir cmake-build-debug -R '^foundation_input_profile$' --output-on-failure
```

Expected: `foundation_input_profile` fails at `new bindings use the sensitive threshold defaults`, showing the current `0.50`/`0.35` values.

- [ ] **Step 3: Add shared constants and use them everywhere defaults are selected**

In `src/input/InputTypes.h`, define the canonical values in namespace `input` and use them in `InputBinding`:

```cpp
inline constexpr float kDefaultBindingActivationThreshold = 0.20F;
inline constexpr float kDefaultBindingReleaseThreshold = 0.10F;

struct InputBinding {
  std::string id;
  InputScope scope;
  LogicalAction action;
  PhysicalControl control;
  float deadZone = 0.0F;
  float activationThreshold = kDefaultBindingActivationThreshold;
  float releaseThreshold = kDefaultBindingReleaseThreshold;
  bool inverted = false;
};
```

In `src/input/InputProfile.cpp`, add and reuse one reset helper for both non-finite and misordered thresholds:

```cpp
void resetThresholdsToDefaults(input::InputBinding &binding) {
  binding.deadZone = 0.0F;
  binding.releaseThreshold = input::kDefaultBindingReleaseThreshold;
  binding.activationThreshold = input::kDefaultBindingActivationThreshold;
}
```

Replace both literal reset blocks with `resetThresholdsToDefaults(binding)`. Keep clamping and `hasValidThresholdOrder` unchanged so valid explicit values pass through untouched.

In `src/input/InputProfileStore.cpp`, replace missing-field literals with the shared constants:

```cpp
binding.activationThreshold = document.value(
    "activationThreshold", input::kDefaultBindingActivationThreshold);
binding.releaseThreshold = document.value(
    "releaseThreshold", input::kDefaultBindingReleaseThreshold);
```

- [ ] **Step 4: Run the focused profile test and verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target input_profile_tests -j 6
ctest --test-dir cmake-build-debug -R '^foundation_input_profile$' --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 1`.

- [ ] **Step 5: Commit the canonical defaults**

```bash
git add tests/input_profile_tests.cpp src/input/InputTypes.h src/input/InputProfile.cpp src/input/InputProfileStore.cpp
git commit -m "fix(input): lower default binding thresholds"
```

---

### Task 2: Axis-specific capture hysteresis

**Files:**
- Modify: `tests/input_capture_controller_tests.cpp:28-670`
- Modify: `src/input/InputCaptureController.h:48-56`
- Modify: `src/input/InputCaptureController.cpp:241-273`

**Interfaces:**
- Consumes: `input::InputBinding` defaults from Task 1, so captured bindings persist `0.20`/`0.10`.
- Preserves: `InputCaptureController::begin`, capture callbacks, conflict behavior, and all public types.
- Produces: axis capture activation at `0.20F` and re-arm at `0.10F`; non-axis capture remains `0.50F`.

- [ ] **Step 1: Write failing positive, negative, hysteresis, and non-axis tests**

In `testMonitoringNoiseActivationRepeatsAndDuplicateIgnore`, replace the positive-axis boundary samples with `0.19F` and `0.20F`, then assert the captured binding defaults:

```cpp
harness.input(event(axisControl("stick:one", 2), 0.19F, 11));
require(profile.bindings.empty(),
        "an axis remains unbound below the sensitive activation threshold");
harness.input(event(axisControl("stick:one", 2), 0.20F, 12));
require(controller.state() == InputCaptureController::State::Idle &&
            profile.bindings.size() == 1 && saves == 1,
        "an axis binds at the sensitive activation threshold");
require(profile.bindings.front().activationThreshold == 0.20F &&
            profile.bindings.front().releaseThreshold == 0.10F,
        "captured axes inherit the sensitive gameplay thresholds");
```

Add a focused hysteresis test:

```cpp
void testAxisCaptureUsesSensitiveHysteresis() {
  RegistryHarness harness;
  InputProfile profile;
  int saves = 0;
  InputCaptureController controller(harness.registry, profile,
                                    [&](const InputProfile &, std::string &) {
                                      ++saves;
                                      return true;
                                    });
  const auto axis = axisControl("gyro:one", 1);

  controller.begin({1, 7}, lane(0));
  harness.input(event(axis, 0.20F));
  require(profile.bindings.size() == 1 && saves == 1,
          "light deliberate gyro motion creates a binding");

  controller.begin({1, 7}, lane(1));
  harness.input(event(axis, 0.15F));
  harness.input(event(axis, 0.20F));
  require(controller.state() == InputCaptureController::State::Listening &&
              saves == 1,
          "mid-band gyro noise does not re-arm an active direction");

  harness.input(event(axis, 0.10F));
  harness.input(event(axis, 0.20F));
  require(controller.state() ==
              InputCaptureController::State::AwaitingConflictConfirmation,
          "returning to the release threshold re-arms the gyro direction");
  controller.rejectReplace();
}
```

Change the negative-axis boundary samples in `testNegativeAxisHatAndMidiCapturePreserveControlIdentity` from `-0.49F`/`-0.50F` to `-0.19F`/`-0.20F`.

Add this helper near `midiNoteControl`:

```cpp
input::PhysicalControl midiControlControl(std::string deviceId, int index) {
  return {.deviceId = std::move(deviceId),
          .deviceClass = input::DeviceClass::Midi,
          .kind = input::ControlKind::MidiControl,
          .index = index,
          .direction = input::ControlDirection::Any};
}
```

Add a non-axis regression test proving MIDI control capture still requires 50%:

```cpp
void testNonAxisCaptureThresholdRemainsUnchanged() {
  RegistryHarness harness;
  InputProfile profile;
  InputCaptureController controller(harness.registry, profile,
                                    [](const InputProfile &, std::string &) {
                                      return true;
                                    });

  controller.begin({1, 7}, lane(0));
  harness.input(event(midiControlControl("midi:one", 7), 0.20F));
  require(profile.bindings.empty() &&
              controller.state() == InputCaptureController::State::Listening,
          "MIDI control capture does not inherit the axis threshold");
  harness.input(event(midiControlControl("midi:one", 7), 0.50F));
  require(profile.bindings.size() == 1,
          "MIDI control capture retains the existing threshold");
}
```

Call both new tests from `main()`.

- [ ] **Step 2: Run the focused capture test and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target input_capture_controller_tests -j 6
ctest --test-dir cmake-build-debug -R '^foundation_input_capture$' --output-on-failure
```

Expected: `foundation_input_capture` fails because `0.20F` axis motion does not bind under the current `0.50F` capture threshold.

- [ ] **Step 3: Implement axis-only capture hysteresis**

Replace the single private threshold in `src/input/InputCaptureController.h` with:

```cpp
static constexpr float kNonAxisCaptureActivationThreshold = 0.50F;
static constexpr float kAxisCaptureActivationThreshold = 0.20F;
static constexpr float kAxisCaptureReleaseThreshold = 0.10F;
```

Update `considerControlActivation` in `src/input/InputCaptureController.cpp` without changing its signature:

```cpp
const bool wasActive = activationStates_[control];
const bool active =
    control.kind == input::ControlKind::Axis
        ? (wasActive ? value > kAxisCaptureReleaseThreshold
                     : value >= kAxisCaptureActivationThreshold)
        : value >= kNonAxisCaptureActivationThreshold;
activationStates_[control] = active;
if (wasActive || !active || state_ != State::Listening) {
  return;
}
stageCandidate(std::move(control));
```

This keeps the existing signed-axis split in `considerCandidate`: positive and negative directions continue to use different `PhysicalControl` map keys.

- [ ] **Step 4: Run focused and related input tests and verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target input_capture_controller_tests input_binding_resolver_tests logical_gameplay_input_tests midi_input_tests -j 6
ctest --test-dir cmake-build-debug -R '^foundation_input_(capture|resolver|gameplay|midi)$' --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 4`.

- [ ] **Step 5: Commit the capture hysteresis**

```bash
git add tests/input_capture_controller_tests.cpp src/input/InputCaptureController.h src/input/InputCaptureController.cpp
git commit -m "fix(input): add sensitive axis capture hysteresis"
```

---

### Task 3: Integrated verification

**Files:**
- Verify only: all files changed in Tasks 1 and 2

**Interfaces:**
- Consumes: canonical binding defaults and axis capture hysteresis from Tasks 1 and 2.
- Produces: fresh build, focused regression, full-suite, and repository-cleanliness evidence.

- [ ] **Step 1: Reconfigure and build all desktop targets**

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug -j 6
```

Expected: both commands exit `0`; warnings from existing third-party targets are allowed, but no compilation or linker error is allowed.

- [ ] **Step 2: Run the complete input foundation subset**

```bash
ctest --test-dir cmake-build-debug -R '^foundation_input_(profile|capture|resolver|registry|gameplay|midi)$' --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 6`.

- [ ] **Step 3: Run the full CTest suite**

```bash
ctest --test-dir cmake-build-debug --output-on-failure -j 6
```

Expected: zero failed tests; the repository's existing explicitly skipped Yoga tests may remain skipped.

- [ ] **Step 4: Verify the finished branch state**

```bash
git diff --check
git status --short --branch
git log -3 --oneline
```

Expected: `git diff --check` exits `0`, the worktree has no uncommitted paths, and the latest commits include the defaults and capture-hysteresis changes.
