# Configurable Input Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace hard-coded gameplay keyboard maps with versioned per-profile bindings for keyboard, controllers, analog scratch, touch, and MIDI while preserving existing gameplay and replay semantics.

**Architecture:** Hardware backends normalize events into `PhysicalInputEvent`; a deterministic `InputBindingResolver` maps those events to device-independent logical transitions; a thin adapter feeds the existing rhythm control API. Profile serialization, capture/conflict logic, and UI consume the same shared types, while native callbacks only enqueue work for main-thread delivery.

**Tech Stack:** C++23, SDL 2.32, nlohmann/json already vendored under Yoga, CoreMIDI, Windows Multimedia MIDI, Android `MidiManager`/JNI, CMake/CTest.

## Global Constraints

- Work only in task worktrees and branches created from `feature/player-foundations`; never edit or commit on `develop`.
- Follow red-green-refactor for every behavior-bearing task and register every new executable with CTest.
- Persist keyboard controls as SDL scancodes, not keycodes, so bindings remain physical-layout based like the current implementation.
- Preserve the current touch/flick/drag path and replay touch sampling unchanged.
- Replays contain logical actions only; neither replays nor score provenance may store physical device IDs.
- All backend callbacks enqueue data; registered listeners run only from `InputDeviceRegistry::pump()` on the main thread.
- A disconnected device preserves bindings and releases all logical actions it currently holds.
- iOS source additions under `src/` must be added to `membershipExceptions` in `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`.
- Linux remains interface-compatible but is not a release-verification target for this milestone.
- Android MIDI is optional at runtime and must not raise the current API 28 minimum.

---

### Task 1: Versioned input contracts, defaults, and persistence

**Files:**
- Create: `src/input/InputTypes.h`
- Create: `src/input/InputProfile.h`
- Create: `src/input/InputProfile.cpp`
- Create: `src/input/InputDefaults.cpp`
- Create: `src/input/InputProfileStore.h`
- Create: `src/input/InputProfileStore.cpp`
- Create: `tests/input_profile_tests.cpp`
- Create: `tests/fixtures/input/input-v1.json`
- Create: `tests/fixtures/input/input-future.json`
- Modify: `src/input/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: current keyboard defaults from `RhythmInputHandler.cpp:445-528`; active profile supplies the eventual `input.json` path.
- Produces: `input::InputBinding`, `InputProfile`, `makeDefaultInputProfile()`, and `InputProfileStore::{load,saveAtomic}` for all later input tasks.

- [ ] **Step 1: Write the failing profile/default tests**

Create a standalone assertion test whose central checks are:

```cpp
#include "input/InputProfile.h"
#include "input/InputProfileStore.h"
#include <SDL2/SDL_scancode.h>
#include <cassert>

int main() {
  const InputProfile defaults = makeDefaultInputProfile();
  assert(defaults.schemaVersion == InputProfile::kSchemaVersion);
  assert(defaults.hasDigitalBinding({1, 7}, {input::LogicalActionKind::Lane, 0},
                                    "keyboard", SDL_SCANCODE_S));
  assert(defaults.hasDigitalBinding({1, 7}, {input::LogicalActionKind::Lane, 7},
                                    "keyboard", SDL_SCANCODE_LSHIFT));
  assert(defaults.hasDigitalBinding({1, 7}, {input::LogicalActionKind::Lane, 7},
                                    "keyboard", SDL_SCANCODE_RSHIFT));
  assert(defaults.hasDigitalBinding({2, 14}, {input::LogicalActionKind::Lane, 8},
                                    "keyboard", SDL_SCANCODE_M));

  input::InputBinding invalid = defaults.bindings.front();
  invalid.releaseThreshold = 0.9f;
  invalid.activationThreshold = 0.2f;
  InputProfile profile{.bindings = {invalid}};
  std::vector<std::string> diagnostics;
  profile.sanitize(diagnostics);
  assert(profile.bindings.front().releaseThreshold <
         profile.bindings.front().activationThreshold);
}
```

Add additional assertions for every 4/5/6/7/8/10/14-key default, DP player scope, missing-file defaults, JSON round trip with a missing device ID, malformed JSON, future schema, duplicate suppression, conflict detection, and atomic-save preservation of the prior file after a forced rename failure.

- [ ] **Step 2: Wire and verify the red test**

Add `input_profile_tests` with `InputProfile.cpp`, `InputDefaults.cpp`, and `InputProfileStore.cpp`, register it as `foundation_input_profile`, then run:

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target input_profile_tests -j 6
```

Expected: compilation fails because the new headers/types do not exist.

- [ ] **Step 3: Add the physical and logical type contract**

Implement `src/input/InputTypes.h` with these exact public types:

```cpp
#pragma once
#include <compare>
#include <cstdint>
#include <string>

namespace input {
enum class DeviceClass { Keyboard, GameController, Joystick, Touch, Midi };
enum class ControlKind { Key, Button, Axis, Hat, TouchRegion, MidiNote, MidiControl };
enum class ControlDirection { Any, Negative, Positive, Up, Right, Down, Left };
enum class LogicalActionKind {
  Lane, ScratchClockwise, ScratchCounterClockwise, Start, Select, Pause, Retry,
  LaneCoverIncrease, LaneCoverDecrease
};

struct InputScope {
  int player = 1;
  int keyMode = 7;
  auto operator<=>(const InputScope &) const = default;
};
struct LogicalAction {
  LogicalActionKind kind = LogicalActionKind::Lane;
  int lane = 0;
  auto operator<=>(const LogicalAction &) const = default;
};
struct PhysicalControl {
  std::string deviceId;
  DeviceClass deviceClass = DeviceClass::Keyboard;
  ControlKind kind = ControlKind::Key;
  int index = 0;
  ControlDirection direction = ControlDirection::Any;
  auto operator<=>(const PhysicalControl &) const = default;
};
struct InputBinding {
  std::string id;
  InputScope scope;
  LogicalAction action;
  PhysicalControl control;
  float deadZone = 0.0f;
  float activationThreshold = 0.5f;
  float releaseThreshold = 0.35f;
  bool inverted = false;
};
struct PhysicalInputEvent {
  PhysicalControl control;
  double rawValue = 0.0;
  float normalizedValue = 0.0f;
  std::uint64_t timestampMicros = 0;
};
struct LogicalInputTransition {
  InputScope scope;
  LogicalAction action;
  bool pressed = false;
  float value = 0.0f;
};
struct InputDeviceSnapshot {
  std::string stableId;
  std::string displayName;
  DeviceClass deviceClass = DeviceClass::Keyboard;
  bool connected = false;
  int buttons = 0;
  int axes = 0;
  int hats = 0;
};
} // namespace input
```

- [ ] **Step 4: Implement the profile API and current defaults**

Expose:

```cpp
struct InputProfile {
  static constexpr int kSchemaVersion = 1;
  int schemaVersion = kSchemaVersion;
  std::vector<input::InputBinding> bindings;

  void sanitize(std::vector<std::string> &diagnostics);
  std::vector<std::reference_wrapper<const input::InputBinding>>
  bindingsFor(input::InputScope scope) const;
  std::vector<input::InputBinding>
  conflictsWith(const input::InputBinding &candidate) const;
  bool hasDigitalBinding(input::InputScope, input::LogicalAction,
                         std::string_view deviceId, int scancode) const;
};
InputProfile makeDefaultInputProfile();
```

Use scancodes equivalent to the existing maps: `S D F Space J K L` plus both Shifts for 7K; `A S D F J K L Semicolon` for 8K; `S D F J K L` for 6K; `D F Space J K` plus both Shifts for 5K; `D F J K` for 4K; `Z S X D C F V`/`M K Comma L Period Semicolon Slash` and left/right Shift for 14K; `Z S X D C`/`Comma L Period Semicolon Slash` and left/right Shift for 10K.

Sanitize player to 1 or 2, known key modes to 4/5/6/7/8/10/14, normalized values to finite ranges, and thresholds to `0 <= deadZone < releaseThreshold < activationThreshold <= 1`. Remove only exact duplicate bindings; retain missing device IDs.

- [ ] **Step 5: Implement JSON load and atomic save**

Expose:

```cpp
enum class InputProfileLoadStatus { Loaded, MissingDefaults, InvalidDocument, FutureVersion };
struct InputProfileLoadResult {
  InputProfileLoadStatus status = InputProfileLoadStatus::MissingDefaults;
  InputProfile profile;
  std::vector<std::string> diagnostics;
};
class InputProfileStore {
public:
  static InputProfileLoadResult load(const std::filesystem::path &path);
  static bool saveAtomic(const std::filesystem::path &path,
                         const InputProfile &profile,
                         std::string &errorMessage);
};
```

Serialize `schemaVersion: 1` and a `bindings` array. MIDI indices use `channel * 128 + noteOrController`. Save to `<name>.tmp`, flush/close, rename the old file to `<name>.bak`, rename the temporary file into place, and restore the backup if the final rename fails. A future-version or invalid document is never rewritten during load.

- [ ] **Step 6: Verify green and commit**

```bash
cmake --build cmake-build-debug --target input_profile_tests -j 6
ctest --test-dir cmake-build-debug -R '^foundation_input_profile$' --output-on-failure
git add src/input tests/input_profile_tests.cpp tests/fixtures/input CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat(input): add versioned input profiles"
```

Expected: the focused test passes and no unrelated file is staged.

---

### Task 2: Deterministic scoped binding resolver

**Files:**
- Create: `src/input/InputBindingResolver.h`
- Create: `src/input/InputBindingResolver.cpp`
- Create: `tests/input_binding_resolver_tests.cpp`
- Modify: `src/input/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: `InputProfile`, `input::PhysicalInputEvent`, and active `InputScope` values from Task 1.
- Produces: batched `LogicalInputTransition` values and category-only active device provenance.

- [ ] **Step 1: Write the failing resolver test matrix**

Create a standalone test with a callback vector and assertions following this pattern:

```cpp
std::vector<std::vector<input::LogicalInputTransition>> batches;
InputBindingResolver resolver(profile, {{1, 7}}, {
  .onTransitions = [&](std::span<const input::LogicalInputTransition> value) {
    batches.emplace_back(value.begin(), value.end());
  }
});
resolver.consume(keyEvent("keyboard", SDL_SCANCODE_S, true));
assert(batches.size() == 1 && batches[0][0].pressed);
resolver.consume(keyEvent("keyboard", SDL_SCANCODE_S, true));
assert(batches.size() == 1); // repeat suppressed
```

Add exact cases for player/key-mode filtering, press/release, two controls bound to one action, dead-zone rescaling, hysteresis, inversion, positive/negative axis directions, one-batch sign reversal, hats, MIDI CC threshold, disconnect release, and `activeDeviceClasses()` returning categories without IDs.

- [ ] **Step 2: Register and verify red**

Create target `input_binding_resolver_tests`, register `foundation_input_resolver`, and run it. Expected: compile failure for missing resolver.

- [ ] **Step 3: Implement the resolver contract**

```cpp
class InputBindingResolver {
public:
  enum class Mode { Gameplay, Capture };
  struct Callbacks {
    std::function<void(std::span<const input::LogicalInputTransition>)> onTransitions;
    std::function<void(const input::PhysicalInputEvent &)> onMonitorSample;
    std::function<void(const input::PhysicalInputEvent &)> onCaptureCandidate;
  };

  InputBindingResolver(const InputProfile &, std::vector<input::InputScope>,
                       Callbacks);
  void setMode(Mode);
  void consume(const input::PhysicalInputEvent &);
  void disconnectDevice(std::string_view stableId);
  void reset();
  std::set<input::DeviceClass> activeDeviceClasses() const;
};
```

Track physical active states separately from logical reference counts. Apply inversion, radial/sign dead-zone removal and rescaling, then activation/release hysteresis. Evaluate both directions of an axis before emitting one transition batch. In capture mode emit monitor/candidate callbacks without gameplay transitions.

- [ ] **Step 4: Verify green and commit**

```bash
cmake --build cmake-build-debug --target input_binding_resolver_tests -j 6
ctest --test-dir cmake-build-debug -R '^foundation_input_resolver$' --output-on-failure
git add src/input tests/input_binding_resolver_tests.cpp CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat(input): resolve scoped digital and analog bindings"
```

---

### Task 3: SDL device registry and stable identities

**Files:**
- Create: `src/input/IInputBackend.h`
- Create: `src/input/InputDeviceIdentity.h`
- Create: `src/input/InputDeviceIdentity.cpp`
- Create: `src/input/InputDeviceRegistry.h`
- Create: `src/input/InputDeviceRegistry.cpp`
- Create: `src/input/SDLInputBackend.h`
- Create: `src/input/SDLInputBackend.cpp`
- Create: `tests/input_device_registry_tests.cpp`
- Modify: `src/context.h`
- Modify: `src/main.cpp`
- Modify: `src/input/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: SDL events and the physical input contract.
- Produces: a main-thread registry subscription API used by gameplay, capture, and MIDI backends.

- [ ] **Step 1: Write registry and identity tests first**

Use a fake `IInputBackend` to assert that keyboard is always `keyboard`, reconnecting an SDL instance retains its stable ID, removal publishes `connected=false`, identical serial-less devices get distinct ordinals, controller events are not duplicated as raw joystick events, `-32768` normalizes exactly to `-1.0f`, listeners run only during `pump()`, and unsubscribe prevents later callbacks.

- [ ] **Step 2: Register and verify red**

Create `input_device_registry_tests`, register `foundation_input_registry`, and confirm missing-header compilation failure.

- [ ] **Step 3: Implement the backend and registry APIs**

```cpp
class IInputBackend {
public:
  virtual ~IInputBackend() = default;
  virtual bool start(std::string &errorMessage) = 0;
  virtual void stop() = 0;
  virtual void handleSdlEvent(const SDL_Event &) {}
  virtual void pump() = 0;
};

class InputDeviceRegistry {
public:
  using InputListener = std::function<void(const input::PhysicalInputEvent &)>;
  using DeviceListener = std::function<void(const input::InputDeviceSnapshot &)>;
  InputDeviceRegistry();
  ~InputDeviceRegistry();
  std::uint64_t subscribeInput(InputListener);
  std::uint64_t subscribeDevices(DeviceListener);
  void unsubscribe(std::uint64_t token);
  void handleSdlEvent(const SDL_Event &);
  void pump();
  std::vector<input::InputDeviceSnapshot> snapshot() const;
  bool isConnected(std::string_view stableId) const;
};
```

Stable SDL IDs use, in order: `sdl:<guid>:serial:<serial>`, `sdl:<guid>:path:<sha256>`, or `sdl:<guid>:name:<normalized-name>:<ordinal>`. Do not use volatile instance IDs in persistence.

- [ ] **Step 4: Integrate the SDL event path**

Add the registry to `ApplicationContext` after SDL initialization. Feed every SDL event to `handleSdlEvent()` before scene filtering, then call `pump()` once per foreground or background event-loop iteration. Keep controller/joystick events out of scene dispatch because the registry owns them.

- [ ] **Step 5: Verify and commit**

```bash
cmake --build cmake-build-debug --target input_device_registry_tests main -j 6
ctest --test-dir cmake-build-debug -R '^foundation_input_registry$' --output-on-failure
git add src/input src/context.h src/main.cpp tests/input_device_registry_tests.cpp CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat(input): add SDL device registry"
```

---

### Task 4: Logical gameplay adapter and compatibility migration

**Files:**
- Create: `src/input/LogicalGameplayInputAdapter.h`
- Create: `src/input/LogicalGameplayInputAdapter.cpp`
- Create: `tests/logical_gameplay_input_tests.cpp`
- Modify: `src/input/RhythmInputHandler.h`
- Modify: `src/input/RhythmInputHandler.cpp`
- Modify: `src/scene/play/GamePlayScene.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/scene/SettingsScenePreview.cpp`
- Modify: `src/input/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: logical transition batches from Task 2 and registry subscriptions from Task 3.
- Produces: existing `IRhythmControl` lane calls and scene commands without changing replay lane/touch representation.

- [ ] **Step 1: Write adapter tests first**

Use a recording `IRhythmControl` fake to assert lane press/release, scratch reversal ordering, normal scratch release with `isBackSpin=false`, non-lane commands sent only to the command callback, and DP lane numbers remaining 0–15.

- [ ] **Step 2: Implement the adapter**

```cpp
class LogicalGameplayInputAdapter {
public:
  using CommandCallback = std::function<void(const input::LogicalInputTransition &)>;
  LogicalGameplayInputAdapter(IRhythmControl &, CommandCallback);
  void apply(std::span<const input::LogicalInputTransition> transitions);
  void reset();
};
```

Release the previously held scratch direction before pressing the opposite direction. Forward lane actions only to `IRhythmControl`; route Start/Select/Pause/Retry/lane-cover actions only to `CommandCallback`.

- [ ] **Step 3: Replace hard-coded gameplay listening**

Construct `RhythmInputHandler` with `InputDeviceRegistry&`, `const InputProfile&`, and active scopes. SP uses `{1, keyMode}`; 10K/14K use both `{1, keyMode}` and `{2, keyMode}`. Replace `startListenSDL()` with a registry subscription, but retain `SDLTouchInputSource`, iOS pending-touch pumping, lane geometry, flick thresholds, drag mode, and replay touch callbacks. Keep Escape as a compatibility pause fallback.

- [ ] **Step 4: Verify old and new paths**

```bash
cmake --build cmake-build-debug --target logical_gameplay_input_tests main -j 6
ctest --test-dir cmake-build-debug -R '^(foundation_input_gameplay|gbattle_tests|replay_)' --output-on-failure
git add src/input src/scene tests/logical_gameplay_input_tests.cpp CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat(input): route configurable actions into gameplay"
```

---

### Task 5: MIDI normalization and platform adapters

**Files:**
- Create: `src/input/MidiMessageParser.h`
- Create: `src/input/MidiMessageParser.cpp`
- Create: `src/input/MidiInputBackendFactory.h`
- Create: `src/input/MidiInputBackendFactory.cpp`
- Create: `src/input/CoreMidiInputBackend.mm`
- Create: `src/input/WinMidiInputBackend.cpp`
- Create: `src/input/AndroidMidiInputBackend.cpp`
- Create: `src/input/UnsupportedMidiInputBackend.cpp`
- Create: `tests/midi_input_tests.cpp`
- Create: `android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowMidiManager.java`
- Modify: `android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowActivity.java`
- Modify: `android/app/src/main/AndroidManifest.xml`
- Modify: `src/input/InputDeviceRegistry.cpp`
- Modify: `src/input/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: `IInputBackend`, registry queue, and physical event types.
- Produces: normalized MIDI notes/controls with canonical `channel * 128 + number` indices.

- [ ] **Step 1: Write the parser tests first**

```cpp
MidiMessageParser parser;
auto noteOn = parser.consume("midi:test", std::array<std::uint8_t,3>{0x92, 60, 127}, 10);
assert(noteOn.size() == 1);
assert(noteOn[0].control.kind == input::ControlKind::MidiNote);
assert(noteOn[0].control.index == 2 * 128 + 60);
assert(noteOn[0].normalizedValue == 1.0f);
```

Add note-off, note-on velocity zero, CC normalization, running status across packets, multiple messages, incomplete buffering, real-time byte handling, SysEx/system ignore, and malformed packet/stuck-state cases.

- [ ] **Step 2: Implement and verify the portable parser**

```cpp
class MidiMessageParser {
public:
  std::vector<input::PhysicalInputEvent>
  consume(std::string_view deviceId, std::span<const std::uint8_t> bytes,
          std::uint64_t timestampMicros);
  void reset();
};
```

Register `midi_input_tests` as `foundation_input_midi`, run red then green, and commit the portable parser before native files.

- [ ] **Step 3: Implement native backends behind the factory**

CoreMIDI uses client/port source notifications and `kMIDIPropertyUniqueID`; WinMM uses `midiIn*` APIs and links `winmm`; Android uses `MidiManager`, `DeviceCallback`, device output ports, and a `MidiReceiver` that calls:

```java
private static native void nativeMidiPacket(
    String stableId, byte[] data, int offset, int count, long timestampNanos);
```

Add `<uses-feature android:name="android.software.midi" android:required="false" />`. JNI/native callbacks copy packets into the backend queue only. Missing platform service reports unsupported and retains imported bindings.

- [ ] **Step 4: Wire platform builds and verify**

Add CoreMIDI framework discovery for macOS/iOS, `winmm` for Windows, Android source/JNI wiring, and the unsupported backend elsewhere. Update iOS membership exceptions for every new `src` file.

```bash
cmake --build cmake-build-debug --target midi_input_tests main -j 6
ctest --test-dir cmake-build-debug -R '^foundation_input_midi$' --output-on-failure
git add src/input android CMakeLists.txt tests/midi_input_tests.cpp ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat(input): add portable MIDI backends"
```

---

### Task 6: Binding capture model and settings UI

**Files:**
- Create: `src/input/InputCaptureController.h`
- Create: `src/input/InputCaptureController.cpp`
- Create: `tests/input_capture_controller_tests.cpp`
- Create: `src/scene/SettingsSceneInput.cpp`
- Modify: `src/scene/SettingsScene.h`
- Modify: `src/scene/SettingsScene.cpp`
- Modify: `src/scene/SettingsSceneLayout.cpp`
- Modify: `src/scene/CMakeLists.txt`
- Modify: `src/input/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: registry, resolver capture mode, input profile/store, and active profile path.
- Produces: press-to-bind, conflict confirmation, live monitoring, thresholds/inversion, and scoped reset UI.

- [ ] **Step 1: Write capture-controller tests first**

Cover repeat/axis-noise rejection, activation crossing, exact-duplicate ignore, no mutation before conflict confirmation, rejection preserving the profile, replacement removing only same-scope conflicts, missing-device visibility, scoped default reset, and sanitized threshold edits persisted through a fake save callback.

- [ ] **Step 2: Implement the tested controller**

```cpp
class InputCaptureController {
public:
  enum class State { Idle, Listening, AwaitingConflictConfirmation };
  InputCaptureController(InputDeviceRegistry &, InputProfile &,
                         std::function<bool(const InputProfile &, std::string &)> save);
  void begin(input::InputScope, input::LogicalAction);
  void cancel();
  void confirmReplace();
  void rejectReplace();
  void updateBinding(std::string_view bindingId, float deadZone,
                     float activationThreshold, float releaseThreshold,
                     bool inverted);
  void resetScopeToDefaults(input::InputScope);
  State state() const;
  std::optional<input::PhysicalInputEvent> monitorSample() const;
  std::span<const input::InputBinding> pendingConflicts() const;
};
```

- [ ] **Step 3: Add the Input settings tab**

Add `SettingsTab::Input` and keep all input-specific view construction/update logic in `SettingsSceneInput.cpp`. Provide player/key-mode/device dropdowns, binding rows, capture/monitor state, conflict confirmation, dead-zone/activation/release numeric controls, inversion, missing-device label with stable ID, and scoped reset. Persist only confirmed capture, committed threshold/inversion changes, or reset.

- [ ] **Step 4: Verify UI model, compact layouts, and full input suite**

```bash
cmake --build cmake-build-debug --target input_capture_controller_tests view_layout_tests main -j 6
ctest --test-dir cmake-build-debug -R '^(foundation_input_|view_layout_tests)' --output-on-failure
scripts/ios_firebase_deploy.sh --build-only
git add src/input src/scene tests/input_capture_controller_tests.cpp CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat(settings): add configurable input editor"
```

Expected: all input tests and existing layout tests pass; the iOS command performs a build-only run and does not upload.

