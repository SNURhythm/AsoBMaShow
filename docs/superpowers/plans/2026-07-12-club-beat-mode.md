# Club Beat Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add separately persisted gameplay and music-player Club checkboxes that synthesize a chart-synchronized four-on-the-floor kick/clap layer without affecting clear eligibility.

**Architecture:** A shared platform-neutral module extracts the chart beat grid and generates deterministic stereo kick/clap PCM. Gameplay schedules those sounds through Jukebox; music playback and video export mix them into variant rendered WAV files.

**Tech Stack:** C++23, BMS parser timing, Jukebox/AudioWrapper, libsndfile chart rendering, custom View UI, CTest.

## Global Constraints

- Kick plays on every quarter-note beat.
- Clap plays on measure beats 2 and 4 when present.
- Gameplay and music-player Club settings persist separately.
- Club works at 100% and all supported playback rates.
- Club is not an assist and never lowers the clear-mark cap.
- Courses allow Club while keeping playback-rate controls locked to 100%.
- Replay and autoplay video exports reproduce Club audio when recorded or selected.
- Work inline on `feature/improve-practice`; no new worktree.

---

### Task 1: Shared chart beat plan and synthetic sounds

**Files:**
- Create: `src/audio/ClubBeat.h`
- Create: `src/audio/ClubBeat.cpp`
- Create: `tests/club_beat_tests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Produces: `club_beat::buildPlan(const bms_parser::Chart&) -> std::vector<Event>`
- Produces: `club_beat::synthesizeKick(int) -> StereoSound`
- Produces: `club_beat::synthesizeClap(int) -> StereoSound`

- [ ] **Step 1: Write failing beat-plan and synthesis tests**

Cover 4/4 kick beats with claps on 2/4, a shortened 3/4 measure with only beat-2 clap, timing across a BPM change and stop, deterministic clap samples, finite bounded PCM, and non-empty kick/clap durations.

- [ ] **Step 2: Verify RED**

Run: `cmake --build cmake-build-debug --target club_beat_tests -j 6`

Expected: CMake target or ClubBeat API is missing.

- [ ] **Step 3: Implement the shared module**

Define:

```cpp
namespace club_beat {
struct Event {
  long long timeMicros = 0;
  int beatInMeasure = 1;
  bool kick = true;
  bool clap = false;
};
struct StereoSound {
  int sampleRate = 44100;
  std::vector<float> samples;
};
}
```

Walk parsed measures in 0.25 BMS-measure increments, using timeline `BeatPosition`, `Timing`, BPM changes, and stop duration with the same timing rules as the preparation metronome. Generate a decaying downward-pitch sine kick and fixed-seed filtered-noise clap bursts.

Register the focused CTest target and add `audio/ClubBeat.cpp` to iOS synchronized-group membership exceptions.

- [ ] **Step 4: Verify GREEN**

Run: `cmake --build cmake-build-debug --target club_beat_tests -j 6 && ./cmake-build-debug/club_beat_tests`

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/audio/ClubBeat.h src/audio/ClubBeat.cpp tests/club_beat_tests.cpp CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat: add club beat synthesis"
```

### Task 2: Club-rendered audio and cache variants

**Files:**
- Modify: `src/audio/ChartAudioRenderer.h`
- Modify: `src/audio/ChartAudioRenderer.cpp`
- Modify: `src/audio/ChartMusicCache.h`
- Modify: `src/audio/ChartMusicCache.cpp`
- Test: `tests/club_beat_tests.cpp`

**Interfaces:**
- Consumes: shared Club beat plan and PCM
- Produces: `RenderOptions::clubMode`
- Produces: Club-aware `CachedAudioPathForChart(meta, bool)` and `EnsureRenderedMusicFile(..., bool clubMode, ...)`

- [ ] **Step 1: Add failing render/cache expectations**

Assert normal and Club cache paths differ while retaining the same chart identity stem. Render a silent fixture with Club enabled and assert frames around planned kick/clap events are non-zero while the normal render stays silent.

- [ ] **Step 2: Verify RED**

Run: `cmake --build cmake-build-debug --target club_beat_tests -j 6`

Expected: missing Club render option/cache overloads.

- [ ] **Step 3: Mix Club PCM and variant cache paths**

Add `bool clubMode = false` to render options. At output sample rate, mix kick and clap PCM at each planned chart time through the existing playback time conversion and clamp only at WAV encoding. Add a `_club` suffix before `.wav` for Club cache entries and propagate the flag through parsing/rendering overloads.

- [ ] **Step 4: Verify GREEN**

Run: `cmake --build cmake-build-debug --target club_beat_tests main -j 6 && ./cmake-build-debug/club_beat_tests`

Expected: PASS and `main` links.

- [ ] **Step 5: Commit**

```bash
git add src/audio/ChartAudioRenderer.h src/audio/ChartAudioRenderer.cpp src/audio/ChartMusicCache.h src/audio/ChartMusicCache.cpp tests/club_beat_tests.cpp
git commit -m "feat: render club beat audio variants"
```

### Task 3: Gameplay, practice, courses, and replay provenance

**Files:**
- Modify: `src/AppSettings.h`
- Modify: `src/AppSettingsStore.cpp`
- Modify: `src/audio/Jukebox.h`
- Modify: `src/audio/Jukebox.cpp`
- Modify: `src/ScoreProvenance.h`
- Modify: `src/ScoreProvenance.cpp`
- Modify: `src/scene/play/GamePlayStartOptions.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/scene/MainMenuScene.h`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `src/scene/ChartViewerScene.cpp`
- Modify: `tests/app_settings_store_tests.cpp`
- Modify: `tests/score_provenance_tests.cpp`
- Modify: `tests/audio_mix_tests.cpp`
- Modify: `tests/gameplay_playback_startup_tests.cpp`

**Interfaces:**
- Produces: `AppSettings::gameplayClubModeEnabled` and `musicPlayerClubModeEnabled`
- Produces: `StartOptions::clubMode`
- Produces: `ScoreProvenance::clubMode` as reproduction-only metadata
- Consumes: shared Club sounds and plan in Jukebox

- [ ] **Step 1: Write failing persistence/provenance/startup tests**

Assert separate settings round-trip with legacy false defaults; provenance JSON round-trips `clubMode`; replay startup restores it; and eligibility/clear-cap logic remains unchanged. Extend audio source classification to require Club events on the BGM bus.

- [ ] **Step 2: Verify RED**

Run: `cmake --build cmake-build-debug --target app_settings_store_tests score_provenance_tests audio_mix_tests gameplay_playback_startup_tests -j 6`

Expected: missing settings, provenance, and start-option fields.

- [ ] **Step 3: Implement gameplay scheduling and checkbox**

Pass `clubMode` from Ready/practice/replay startup into Jukebox scheduling. Load generated kick/clap resources and add Club events as BGM sources. Add a standalone Gameplay Club checkbox outside the Assist/playback-rate card; leave it enabled for course starts and do not reference it in playback clear-cap logic.

Serialize `clubMode` in score provenance with legacy false default, copy it into recorded replays, and restore it for replay/course stages without changing eligibility.

- [ ] **Step 4: Verify GREEN**

Run: `cmake --build cmake-build-debug --target app_settings_store_tests score_provenance_tests audio_mix_tests gameplay_playback_startup_tests main -j 6 && ./cmake-build-debug/app_settings_store_tests && ./cmake-build-debug/score_provenance_tests && ./cmake-build-debug/audio_mix_tests && ./cmake-build-debug/gameplay_playback_startup_tests`

Expected: all focused tests pass and `main` links.

- [ ] **Step 5: Commit**

```bash
git add src/AppSettings.h src/AppSettingsStore.cpp src/audio/Jukebox.h src/audio/Jukebox.cpp src/ScoreProvenance.h src/ScoreProvenance.cpp src/scene/play/GamePlayStartOptions.h src/scene/play/GamePlayScene.cpp src/scene/MainMenuScene.h src/scene/MainMenuScene.cpp src/scene/ChartViewerScene.cpp tests/app_settings_store_tests.cpp tests/score_provenance_tests.cpp tests/audio_mix_tests.cpp tests/gameplay_playback_startup_tests.cpp
git commit -m "feat: add club beat mode to gameplay"
```

### Task 4: Music-player Club switching

**Files:**
- Modify: `src/audio/MusicPlayerService.h`
- Modify: `src/audio/MusicPlayerService.cpp`
- Modify: `src/scene/MusicPlayerScene.h`
- Modify: `src/scene/MusicPlayerScene.cpp`

**Interfaces:**
- Consumes: Club-aware chart music cache
- Produces: `MusicPlayerService::SetClubMode(bool, std::string&)`
- Produces: persisted Music Player Club checkbox

- [ ] **Step 1: Implement service state and cache propagation**

Store the selected Club flag under `stateMutex`. Pass it to synchronous play, asynchronous play, adjacent preload, keep-path calculation, and cache pruning.

Extend playback-worker requests with an alternate-mix switch flag. When switching the loaded track, read native position/playing state immediately before replacing the item, load the alternate cached WAV, seek to the preserved source position, and resume only when previously playing.

- [ ] **Step 2: Add the checkbox UI**

Add a 52-point Music Player Club row beneath the two playback dropdowns, increase the transport container height, apply the service change immediately, persist only the music-player setting, and display preparation/failure status through the existing native-control status path.

- [ ] **Step 3: Compile**

Run: `cmake --build cmake-build-debug --target main -j 6`

Expected: `main` links.

- [ ] **Step 4: Commit**

```bash
git add src/audio/MusicPlayerService.h src/audio/MusicPlayerService.cpp src/scene/MusicPlayerScene.h src/scene/MusicPlayerScene.cpp
git commit -m "feat: add club beat mode to music player"
```

### Task 5: Replay and autoplay video export

**Files:**
- Modify: `src/ReplayVideoExporter.cpp`
- Modify: `src/ReplayAutoPlay.h`
- Modify: relevant autoplay launch construction in `src/scene/MainMenuScene.cpp`
- Test: `tests/replay_summary_list_tests.cpp`

**Interfaces:**
- Consumes: `ScoreProvenance::clubMode`
- Consumes: `RenderOptions::clubMode`
- Produces: Club audio in replay, course-stage, and synthetic autoplay export WAVs

- [ ] **Step 1: Write failing autoplay provenance test**

Assert synthetic autoplay replay construction carries the selected Club flag without becoming eligible for a different clear cap.

- [ ] **Step 2: Verify RED**

Run: `cmake --build cmake-build-debug --target replay_summary_list_tests -j 6`

Expected: autoplay builder does not accept or retain Club.

- [ ] **Step 3: Propagate Club into export rendering**

Set `RenderOptions::clubMode` from each replay stage provenance. Pass the gameplay checkbox into synthetic autoplay provenance so its video export uses the same Club layer. Do not alter playback-rate or assist calculations.

- [ ] **Step 4: Verify GREEN**

Run: `cmake --build cmake-build-debug --target replay_summary_list_tests main -j 6 && ./cmake-build-debug/replay_summary_list_tests`

Expected: PASS and `main` links.

- [ ] **Step 5: Commit**

```bash
git add src/ReplayVideoExporter.cpp src/ReplayAutoPlay.h src/scene/MainMenuScene.cpp tests/replay_summary_list_tests.cpp
git commit -m "feat: include club beats in replay exports"
```

### Task 6: Final verification and handoff

**Files:**
- Modify: `.superpowers/sdd/progress.md` (ignored local handoff log)

- [ ] **Step 1: Run focused CTest verification**

Run:

```bash
ctest --test-dir cmake-build-debug --output-on-failure -R "club_beat|app_settings_store|score_provenance_tests|audio_mix|gameplay_playback_startup|replay_summary_list"
cmake --build cmake-build-debug --target main -j 6
git diff --check
git status --short
```

Expected: all selected tests pass, `main` links, no whitespace errors, and tracked worktree is clean.

- [ ] **Step 2: Record feature commits and verification**

Append the commit range and verification result to `.superpowers/sdd/progress.md`.

