# Audio Playback Domains Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task by task.

**Goal:** Isolate gameplay and system playback lifecycle while retaining one native output.

**Architecture:** ApplicationAudio owns two AudioWrapper submix processors and one SharedAudioOutput. Software backend adapters provide independently drainable callback lifetimes; the shared callback sums float output and performs final conversion.

**Tech Stack:** C++23, existing native audio backends, CMake/CTest.

**Spec:** docs/superpowers/specs/2026-10-31-audio-playback-domains-design.md

## Global Constraints

No new audio dependency, output-device setting, or distribution action. No callback allocation, decoding, locks or ownership destruction. One build at a time in cmake-build-debug. Use the current checkout and the user-authorized new branch.

## Review Focus

- A callback in flight during a chart unload must finish before PCM release while SFX continues.
- System commands issued concurrently with device changes must not observe partially converted PCM.
- Failed native open/start and rollback must preserve system handles and safe ownership.
- Oversized native blocks must preserve timing offsets and clip only after summing domains.
- Scene transitions must explicitly retire their own menu audio rather than relying on global chart stops.

### Task 1: Isolation regressions

Files: tests/audio_wrapper_lifecycle_tests.cpp.
- [x] Add a regression that plays a looping SFX, stops/unloads a chart, and verifies continued next-frame output and a valid handle without native stop/start.
- [x] Add normal-speed system playback coverage with altered gameplay rate; watch the current shared wrapper fail.

### Task 2: Shared output and application coordinator

Files: src/audio/SharedAudioOutput.{h,cpp}, src/audio/ApplicationAudio.{h,cpp}, src/audio/AudioBackend.h, src/audio/AudioWrapper.{h,cpp}, CMakeLists.txt, src/audio/CMakeLists.txt.
Interfaces: SharedAudioOutput::createSubmixFactory(index) returns an IBackendFactory for a float software node; ApplicationAudio exposes gameplay(), systemSounds(), and IAudioRuntime controls.
- [x] Implement bounded float output, submix admission/drain, one native stream, and timestamp propagation.
- [x] Add float render-format negotiation to backend factories, defaulting existing native factories to signed 16-bit.
- [x] Coordinate both processors under lifecycle locks for device changes and rollback. Preserve system voice cursors and handles.
- [x] Verify isolation, concurrency, output format, settings rollback and suspension tests.

### Task 3: Application routing

Files: src/context.h, src/main.cpp, src/audio/Jukebox.{h,cpp}, SFX consumers under src/scene, and their fixture tests.
Interfaces: ApplicationContext owns ApplicationAudio before Jukebox; Jukebox borrows its gameplay AudioWrapper; system consumers use context.systemAudio().
- [x] Route device settings and app lifecycle to ApplicationAudio and SFX to the system processor.
- [x] Remove chart/device preservation workarounds where isolation makes them unnecessary.
- [x] Verify scene handoff, retries, and settings behavior with focused tests and desktop compile.

### Task 4: Review and deliver

- [x] Run broader tests appropriate to shared-header and application ownership changes.
- [x] Request one fresh whole-change review; resolve material findings and verify fixes.
- [x] Update documentation, commit task-related changes, and push refactor/audio-playback-domains.

## Verification record

- Desktop `main` build passed.
- The affected audio, gameplay, music-select, settings, skin, and adjacent integration set ran 180 tests. After updating the preview fixture to reflect independent file playback, its rerun and the audio lifecycle rerun passed; all 180 selected cases are green across those runs.
- Lifecycle regressions cover independent queues and playback rates, PCM retirement during an in-flight callback, zero callback allocations under command pressure, asynchronous load conversion during device changes, startup failure recovery, suspension, rollback, and final float summing.
- Gameplay output matches standalone output sample for sample at 50%, 100%, and 200% rates, including scheduled onsets across chunk boundaries.
- The independent review's startup-failure finding and obsolete preview-silencing workaround were resolved and verified.
- Optimized local callback benchmark: the largest measured increase was 0.106 microseconds (64 frames, one voice). The 128-voice cases were within 1% of standalone. These are CPU timings, not device-latency measurements; native mobile device testing remains outstanding.
