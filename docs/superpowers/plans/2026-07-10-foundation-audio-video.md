# Audio and Display Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add capability-aware audio device/latency/volume controls and safe display/VSync/frame-cap controls without losing active playback or a working window configuration.

**Architecture:** Persist user intent in backend-neutral settings, expose runtime capability snapshots, and apply risky changes through tested transactional managers. PortAudio/miniaudio and SDL/bgfx details remain behind runtime interfaces so fake backends can prove restart and rollback behavior deterministically.

**Tech Stack:** C++23, PortAudio desktop backend, miniaudio mobile backend, SDL2, bgfx, CMake/CTest.

## Global Constraints

- Work only in task worktrees/branches created from `feature/player-foundations`; never edit or commit on `develop`.
- CTest registration lands before these tasks; register every new standalone executable with an exact `foundation_av_*` name.
- Preserve valid but unsupported imported setting values; capability filtering affects runtime/UI, not persisted intent.
- Desktop legacy defaults are windowed 1280×720 with VSync off; iOS/Android runtime remains fixed fullscreen with VSync on.
- Volume values clamp to `[0,1]`; sample rate is `0` or `[8000,384000]`; buffer frames is `0` or `[16,8192]`; frame cap is `0` or `[15,1000]`.
- Audio offset remains independent of backend device/buffer latency.
- New `src` files must be listed in the iOS Xcode `membershipExceptions`.
- Do not upload builds. The iOS deployment script is used only with `--build-only`; Android build-only requires its existing private signing environment.

---

### Task 1: Freeze the audio/video settings contract

**Files:**
- Create: `src/settings/AudioVideoSettings.h`
- Create: `src/settings/AudioVideoSettings.cpp`
- Create: `tests/audio_video_settings_tests.cpp`
- Modify: `src/AppSettings.h`
- Modify: `src/AppSettings.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: current platform defaults from `main.cpp`.
- Produces: `player_settings::AudioVideoSettings`, stored as `AppSettings::audioVideo`, for the settings store, runtime managers, and UI.

- [ ] **Step 1: Write sanitization/default tests first**

Create assertions for every boundary and for preserving a non-empty unavailable device ID:

```cpp
player_settings::AudioVideoSettings value;
value.audio.outputDeviceId = "missing:device";
value.audio.masterVolume = 2.0f;
value.audio.requestedSampleRate = 7999;
value.video.displayIndex = -1;
value.video.frameCap = 14;
value.sanitize();
assert(value.audio.outputDeviceId == "missing:device");
assert(value.audio.masterVolume == 1.0f);
assert(value.audio.requestedSampleRate == 0);
assert(value.video.displayIndex == 0);
assert(value.video.frameCap == 0);
```

Also assert accepted boundary values, desktop defaults, and platform-default factory behavior.

- [ ] **Step 2: Register and verify red**

Create `audio_video_settings_tests`, register `foundation_av_settings`, and confirm it fails to compile because the contract is absent.

- [ ] **Step 3: Implement the exact contract**

```cpp
namespace player_settings {
enum class DisplayMode { Windowed, BorderlessFullscreen, ExclusiveFullscreen };
struct AudioSettings {
  std::string outputDeviceId;
  std::uint32_t requestedSampleRate = 0;
  std::uint32_t requestedBufferFrames = 0;
  float masterVolume = 1.0f;
  float bgmVolume = 1.0f;
  float keysoundVolume = 1.0f;
  bool operator==(const AudioSettings &) const = default;
};
struct VideoSettings {
  DisplayMode mode = DisplayMode::Windowed;
  int displayIndex = 0;
  int width = 1280;
  int height = 720;
  bool vsync = false;
  std::uint32_t frameCap = 0;
  bool operator==(const VideoSettings &) const = default;
};
struct AudioVideoSettings {
  AudioSettings audio;
  VideoSettings video;
  void sanitize();
  bool operator==(const AudioVideoSettings &) const = default;
};
AudioVideoSettings defaultAudioVideoSettingsForPlatform();
} // namespace player_settings
```

Add `player_settings::AudioVideoSettings audioVideo = defaultAudioVideoSettingsForPlatform();` to `AppSettings` and call its sanitizer from `AppSettings::sanitize()`. Persistence moves to the versioned settings plan; do not add more legacy `settings.cfg` keys.

- [ ] **Step 4: Verify and commit**

```bash
cmake --build cmake-build-debug --target audio_video_settings_tests main -j 6
ctest --test-dir cmake-build-debug -R '^foundation_av_settings$' --output-on-failure
git add src/settings src/AppSettings.* tests/audio_video_settings_tests.cpp src/CMakeLists.txt CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat(settings): define audio and display settings"
```

---

### Task 2: Add audio buses and restart-safe PCM

**Files:**
- Create: `src/audio/AudioMix.h`
- Create: `src/audio/AudioMix.cpp`
- Create: `tests/audio_mix_tests.cpp`
- Modify: `src/audio/AudioWrapper.h`
- Modify: `src/audio/AudioWrapper.cpp`
- Modify: `src/audio/Jukebox.h`
- Modify: `src/audio/Jukebox.cpp`
- Modify: `src/audio/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: `player_settings::AudioSettings` volume fields.
- Produces: BGM/keysound buses, atomic gain controls, and retained source PCM that can be regenerated for a new output sample rate.

- [ ] **Step 1: Write audio mix/resample tests first**

```cpp
audio::Volumes volumes{.master = 0.5f, .bgm = 0.25f, .keysound = 0.75f};
assert(audio::EffectiveGain(audio::Bus::Bgm, volumes) == 0.125f);
assert(audio::EffectiveGain(audio::Bus::Keysound, volumes) == 0.375f);
auto same = audio::ResamplePcm(source, 2, 48000, 48000);
assert(same == source);
auto converted = audio::ResamplePcm(source, 2, 44100, 48000);
assert(converted.size() == expectedFrames * 2);
```

Cover clamping, both buses, master multiplication, silent endpoints, same-rate copy, and 44.1↔48 kHz frame counts.

- [ ] **Step 2: Implement and integrate the mix API**

```cpp
namespace audio {
enum class Bus : std::uint8_t { Bgm, Keysound };
struct Volumes { float master = 1.0f; float bgm = 1.0f; float keysound = 1.0f; };
float EffectiveGain(Bus, const Volumes &);
std::vector<short> ResamplePcm(std::span<const short>, int channels,
                               int sourceRate, int targetRate);
}
```

Retain `sourceData`, `outputData`, source/output frame counts, and `sourceSampleRate` in each sound. Carry `audio::Bus` through playing, scheduled, and command records. Add bus arguments to `playSound` and `scheduleSound`; use atomics for callback-visible gains.

- [ ] **Step 3: Classify all chart audio**

Replace the Jukebox audio pair with:

```cpp
struct ScheduledAudioEvent {
  long long timeMicros = 0;
  int wav = bms_parser::Parser::NoWav;
  audio::Bus bus = audio::Bus::Bgm;
};
```

Background notes are BGM; chart notes, direct keysounds, replay keysounds, preparation metronome, and settings test tone are Keysound.

- [ ] **Step 4: Verify and commit**

```bash
cmake --build cmake-build-debug --target audio_mix_tests main -j 6
ctest --test-dir cmake-build-debug -R '^foundation_av_audio_mix$' --output-on-failure
git add src/audio tests/audio_mix_tests.cpp CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat(audio): add buses and restart-safe PCM"
```

---

### Task 3: Extract audio backends and transactional restart

**Files:**
- Create: `src/audio/AudioBackend.h`
- Create: `src/audio/AudioBackend.cpp`
- Create: `src/audio/AudioDeviceManager.h`
- Create: `src/audio/AudioDeviceManager.cpp`
- Create: `tests/audio_device_manager_tests.cpp`
- Modify: `src/audio/AudioWrapper.h`
- Modify: `src/audio/AudioWrapper.cpp`
- Modify: `src/audio/Jukebox.h`
- Modify: `src/audio/Jukebox.cpp`
- Modify: `src/audio/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: Task 1 settings and Task 2 source PCM/buses.
- Produces: capability enumeration, effective runtime state, and tested restart/rollback.

- [ ] **Step 1: Write fake-runtime transaction tests first**

Cover unknown device/rate/buffer rejection before suspension, volume-only apply without restart, successful order `suspend → restart → restore`, open failure rollback, playback-resume failure rollback, double failure leaving playback stopped, failed candidates not replacing last-working settings, and persisted unsupported intent remaining distinct from effective runtime.

- [ ] **Step 2: Implement backend contracts**

```cpp
namespace audio {
struct DeviceInfo { std::string id; std::string name; bool isDefault=false;
  std::vector<std::uint32_t> sampleRates; std::vector<std::uint32_t> bufferFrames; };
struct Capabilities { bool canSelectOutputDevice=false; bool canSelectSampleRate=false;
  bool canSelectBufferFrames=false; std::vector<DeviceInfo> outputDevices; };
struct StreamRequest { std::string deviceId; std::uint32_t sampleRate=0; std::uint32_t bufferFrames=0; };
struct RuntimeState { StreamRequest request; std::uint32_t effectiveSampleRate=0;
  std::uint32_t effectiveBufferFrames=0; double effectiveLatencyMs=0.0; };
using RenderCallback = void (*)(void *, std::uint32_t, int, void *);
class IBackend { public: virtual ~IBackend()=default; virtual bool start(std::string&)=0;
  virtual bool stop(std::string&)=0; virtual bool isStarted() const=0;
  virtual RuntimeState runtimeState() const=0; };
class IBackendFactory { public: virtual ~IBackendFactory()=default;
  virtual Capabilities capabilities() const=0;
  virtual std::unique_ptr<IBackend> open(const StreamRequest &, RenderCallback,
      void *, std::string &)=0; };
std::unique_ptr<IBackendFactory> CreatePlatformBackendFactory();
}
```

PortAudio IDs encode host API plus device name and duplicate ordinal, never raw index. Probe `{44100,48000,88200,96000,176400,192000}` and buffers `{0,64,128,256,512,1024,2048}`. An explicitly requested PortAudio configuration fails explicitly; it does not fall back to miniaudio. Mobile reports device/rate/buffer selection unsupported.

- [ ] **Step 3: Implement `AudioDeviceManager`**

```cpp
namespace audio {
struct PlaybackSnapshot { bool active=false; bool paused=false; long long positionMicros=0; };
class IAudioRuntime { public: virtual ~IAudioRuntime()=default;
  virtual Capabilities capabilities() const=0; virtual RuntimeState runtimeState() const=0;
  virtual bool restart(const StreamRequest &, std::string &)=0;
  virtual bool restore(const RuntimeState &, std::string &)=0;
  virtual void setVolumes(const Volumes &)=0; };
class IPlaybackSession { public: virtual ~IPlaybackSession()=default;
  virtual PlaybackSnapshot suspendAndDrain()=0;
  virtual bool restorePlayback(const PlaybackSnapshot &, std::string &)=0;
  virtual void leavePlaybackStopped()=0; };
enum class ApplyStatus { Applied, Unsupported, FailedRolledBack, FailedStopped };
struct ApplyResult { ApplyStatus status; RuntimeState effective;
  bool playbackResumed=false; std::string message; };
class AudioDeviceManager {
public:
  AudioDeviceManager(IAudioRuntime &, IPlaybackSession &, player_settings::AudioSettings);
  Capabilities capabilities() const;
  const player_settings::AudioSettings &lastWorkingSettings() const;
  ApplyResult apply(const player_settings::AudioSettings &);
};
}
```

`AudioWrapper` implements `IAudioRuntime`; Jukebox implements `IPlaybackSession`. Restart generates candidate output buffers before replacing the active stream and restores the exact prior playback position/paused state.

- [ ] **Step 4: Verify and commit**

```bash
cmake --build cmake-build-debug --target audio_device_manager_tests main -j 6
ctest --test-dir cmake-build-debug -R '^foundation_av_audio_device$' --output-on-failure
git add src/audio tests/audio_device_manager_tests.cpp CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat(audio): add transactional device manager"
```

---

### Task 4: Display preview and frame pacing

**Files:**
- Create: `src/video/DisplaySettingsManager.h`
- Create: `src/video/DisplaySettingsManager.cpp`
- Create: `src/video/SDLDisplayBackend.h`
- Create: `src/video/SDLDisplayBackend.cpp`
- Create: `src/video/FramePacer.h`
- Create: `src/video/FramePacer.cpp`
- Create: `tests/display_settings_manager_tests.cpp`
- Create: `tests/frame_pacer_tests.cpp`
- Modify: `src/video/CMakeLists.txt`
- Modify: `src/context.h`
- Modify: `src/main.cpp`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: `player_settings::VideoSettings`.
- Produces: a 15-second reversible display preview and deterministic app-owned frame cap.

- [ ] **Step 1: Write fake-backend and pacer tests first**

Assert preview confirmation, timeout restoration, immediate focus-loss restoration, SDL/renderer failure rollback, unsupported-field rejection without backend calls, second-preview rollback, frame-cap-only safe apply, uncapped pacing, exact deadline, overrun, cap changes, background reset, and drift-free advancement.

- [ ] **Step 2: Implement the display contracts**

```cpp
namespace display {
struct Resolution { int width=0; int height=0; int refreshRateHz=0; };
struct DisplayInfo { int index=0; std::string name; std::vector<Resolution> resolutions; };
struct Capabilities { bool canChangeMode=false; bool canSelectDisplay=false;
  bool canSelectResolution=false; bool canChangeVsync=false; bool canSetFrameCap=true;
  std::vector<DisplayInfo> displays; };
struct RuntimeState { player_settings::VideoSettings settings; int windowX=0; int windowY=0;
  std::uint32_t sdlWindowFlags=0; std::uint32_t bgfxResetFlags=0; };
class IDisplayBackend { public: virtual ~IDisplayBackend()=default;
  virtual Capabilities capabilities() const=0; virtual RuntimeState capture() const=0;
  virtual bool apply(const player_settings::VideoSettings &, std::string &)=0;
  virtual bool restore(const RuntimeState &, std::string &)=0; };
enum class RollbackReason { Timeout, FocusLost, Cancelled, ApplyFailed };
enum class ApplyStatus { Applied, PreviewPending, Unsupported, FailedRolledBack, FailedUnrecoverable };
struct ApplyResult { ApplyStatus status=ApplyStatus::Unsupported;
  player_settings::VideoSettings effective; std::string message; };
class DisplaySettingsManager {
public:
  static constexpr std::chrono::seconds kConfirmationTimeout{15};
  DisplaySettingsManager(IDisplayBackend &, player_settings::VideoSettings);
  ApplyResult beginPreview(const player_settings::VideoSettings &,
                           std::chrono::steady_clock::time_point);
  bool confirmPreview();
  ApplyResult cancelPreview(RollbackReason);
  std::optional<ApplyResult> tick(std::chrono::steady_clock::time_point);
  void onFocusLost();
  bool hasPendingPreview() const;
};
}
```

- [ ] **Step 3: Implement SDL/bgfx application and frame pacing**

Enumerate/deduplicate SDL display modes. Implement windowed, borderless desktop, and matched exclusive fullscreen modes, then verify flags/display/size. Toggle only `BGFX_RESET_VSYNC` and preserve other reset flags through a renderer callback from `main.cpp`. Mobile reports a fixed display and only permits frame cap.

```cpp
class FramePacer {
public:
  void setCap(std::uint32_t fps);
  void reset(std::chrono::steady_clock::time_point now);
  std::chrono::steady_clock::duration remaining(std::chrono::steady_clock::time_point now) const;
  void framePresented(std::chrono::steady_clock::time_point now);
};
```

Tick previews every frame, call `onFocusLost()` before background wait, and reset pacing after foreground, resize, or export suspension.

- [ ] **Step 4: Verify and commit**

```bash
cmake --build cmake-build-debug --target display_settings_manager_tests frame_pacer_tests main -j 6
ctest --test-dir cmake-build-debug -R '^foundation_av_(display|frame_pacer)$' --output-on-failure
git add src/video src/context.h src/main.cpp tests/display_settings_manager_tests.cpp tests/frame_pacer_tests.cpp CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat(video): add reversible display settings"
```

---

### Task 5: Audio/display settings models and UI

**Files:**
- Create: `src/scene/SettingsAudioVideoModel.h`
- Create: `src/scene/SettingsAudioVideoModel.cpp`
- Create: `src/scene/SettingsSceneAudioVideo.cpp`
- Create: `tests/settings_audio_video_model_tests.cpp`
- Modify: `src/scene/SettingsScene.h`
- Modify: `src/scene/SettingsScene.cpp`
- Modify: `src/scene/SettingsSceneControls.cpp`
- Modify: `src/scene/SettingsSceneLayout.cpp`
- Modify: `src/scene/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: active profile `AppSettings`, audio/display managers, and runtime capabilities.
- Produces: accessible capability-aware settings controls and safe preview confirmation.

- [ ] **Step 1: Write pure model tests first**

```cpp
auto audioModel = BuildAudioControlModel(intent, capabilities, effective);
assert(audioModel.devices.front().persistedValue == "missing:device");
assert(audioModel.devices.front().label == "missing:device (Unavailable)");
assert(audioModel.effectiveLatencyMs ==
       1000.0 * effective.effectiveBufferFrames / effective.effectiveSampleRate);
```

Cover unsupported controls and explanations, friendly label vs stable ID, mobile fixed fields visible/disabled, and effective rather than requested latency.

- [ ] **Step 2: Implement models and tab views**

Expose:

```cpp
AudioControlModel BuildAudioControlModel(
    const player_settings::AudioSettings &, const audio::Capabilities &,
    const audio::RuntimeState &);
DisplayControlModel BuildDisplayControlModel(
    const player_settings::VideoSettings &, const display::Capabilities &);
```

Add Audio and Display tabs. Use dropdowns for enumerated values and existing numeric controls for volume. Volumes apply/save immediately; device/rate/buffer save only after successful apply. “Test Sound” uses Keysound. Display Apply opens the 15-second Keep/Revert overlay; timeout, focus loss, tab exit, and cleanup revert.

- [ ] **Step 3: Verify integration and commit**

```bash
cmake --build cmake-build-debug --target settings_audio_video_model_tests view_layout_tests main -j 6
ctest --test-dir cmake-build-debug -R '^(foundation_av_|view_layout_tests)' --output-on-failure
scripts/ios_firebase_deploy.sh --build-only
git add src/scene tests/settings_audio_video_model_tests.cpp CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat(settings): expose audio and display controls"
```
