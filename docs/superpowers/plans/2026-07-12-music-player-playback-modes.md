# Music Player Playback Modes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add persistent Pitch Shift and Time Stretch modes to the mobile music player while retaining its 50–200% playback-rate control.

**Architecture:** Carry the existing `audio::PlaybackRate` value from settings through `MusicPlayerScene`, `MusicPlayerService`, and `NativeMusicPlayer`. Android maps it to `PlaybackParams`; iOS uses an `AVAudioEngine` player node and time-pitch unit so both algorithms share one transport implementation.

**Tech Stack:** C++20, Objective-C++/AVFoundation, Java/Android MediaPlayer, custom View/Dropdown UI, CTest.

## Global Constraints

- Pitch Shift links speed and pitch; Time Stretch changes speed while preserving pitch.
- Existing settings default to Pitch Shift and 100%.
- Supported rates remain 50–200% in 5% steps.
- The music-player selection remains independent from gameplay playback settings.
- Do not create a new worktree; commit coherent feature slices on `feature/improve-practice`.

---

### Task 1: Persist and propagate the music-player playback mode

**Files:**
- Modify: `src/AppSettings.h`
- Modify: `src/AppSettings.cpp`
- Modify: `src/AppSettingsStore.cpp`
- Modify: `src/audio/MusicPlayerService.h`
- Modify: `src/audio/MusicPlayerService.cpp`
- Modify: `src/audio/NativeMusicPlayer.h`
- Modify: `src/audio/NativeMusicPlayer.cpp`
- Modify: `tests/app_settings_store_tests.cpp`

**Interfaces:**
- Produces: `AppSettings::musicPlayerPlaybackMode`
- Produces: `MusicPlayerService::SetPlaybackRate(audio::PlaybackRate, std::string&)`
- Produces: `MusicPlayerService::PlaybackRate() const -> audio::PlaybackRate`
- Produces: `native_music_player::SetPlaybackRate(audio::PlaybackRate, std::string&)`

- [ ] **Step 1: Write the failing settings tests**

Extend the round-trip fixture with `musicPlayerPlaybackMode = audio::PlaybackMode::TimeStretch`, assert JSON contains `"musicPlayerPlaybackMode": 1`, and assert legacy JSON defaults to `PitchShift`. Add an invalid enum value, call `sanitize()`, and assert it becomes `PitchShift`.

- [ ] **Step 2: Run the test and verify RED**

Run: `cmake --build cmake-build-debug --target app_settings_store_tests -j 6 && ./cmake-build-debug/app_settings_store_tests`

Expected: compilation fails because `musicPlayerPlaybackMode` does not exist.

- [ ] **Step 3: Implement the settings and service value**

Add:

```cpp
audio::PlaybackMode musicPlayerPlaybackMode = audio::PlaybackMode::PitchShift;
```

Sanitize it to the two valid enum values, save/load it as an integer, and replace the service's `int playbackRatePercent` with:

```cpp
audio::PlaybackRate playbackRate{};
```

Pass the complete value to the native layer during mutation and after every track load. Only store the new value after the native call succeeds.

- [ ] **Step 4: Run focused tests and compile**

Run: `cmake --build cmake-build-debug --target app_settings_store_tests main -j 6 && ./cmake-build-debug/app_settings_store_tests`

Expected: PASS and `main` links.

- [ ] **Step 5: Commit**

```bash
git add src/AppSettings.h src/AppSettings.cpp src/AppSettingsStore.cpp src/audio/MusicPlayerService.h src/audio/MusicPlayerService.cpp src/audio/NativeMusicPlayer.h src/audio/NativeMusicPlayer.cpp tests/app_settings_store_tests.cpp
git commit -m "feat: propagate music player playback mode"
```

### Task 2: Add the music-player mode selector

**Files:**
- Modify: `src/scene/MusicPlayerScene.h`
- Modify: `src/scene/MusicPlayerScene.cpp`

**Interfaces:**
- Consumes: `MusicPlayerService::SetPlaybackRate(audio::PlaybackRate, std::string&)`
- Consumes: `MusicPlayerService::PlaybackRate() const`
- Produces: two dropdowns in the existing playback row: `Mode` and `Playback rate`

- [ ] **Step 1: Add the mode-control behavior before wiring the view**

Implement a `setPlaybackMode(const std::string&)` path that parses only `pitch-shift` and `time-stretch`, combines the selected mode with the current percentage, calls the service, persists the accepted value, closes the overlay, and refreshes both controls. Reuse that same apply-and-save path from percentage changes.

- [ ] **Step 2: Build and verify the missing UI wiring is observable**

Run: `cmake --build cmake-build-debug --target main -j 6`

Expected: the model code compiles, but no selector is visible until Step 3.

- [ ] **Step 3: Wire the dropdown**

Add a `DropdownView *playbackModeDropdown`, its open-state boolean, and these options:

```cpp
{{.id = "pitch-shift", .label = "Pitch Shift"},
 {.id = "time-stretch", .label = "Time Stretch"}}
```

Lay the mode and percentage dropdowns side by side in the existing 52-point row with equal flex. Initialize the service from both persisted fields when the scene starts.

- [ ] **Step 4: Compile**

Run: `cmake --build cmake-build-debug --target main -j 6`

Expected: `main` links.

- [ ] **Step 5: Commit**

```bash
git add src/scene/MusicPlayerScene.h src/scene/MusicPlayerScene.cpp
git commit -m "feat: add music player playback mode control"
```

### Task 3: Implement Android mode mapping

**Files:**
- Modify: `src/AndroidNatives.h`
- Modify: `src/AndroidNatives.cpp`
- Modify: `android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowActivity.java`

**Interfaces:**
- Consumes: `audio::PlaybackRate`
- Produces: Android bridge call carrying percent and mode ID

- [ ] **Step 1: Change the Android bridge contract**

Pass percent and mode as separate string arguments. Reject unknown modes at the C++ boundary before calling Java.

- [ ] **Step 2: Map both algorithms**

Store the selected mode beside `nativeMusicPlaybackRate` and apply:

```java
params.setSpeed(nativeMusicPlaybackRate);
params.setPitch(nativeMusicTimeStretch ? 1.0f : nativeMusicPlaybackRate);
```

Preserve paused state around `setPlaybackParams`, as the current implementation does.

- [ ] **Step 3: Compile Android**

Run: `scripts/android_firebase_deploy.sh --build-only`

Expected: Firebase release APK builds without upload.

- [ ] **Step 4: Commit**

```bash
git add src/AndroidNatives.h src/AndroidNatives.cpp android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowActivity.java
git commit -m "feat: support music playback modes on Android"
```

### Task 4: Implement iOS time-pitch playback

**Files:**
- Modify: `src/iOSNatives.hpp`
- Modify: `src/iOSNatives.mm`

**Interfaces:**
- Consumes: `audio::PlaybackRate`
- Produces: native iOS load/play/pause/stop/seek/state behavior backed by `AVAudioEngine`

- [ ] **Step 1: Replace player globals with an engine transport**

Own `AVAudioEngine`, `AVAudioPlayerNode`, `AVAudioUnitTimePitch`, and `AVAudioFile`, plus source-position, monotonic-start-time, playback-state, and schedule-generation fields. Keep metadata, queue, remote-command, and audio-session state unchanged.

- [ ] **Step 2: Implement one scheduling path**

Schedule the remaining source frames from the current source position. Completion handlers notify `ControlEvent::Finished` only when their captured generation is still current. Pause and seek increment the generation so cancelled schedules cannot report false completion.

- [ ] **Step 3: Apply the mode live**

Before changing parameters, rebase the source position using elapsed real time multiplied by the old rate. Then apply:

```objective-c++
gIOSNativeMusicTimePitch.rate = rate;
gIOSNativeMusicTimePitch.pitch =
    mode == audio::PlaybackMode::PitchShift
        ? static_cast<float>(1200.0 * std::log2(rate))
        : 0.0f;
```

Use the rebased position for state and now-playing elapsed time. Playback rate in `MPNowPlayingInfoCenter` remains the selected speed.

- [ ] **Step 4: Preserve transport semantics**

Load prepares the engine and file at position zero; play schedules or resumes; pause freezes the rebased source position; stop returns to zero and restores the ambient session; seek clamps to file duration and resumes only if previously playing.

- [ ] **Step 5: Compile iOS**

Run: `scripts/ios_firebase_deploy.sh --build-only`

Expected: the iPhoneOS target compiles and links without deployment.

- [ ] **Step 6: Commit**

```bash
git add src/iOSNatives.hpp src/iOSNatives.mm
git commit -m "feat: support music playback modes on iOS"
```

### Task 5: Final verification

**Files:**
- Modify: `.superpowers/sdd/progress.md` (ignored local handoff log)

- [ ] **Step 1: Run focused and desktop verification**

Run:

```bash
cmake --build cmake-build-debug --target app_settings_store_tests main -j 6
./cmake-build-debug/app_settings_store_tests
git diff --check
git status --short
```

Expected: test passes, `main` links, no whitespace errors, and the tracked worktree is clean.

- [ ] **Step 2: Record the completed commit range**

Append the common/UI/Android/iOS commit hashes and verification outcomes to `.superpowers/sdd/progress.md`.

