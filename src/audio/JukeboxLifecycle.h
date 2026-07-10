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
              CommitAudio &&commitAudio, WakeScheduler &&wakeScheduler) {
  return RunAfterConfirmedStart(lifecycle, context, [&] {
    {
      std::lock_guard<std::mutex> positionLock(state.positionMutex);
      state.stopwatch.reset();
      state.audioCursor = target.audio;
      state.bmpCursor = target.bmp;
      state.bmpLayerCursor = target.bmpLayer;
      std::invoke(std::forward<CommitAudio>(commitAudio));
      state.schedulerActive.store(true, std::memory_order_release);
      state.stopwatch.start();
      state.isPlaying.store(true, std::memory_order_release);
    }
    std::invoke(std::forward<WakeScheduler>(wakeScheduler));
  });
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

} // namespace jukebox_lifecycle
