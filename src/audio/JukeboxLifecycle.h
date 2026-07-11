#pragma once

#include "AudioMix.h"
#include "../utils/Stopwatch.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace jukebox_lifecycle {

struct CursorPosition {
  size_t audio = 0;
  size_t bmp = 0;
  size_t bmpLayer = 0;
};

struct ClockState {
  long long positionMicros = 0;
  bool running = true;
};

struct SessionState {
  std::atomic_bool &isPlaying;
  std::atomic_bool &schedulerActive;
  Stopwatch &stopwatch;
  std::mutex &transitionMutex;
  std::mutex &positionMutex;
  size_t &audioCursor;
  size_t &bmpCursor;
  size_t &bmpLayerCursor;
  std::atomic<int> &currentBga;
  std::atomic<int> &currentBmpLayer;
};

struct SeekState {
  bool wasPlaying = false;
  bool wasClockRunning = false;
};

inline void RestoreClockState(Stopwatch &stopwatch, ClockState clock) {
  stopwatch.pause();
  stopwatch.seek(clock.positionMicros);
  if (clock.running) {
    stopwatch.resume();
  }
}

inline audio::playback::BackendOperationResult
ContextualizeFailure(audio::playback::BackendOperationResult result,
                     std::string_view context, std::string_view operation) {
  if (result.success) {
    return result;
  }

  std::string diagnostic(context);
  diagnostic += " could not confirm audio ";
  diagnostic += operation;
  if (!result.diagnostic.empty()) {
    diagnostic += ": ";
    diagnostic += result.diagnostic;
  }
  result.diagnostic = std::move(diagnostic);
  return result;
}

template <typename Lifecycle, typename ConfirmedAction>
[[nodiscard]] audio::playback::BackendOperationResult
RunAfterConfirmedStop(Lifecycle &lifecycle, std::string_view context,
                      ConfirmedAction &&confirmedAction) {
  auto result = ContextualizeFailure(lifecycle.stopSounds(), context, "stop");
  if (result.success) {
    std::invoke(std::forward<ConfirmedAction>(confirmedAction));
  }
  return result;
}

template <typename Lifecycle, typename ConfirmedAction>
[[nodiscard]] audio::playback::BackendOperationResult
RunAfterConfirmedStart(Lifecycle &lifecycle, std::string_view context,
                       ConfirmedAction &&confirmedAction) {
  auto result = ContextualizeFailure(lifecycle.startDevice(), context, "start");
  if (result.success) {
    std::invoke(std::forward<ConfirmedAction>(confirmedAction));
  }
  return result;
}

template <typename Lifecycle>
[[nodiscard]] audio::playback::BackendOperationResult
RollbackStagedAudio(Lifecycle &lifecycle, std::string_view context,
                    audio::playback::BackendOperationResult failure) {
  const auto rollback =
      ContextualizeFailure(lifecycle.stopSounds(), context, "staged rollback");
  if (!rollback.success) {
    if (!failure.diagnostic.empty()) {
      failure.diagnostic += "; ";
    }
    failure.diagnostic += rollback.diagnostic;
  }
  return failure;
}

template <typename Lifecycle, typename WakeScheduler>
[[nodiscard]] audio::playback::BackendOperationResult
StopSessionForTransition(Lifecycle &lifecycle, std::string_view context,
                         SessionState &state, WakeScheduler &&wakeScheduler) {
  return RunAfterConfirmedStop(lifecycle, context, [&] {
    state.isPlaying.store(false, std::memory_order_release);
    state.schedulerActive.store(false, std::memory_order_release);
    state.stopwatch.pause();
    std::invoke(std::forward<WakeScheduler>(wakeScheduler));
  });
}

template <typename Lifecycle, typename WakeScheduler>
[[nodiscard]] audio::playback::BackendOperationResult
StopPlayback(Lifecycle &lifecycle, std::string_view context,
             SessionState &state, WakeScheduler &&wakeScheduler) {
  return RunAfterConfirmedStop(lifecycle, context, [&] {
    state.currentBga.store(-1, std::memory_order_relaxed);
    state.currentBmpLayer.store(-1, std::memory_order_relaxed);
    state.isPlaying.store(false, std::memory_order_release);
    state.schedulerActive.store(false, std::memory_order_release);
    state.stopwatch.pause();
    std::invoke(std::forward<WakeScheduler>(wakeScheduler));
  });
}

template <typename Lifecycle, typename CommitAudio, typename WakeScheduler>
[[nodiscard]] audio::playback::BackendOperationResult
StartPlayback(Lifecycle &lifecycle, std::string_view context,
              SessionState &state, CursorPosition target,
              CommitAudio &&commitAudio, WakeScheduler &&wakeScheduler,
              ClockState clock = {}) {
  return RunAfterConfirmedStart(lifecycle, context, [&] {
    {
      std::lock_guard<std::mutex> positionLock(state.positionMutex);
      state.stopwatch.reset();
      state.audioCursor = target.audio;
      state.bmpCursor = target.bmp;
      state.bmpLayerCursor = target.bmpLayer;
      std::invoke(std::forward<CommitAudio>(commitAudio));
      state.schedulerActive.store(true, std::memory_order_release);
      state.isPlaying.store(true, std::memory_order_release);
      RestoreClockState(state.stopwatch, clock);
    }
    std::invoke(std::forward<WakeScheduler>(wakeScheduler));
  });
}

template <typename Lifecycle, typename StageAudio, typename CommitAudio,
          typename WakeScheduler>
[[nodiscard]] audio::playback::BackendOperationResult
StartPlayback(Lifecycle &lifecycle, std::string_view context,
              SessionState &state, CursorPosition target,
              StageAudio &&stageAudio, CommitAudio &&commitAudio,
              WakeScheduler &&wakeScheduler, ClockState clock = {}) {
  auto staged = ContextualizeFailure(
      std::invoke(std::forward<StageAudio>(stageAudio)), context, "stage");
  if (!staged.success) {
    return RollbackStagedAudio(lifecycle, context, std::move(staged));
  }

  auto started =
      ContextualizeFailure(lifecycle.startDevice(), context, "start");
  if (!started.success) {
    return RollbackStagedAudio(lifecycle, context, std::move(started));
  }

  {
    std::lock_guard<std::mutex> positionLock(state.positionMutex);
    state.stopwatch.reset();
    state.audioCursor = target.audio;
    state.bmpCursor = target.bmp;
    state.bmpLayerCursor = target.bmpLayer;
    std::invoke(std::forward<CommitAudio>(commitAudio));
    state.schedulerActive.store(true, std::memory_order_release);
    state.isPlaying.store(true, std::memory_order_release);
    RestoreClockState(state.stopwatch, clock);
  }
  std::invoke(std::forward<WakeScheduler>(wakeScheduler));
  return started;
}

template <typename CommitAudio, typename WakeScheduler>
[[nodiscard]] audio::playback::BackendOperationResult
RestoreInactivePlayback(SessionState &state, ClockState clock,
                        CommitAudio &&commitAudio,
                        WakeScheduler &&wakeScheduler) {
  std::lock_guard<std::mutex> transitionLock(state.transitionMutex);
  std::lock_guard<std::mutex> positionLock(state.positionMutex);
  state.isPlaying.store(false, std::memory_order_release);
  state.schedulerActive.store(false, std::memory_order_release);
  state.stopwatch.pause();
  std::invoke(std::forward<CommitAudio>(commitAudio));
  RestoreClockState(state.stopwatch, clock);
  std::invoke(std::forward<WakeScheduler>(wakeScheduler));
  return {.success = true};
}

inline SeekState CaptureSeekState(const SessionState &state) {
  return {
      .wasPlaying = state.isPlaying.load(std::memory_order_acquire),
      .wasClockRunning = state.stopwatch.isRunning(),
  };
}

inline bool CanAdvanceSchedulerLocked(const SessionState &state) {
  return state.schedulerActive.load(std::memory_order_acquire) &&
         state.isPlaying.load(std::memory_order_acquire) &&
         state.stopwatch.isRunning();
}

template <typename ReadLiveTime>
long long ReadPublishedTime(SessionState &state, ReadLiveTime &&readLiveTime) {
  std::lock_guard<std::mutex> positionLock(state.positionMutex);
  if (state.isPlaying.load(std::memory_order_acquire)) {
    return std::invoke(std::forward<ReadLiveTime>(readLiveTime));
  }
  return state.stopwatch.elapsedMicros();
}

template <typename PlayKeySound>
bool PlayKeySoundIfPublished(SessionState &state, PlayKeySound &&playKeySound) {
  std::lock_guard<std::mutex> positionLock(state.positionMutex);
  if (!state.isPlaying.load(std::memory_order_acquire)) {
    return false;
  }
  std::invoke(std::forward<PlayKeySound>(playKeySound));
  return true;
}

template <typename Lifecycle, typename CommitAudio, typename WakeScheduler>
[[nodiscard]] audio::playback::BackendOperationResult
ExecuteSeekTransition(Lifecycle &lifecycle, std::string_view context,
                      SessionState &state, CursorPosition target,
                      long long visualMicros, CommitAudio &&commitAudio,
                      WakeScheduler &&wakeScheduler) {
  std::lock_guard<std::mutex> transitionLock(state.transitionMutex);
  std::lock_guard<std::mutex> positionLock(state.positionMutex);
  const auto snapshot = CaptureSeekState(state);

  if (!snapshot.wasPlaying) {
    state.stopwatch.seek(visualMicros);
    state.audioCursor = target.audio;
    state.bmpCursor = target.bmp;
    state.bmpLayerCursor = target.bmpLayer;
    std::invoke(std::forward<CommitAudio>(commitAudio), snapshot.wasPlaying);
    state.schedulerActive.store(false, std::memory_order_release);
    std::invoke(std::forward<WakeScheduler>(wakeScheduler));
    return {.success = true};
  }

  state.isPlaying.store(false, std::memory_order_release);
  state.stopwatch.pause();
  std::invoke(wakeScheduler);

  auto result = ContextualizeFailure(lifecycle.stopSounds(), context, "stop");
  if (result.success) {
    result = ContextualizeFailure(lifecycle.startDevice(), context, "start");
    if (result.success) {
      state.stopwatch.seek(visualMicros);
      state.audioCursor = target.audio;
      state.bmpCursor = target.bmp;
      state.bmpLayerCursor = target.bmpLayer;
      std::invoke(std::forward<CommitAudio>(commitAudio), snapshot.wasPlaying);
      if (snapshot.wasClockRunning) {
        state.stopwatch.resume();
      }
      state.schedulerActive.store(true, std::memory_order_release);
      state.isPlaying.store(true, std::memory_order_release);
      std::invoke(std::forward<WakeScheduler>(wakeScheduler));
      return result;
    }
  }

  state.schedulerActive.store(false, std::memory_order_release);
  state.isPlaying.store(false, std::memory_order_release);
  state.stopwatch.pause();
  std::invoke(std::forward<WakeScheduler>(wakeScheduler));
  return result;
}

template <typename Lifecycle, typename StageAudio, typename CommitAudio,
          typename WakeScheduler>
[[nodiscard]] audio::playback::BackendOperationResult
ExecuteSeekTransition(Lifecycle &lifecycle, std::string_view context,
                      SessionState &state, CursorPosition target,
                      long long visualMicros, StageAudio &&stageAudio,
                      CommitAudio &&commitAudio,
                      WakeScheduler &&wakeScheduler) {
  std::lock_guard<std::mutex> transitionLock(state.transitionMutex);
  std::lock_guard<std::mutex> positionLock(state.positionMutex);
  const auto snapshot = CaptureSeekState(state);

  if (!snapshot.wasPlaying) {
    state.stopwatch.seek(visualMicros);
    state.audioCursor = target.audio;
    state.bmpCursor = target.bmp;
    state.bmpLayerCursor = target.bmpLayer;
    std::invoke(std::forward<CommitAudio>(commitAudio), snapshot.wasPlaying);
    state.schedulerActive.store(false, std::memory_order_release);
    std::invoke(std::forward<WakeScheduler>(wakeScheduler));
    return {.success = true};
  }

  state.isPlaying.store(false, std::memory_order_release);
  state.stopwatch.pause();
  std::invoke(wakeScheduler);

  auto result = ContextualizeFailure(lifecycle.stopSounds(), context, "stop");
  if (result.success) {
    result = ContextualizeFailure(
        std::invoke(std::forward<StageAudio>(stageAudio)), context, "stage");
    if (!result.success) {
      result = RollbackStagedAudio(lifecycle, context, std::move(result));
    } else {
      result = ContextualizeFailure(lifecycle.startDevice(), context, "start");
      if (!result.success) {
        result = RollbackStagedAudio(lifecycle, context, std::move(result));
      } else {
        state.stopwatch.seek(visualMicros);
        state.audioCursor = target.audio;
        state.bmpCursor = target.bmp;
        state.bmpLayerCursor = target.bmpLayer;
        std::invoke(std::forward<CommitAudio>(commitAudio), snapshot.wasPlaying);
        if (snapshot.wasClockRunning) {
          state.stopwatch.resume();
        }
        state.schedulerActive.store(true, std::memory_order_release);
        state.isPlaying.store(true, std::memory_order_release);
        std::invoke(std::forward<WakeScheduler>(wakeScheduler));
        return result;
      }
    }
  }

  state.schedulerActive.store(false, std::memory_order_release);
  state.isPlaying.store(false, std::memory_order_release);
  state.stopwatch.pause();
  std::invoke(std::forward<WakeScheduler>(wakeScheduler));
  return result;
}

} // namespace jukebox_lifecycle
