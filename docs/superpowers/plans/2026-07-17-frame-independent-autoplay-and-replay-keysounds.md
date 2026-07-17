# Frame-Independent Autoplay and Replay Keysounds Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run ordinary live autoplay gameplay on the realtime worker and stage replay-watch keysounds before playback so neither depends on render frames.

**Architecture:** `RealtimeGameplayWorker` will use its existing 1 ms audio-clock advancement for autoplay and will submit automatic input-triggered sounds through its fixed realtime audio sink. Replay watch will keep its current judgement path but convert recorded Press events into Jukebox scheduled-audio entries before `play()` stages the complete schedule.

**Tech Stack:** C++23, `GameplaySimulation`, `RealtimeGameplayWorker`, Jukebox/AudioWrapper scheduled audio, CMake/CTest.

## Global Constraints

- Live autoplay uses one worker-owned gameplay simulation; the scene never runs the legacy autoplay mutation path concurrently.
- Replay-watch judgement behavior and ordering remain unchanged.
- Normal pre-scheduled autoplay keysounds must not be duplicated by the realtime worker.
- Practice-session autoplay remains on its current practice authority.
- Rendering consumes snapshots and may lag without delaying gameplay or audio.
- Replay Press timestamps are gameplay time and must be converted to raw song time using the active audio offset.

---

### Task 1: Realtime worker automatic keysounds

**Files:**
- Modify: `tests/realtime_gameplay_worker_tests.cpp`
- Modify: `src/scene/play/RealtimeGameplayWorker.cpp`

**Interfaces:**
- Consumes: `RealtimeGameplayWorkerConfig::simulation.attempt.autoPlay`, `RealtimeGameplayAudioSink::{reserve,commit,cancel}`, and `GameplayInputResult::soundNoteId`.
- Produces: automatic autoplay transactions whose matching realtime keysound is committed exactly once before snapshot publication.

- [ ] **Step 1: Write the failing worker test**

Add a test that enables autoplay, advances only `FakeClock`, and expects one audio commit and a played/judged note without calling `enqueueInput()`:

```cpp
void testAutoplayCommitsGameplayAndKeysoundWithoutFramePump() {
  FakeClock clock;
  FakeAudio audio;
  auto config = makeConfig(clock, audio);
  config.simulation.attempt.autoPlay = true;
  gameplay::RealtimeGameplayWorker worker(makeRapidDefinition(),
                                           std::move(config));
  require(worker.start(), "autoplay worker starts");
  clock.nowMicros.store(1'000'000, std::memory_order_release);
  require(waitUntil([&] { return audio.commitCount.load() == 1; }),
          "autoplay commits its keysound without a frame pump");
  auto snapshot = worker.acquireLatestSnapshot();
  require(snapshot && snapshot->noteStates[0].played &&
              snapshot->attempt.judgeCounts[PGreat] == 1,
          "autoplay commits note and judgement from audio time");
  worker.stop();
}
```

- [ ] **Step 2: Run the test and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target realtime_gameplay_worker_tests -j 6
./cmake-build-debug/realtime_gameplay_worker_tests
```

Expected: FAIL at `autoplay commits its keysound without a frame pump` because `advanceAutomatic()` currently records transactions without using the audio sink.

- [ ] **Step 3: Reserve and commit automatic sounds**

In `RealtimeGameplayWorker::advanceAutomatic()`, for each transaction with a valid `soundNoteId` while `inputTriggeredKeysounds` is true:

```cpp
RealtimeGameplayAudioReservation reservation;
if (transaction.soundNoteId != kInvalidNoteId &&
    config_.inputTriggeredKeysounds) {
  if (config_.audio.reserve == nullptr ||
      !config_.audio.reserve(config_.audio.context,
                             transaction.soundNoteId, reservation)) {
    latchFault(RealtimeGameplayFault::AudioCapacityUnavailable);
    return true;
  }
}
recordTransaction(transaction);
if (transaction.soundNoteId != kInvalidNoteId &&
    config_.inputTriggeredKeysounds && config_.audio.commit == nullptr) {
  if (reservation.requiresCommit && config_.audio.cancel != nullptr) {
    config_.audio.cancel(config_.audio.context, reservation,
                         transaction.soundNoteId);
  }
  latchFault(RealtimeGameplayFault::AudioCommitFailed);
  return true;
}
if (transaction.soundNoteId != kInvalidNoteId &&
    config_.inputTriggeredKeysounds &&
    !config_.audio.commit(config_.audio.context, reservation,
                          transaction.soundNoteId)) {
  // commit consumes the reservation on both success and failure.
  latchFault(RealtimeGameplayFault::AudioCommitFailed);
  return true;
}
```

Do not invoke the sink when `inputTriggeredKeysounds` is false; that mode is already present in Jukebox's complete scheduled-audio list.

- [ ] **Step 4: Run the focused worker test and verify GREEN**

Run the commands from Step 2.

Expected: PASS, including rapid input, pause, activation gate, and gauge-history cases.

- [ ] **Step 5: Commit Task 1**

```bash
git add src/scene/play/RealtimeGameplayWorker.cpp tests/realtime_gameplay_worker_tests.cpp
git commit -m "fix: submit autoplay keysounds in realtime"
```

### Task 2: Enable realtime authority for ordinary live autoplay

**Files:**
- Modify: `src/scene/play/GamePlayScene.cpp`

**Interfaces:**
- Consumes: `StartOptions::autoPlay`, `RealtimeGameplayWorker`, and the existing snapshot/terminal transfer path.
- Produces: ordinary non-practice, non-replay autoplay sessions that create the worker on every platform without claiming physical input devices.

- [ ] **Step 1: Extend the worker regression to prove frame-free completion**

In `tests/realtime_gameplay_worker_tests.cpp`, advance the fake clock through the second note and assert `GameplayTerminalReason::ChartComplete` from the snapshot without any input or frame call:

```cpp
clock.nowMicros.store(2'000'000, std::memory_order_release);
require(waitUntil([&] {
          auto latest = worker.acquireLatestSnapshot();
          return latest && latest->terminalReason ==
                               gameplay::GameplayTerminalReason::ChartComplete;
        }),
        "autoplay completes from audio time without a frame pump");
```

- [ ] **Step 2: Run the extended test and verify it passes at the worker boundary**

Run:

```bash
cmake --build cmake-build-debug --target realtime_gameplay_worker_tests -j 6
./cmake-build-debug/realtime_gameplay_worker_tests
```

Expected: PASS, proving the reusable authority already owns automatic completion.

- [ ] **Step 3: Wire ordinary live autoplay into `GamePlayScene`**

Change `startRealtimeGameplayAuthority()` so unsupported native-input platforms reject only manual realtime input, while autoplay may use the platform-neutral worker:

```cpp
#if !(TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR || TARGET_OS_WINDOWS)
  if (!options.autoPlay) {
    return false;
  }
#endif
```

Allow `inputHandler == nullptr` only for autoplay, retain replay/practice exclusions, and pass the real option:

```cpp
if (realtimeGameplayAuthorityActive() || chart == nullptr || state == nullptr ||
    renderer == nullptr || (!options.autoPlay && inputHandler == nullptr) ||
    isReplayPlayback() || options.practiceMode ||
    options.practiceSession != nullptr) {
  return false;
}
// ...
.autoPlay = options.autoPlay,
```

Create touch/physical routers, subscriptions, SDL watches, device claims, and registry suppression only when `!options.autoPlay`. Guard `inputHandler->discardPendingTouchEvents()` and every later input-handler dereference. The autoplay worker still starts, maps the audio clock, publishes snapshots, suspends on pause, and transfers terminal replay/gauge state through the existing code.

- [ ] **Step 4: Build the application**

Run:

```bash
cmake --build cmake-build-debug --target main -j 6
```

Expected: `main` builds successfully on macOS, proving the formerly platform-gated scene path compiles on a non-iOS/non-Windows target.

- [ ] **Step 5: Commit Task 2**

```bash
git add src/scene/play/GamePlayScene.cpp tests/realtime_gameplay_worker_tests.cpp
git commit -m "fix: advance live autoplay off the frame loop"
```

### Task 3: Pre-schedule replay-watch keysounds

**Files:**
- Create: `src/scene/play/ReplayKeysoundSchedule.h`
- Create: `src/scene/play/ReplayKeysoundSchedule.cpp`
- Create: `tests/replay_keysound_schedule_tests.cpp`
- Modify: `src/scene/play/CMakeLists.txt`
- Modify: `src/audio/Jukebox.h`
- Modify: `src/audio/Jukebox.cpp`
- Modify: `src/scene/play/GamePlayScene.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `std::vector<ScheduledAudioEvent> buildReplayKeysoundSchedule(const gameplay::GameplayDefinition &, std::span<const ReplayEvent>, long long audioOffsetMicros, std::optional<gameplay::GameplayTimeRange> allowedRange)`.
- Produces: `void Jukebox::appendScheduledAudioEvents(std::span<const ScheduledAudioEvent> events)`; callable after `schedule()` and before `play()`.
- Consumes: replay Press events, immutable gameplay definition lane indices, `gameplay_timing::rawSongTimeFromGameplayTime()`, and Jukebox's existing staged schedule.

- [ ] **Step 1: Write failing pure schedule tests**

Create a fixture with a WAV-bearing note at lane 1/time 1,000,000 and assert:

```cpp
const std::array events{
    ReplayEvent{.action = ReplayEventAction::Press,
                .lane = 1,
                .noteTimeMicros = 1'000'000,
                .songTimeMicros = 1'120'000},
    ReplayEvent{.action = ReplayEventAction::Release,
                .lane = 1,
                .noteTimeMicros = 1'000'000,
                .songTimeMicros = 1'220'000}};
const auto scheduled = buildReplayKeysoundSchedule(
    definition, events, 120'000, std::nullopt);
require(scheduled.size() == 1 &&
            scheduled[0].timeMicros == 1'000'000 &&
            scheduled[0].wav == 42 &&
            scheduled[0].bus == audio::Bus::Keysound,
        "replay Press is scheduled at raw audio time on the keysound bus");
```

Add cases proving unresolved notes, `NoWav`, non-Press actions, and events outside `allowedRange` produce no entry.

- [ ] **Step 2: Build and verify RED**

Add the test target to `CMakeLists.txt`, then run:

```bash
cmake --build cmake-build-debug --target replay_keysound_schedule_tests -j 6
```

Expected: compilation FAIL because `ReplayKeysoundSchedule.h` and `buildReplayKeysoundSchedule()` do not exist.

- [ ] **Step 3: Implement the pure builder**

Use `definition.laneNotes(event.lane)` plus `std::ranges::lower_bound` on `GameplayNote::timingMicros`. Include an entry only when action is Press, both replay times satisfy the optional range, the exact lane/time note exists, and its WAV is not `NoWav`:

```cpp
result.push_back(makeScheduledAudioEvent(
    gameplay_timing::rawSongTimeFromGameplayTime(event.songTimeMicros,
                                                  audioOffsetMicros),
    note.wav, JukeboxAudioSource::ReplayKeysound));
```

Preserve replay event order; Jukebox performs the final deterministic schedule sort.

- [ ] **Step 4: Add the Jukebox append boundary and scene wiring**

Implement:

```cpp
void Jukebox::appendScheduledAudioEvents(
    std::span<const ScheduledAudioEvent> events) {
  audioList.insert(audioList.end(), events.begin(), events.end());
  std::sort(audioList.begin(), audioList.end(), scheduledAudioEventLess);
}
```

In `GamePlayScene::reset()`, immediately after `context.jukebox.schedule(...)` and before `context.jukebox.play(...)`, build a gameplay definition and append the replay schedule when `isReplayPlayback()` and `!options.autoKeySound`.

Remove `replayKeySoundCursor`, `processReplayKeySounds()`, its per-frame call, and its reset assignment so no duplicate direct sound remains.

- [ ] **Step 5: Run replay and audio tests**

Run:

```bash
cmake --build cmake-build-debug --target replay_keysound_schedule_tests audio_mix_tests main -j 6
./cmake-build-debug/replay_keysound_schedule_tests
./cmake-build-debug/audio_mix_tests
```

Expected: both tests PASS and `main` builds.

- [ ] **Step 6: Run aggregate gameplay regressions**

Run:

```bash
ctest --test-dir cmake-build-debug --output-on-failure -R '^(realtime_gameplay_worker_tests|gameplay_automatic_authority_tests|gameplay_simulation_tests)$'
git diff --check
```

Expected: all selected tests PASS and `git diff --check` emits no output.

- [ ] **Step 7: Commit Task 3**

```bash
git add CMakeLists.txt src/audio/Jukebox.h src/audio/Jukebox.cpp src/scene/play/CMakeLists.txt src/scene/play/GamePlayScene.h src/scene/play/GamePlayScene.cpp src/scene/play/ReplayKeysoundSchedule.h src/scene/play/ReplayKeysoundSchedule.cpp tests/replay_keysound_schedule_tests.cpp
git commit -m "fix: schedule replay keysounds independently of frames"
```
