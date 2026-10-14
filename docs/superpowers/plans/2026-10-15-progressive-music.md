# Progressive Music Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans for the shared source and service integration; the independent platform adapters can be implemented in parallel under superpowers:dispatching-parallel-agents.

**Goal:** Start music playback from prepared opening PCM while the remainder is rendered in the background.
**Architecture:** A listening-only C++ Source owns the chart, bounded decoder windows, a producer, and a private PCM spool. Native streaming adapters consume published frames; complete cache hits retain file playback. Gameplay and full export rendering stay unchanged.
**Tech Stack:** C++23, libsndfile, AVAudioEngine, Android AudioTrack/JNI.
**Spec:** `docs/superpowers/specs/2026-10-15-progressive-music.md`.

## Global Constraints

- Gameplay continues full preparation; no changes to its scheduler/callback behavior.
- Existing PCM rounding, mixing headroom, and sample-frame timing are preserved.
- No deployment, new dependencies, worktrees, or whole-file formatting.
- Commit/push verified task changes on the current branch.

## Review Focus

- Long BGM beginning at zero must not be fully decoded before initial readiness.
- Stream replacement while a reader waits must cancel safely without stale completion.
- Seeking ahead may buffer but must include tails from sounds begun earlier.
- Pause/stop/background transitions must not spin or let preparation compete with gameplay.
- Truncated assets, disk failure, and cancellation must not publish corrupt cache files.

### Task 1: Shared progressive source

Files: create `src/audio/ProgressiveMusicSource.h/.cpp` and `tests/progressive_music_source_tests.cpp`; wire CMake.
Interface: `music_stream::Source::Create(unique_ptr<Chart>, path, bool)`, `ReadFrames(uint64_t, span<short>, milliseconds)`, `GetState()`, `SetRenderingEnabled(bool)`, `Cancel()`; see header for exact types.
- [x] Write tests for incremental readiness, full-render equivalence, long BGM, overlapping rates/tails, seeking, cancellation, and atomic cache publication; observe failure with the initial stub.
- [x] Implement a producer with 4096-frame blocks, ten-second read-ahead, bounded decoded windows, lazy asset opening and cancellable archive staging.
- [x] Run `progressive_music_source_tests` and existing chart/audio suites.

### Task 2: iOS adapter

Files: new music streaming adapter under `src/audio/`, `src/iOSNatives.hpp/.mm`.
Interface: `LoadIOSNativeMusicStream(shared_ptr<music_stream::Source>, const IOSNativeMusicMetadata&, string&)`; route existing transport and remote controls through the active backend.
- [x] Add/run transport or platform contract checks before integration.
- [x] Implement queued PCM buffers, source-frame position, seek/flush, rate modes, pause/stop, interruption handling, and completion only after drained audio.
- [x] Compile the iOS target and verify adapter lifecycle coverage.

### Task 3: Android adapter

Files: new Java streaming player, `AsoBMaShowActivity.java`, `src/AndroidNatives.h/.cpp`.
Interface: `LoadAndroidNativeMusicStream(shared_ptr<music_stream::Source>, const AndroidNativeMusicMetadata&, string&)`; JNI retains safe shared ownership and bounded reads.
- [x] Add/run transport tests before integration.
- [x] Implement streaming writes, hardware-frame position, seek/flush, PlaybackParams, cancellation, audio-focus/media-session routing, and EOF after drain.
- [x] Run Java tests and release build-only; smoke test on an emulator where available.

### Task 4: Music service integration and verification

Files: `NativeMusicPlayer.h/.cpp`, `ChartMusicCache.h/.cpp`, `MusicPlayerService.cpp`, focused tests.
- [x] Add preparation API returning a cache result plus an optional stream; wait only for the opening half-second or a short track's completion.
- [x] Route selected-track and Club Beat switching through streaming, keep adjacent preload/full export and gameplay paths intact.
- [x] Check cancellation/replacement/cache ownership and expose errors through existing status plumbing.
- [x] Run affected native tests, desktop/iOS/Android compile checks, and a final review. Commit and push.


### Task 5: Progressive chart preview and gameplay handoff

- [x] Add explicit-origin preview source, exact cursor seek, overlapping-tail tests,
      and private suffix spool that cannot populate the full-song cache.
- [x] Share session-owned bounded native PCM pages between preview and the full
      Jukebox load, validating file identity and retaining archive extraction.
- [x] Add portable worker/ring preview playback with native device-rate conversion,
      callback-only consumption, cancellation, and selected audio preferences.
- [x] Retain parsed charts and random choices; require full successful preparation
      before gameplay even when preview audio became ready much earlier.
- [x] Validate nonzero-origin output, missing-page reuse, cancellation and lifecycle,
      then complete platform builds and review before commit/push.

## Verification results

- Desktop `main` build passed; all 490 CTest checks passed in parallel.
- Final affected audio, preview, cache, handoff, and queue-boundary checks passed.
- Final unsigned iOS build and signed Android release build-only passed.
- Android JVM suite: 48 tests passed.
- Review corrections cover Android foreground-service handoff and queue boundaries,
  iOS session preservation and estimated-duration seeking, pre-clipping volume,
  concurrent preview stop, and replaced source-file identity.
- Preview/player sanitizer checks passed with address, undefined-behavior, and
  thread sanitizers on the portable implementations.
- Android 10 emulator smoke passed with an isolated data image: archive import,
  progressive preview, full gameplay entry, cold music playback, background
  position advancement, pause/resume, next-track replacement, and stop with
  foreground-service release. No app crash or stream error was observed.
- Emulator audio output was disabled; audible quality and physical-device timing
  were not assessed. Android 15 foreground eligibility is covered by review and
  contract checks, not by this Android 10 runtime smoke.


## Follow-up: preview visuals, direct seeking, and temporary music data

- Restore asynchronous preview BGA loading and drive it from consumed audio time;
  preserve preview chart ownership and later gameplay visual resources.
- Prepare seek ranges directly and preserve overlapping voices/resampler phase.
  Never read sparse gaps as silence; only declare full preparation after coverage.
- Replace ChartMusicCache with MusicPreparation, always returning an active stream.
  Remove adjacent-track prerendering and persistent full-song cache publication;
  retain private seek data only for the stream lifetime and clean legacy cache files.
- Keep explicit exports on the existing complete renderer.
- Verify focused PCM/lifecycle regressions, native builds, full CTest, and Android
  preview BGA plus player transport behavior.

## Follow-up: cold-seek latency and iOS native scrubber

- Skip decoder initialization for expired small Vorbis sounds using validated
  container duration upper bounds; retain exact PCM and backward-seek behavior.
- Route native seeks directly to the shared producer, interrupt obsolete blocks
  cooperatively, and wake old feeder reads. Reset pending seeks on stop/restart
  and iOS audio-engine configuration changes.
- Publish explicit finite-audio metadata and duration fallback for iOS controls.
- Cover decoder-open counts, exact output, obsolete-work interruption, stale reads,
  oversized seeks, native forwarding, and iOS metadata/remote seek registration.
- Run desktop, Android, and iOS build/static checks. The user will verify device
  interaction manually; do not run an emulator or simulator for this follow-up.

Verification: desktop `main` and affected native targets passed, with no compilation
on a repeat `main` build. All 491 CTest checks passed; all eight Android PCM player
JVM tests passed. Android release build-only and unsigned iOS build-only passed.
The synthetic desktop fixture with 1,200 short Vorbis keysounds reduced its first
100-second seek from about 3.45 seconds to 134 milliseconds; this is not an iPhone
latency measurement. Physical-device scrubber behavior remains for manual testing.
