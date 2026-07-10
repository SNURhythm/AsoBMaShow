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
  Stopwatch &stopwatch;
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
    state.stopwatch.reset();
    {
      std::lock_guard<std::mutex> positionLock(state.positionMutex);
      state.audioCursor = target.audio;
      state.bmpCursor = target.bmp;
      state.bmpLayerCursor = target.bmpLayer;
      std::invoke(std::forward<CommitAudio>(commitAudio));
    }
    state.isPlaying.store(true, std::memory_order_release);
    state.stopwatch.start();
    std::invoke(std::forward<WakeScheduler>(wakeScheduler));
  });
}

inline SeekState CaptureSeekState(const SessionState &state) {
  return {
      .wasPlaying = state.isPlaying.load(std::memory_order_acquire),
      .wasClockRunning = state.stopwatch.isRunning(),
  };
}

template <typename Lifecycle, typename WakeScheduler>
[[nodiscard]] audio::playback::BackendOperationResult
StopForSeek(Lifecycle &lifecycle, std::string_view context, SessionState &state,
            SeekState snapshot, WakeScheduler &&wakeScheduler) {
  return RunAfterConfirmedStop(lifecycle, context, [&] {
    if (snapshot.wasClockRunning) {
      state.stopwatch.pause();
    }
    std::invoke(std::forward<WakeScheduler>(wakeScheduler));
  });
}

template <typename CommitAudio, typename WakeScheduler>
void CommitStoppedSeek(SessionState &state, SeekState snapshot,
                       CursorPosition target, long long visualMicros,
                       CommitAudio &&commitAudio,
                       WakeScheduler &&wakeScheduler) {
  {
    std::lock_guard<std::mutex> positionLock(state.positionMutex);
    state.stopwatch.seek(visualMicros);
    state.audioCursor = target.audio;
    state.bmpCursor = target.bmp;
    state.bmpLayerCursor = target.bmpLayer;
    std::invoke(std::forward<CommitAudio>(commitAudio));
  }
  state.isPlaying.store(snapshot.wasPlaying, std::memory_order_release);
  if (snapshot.wasClockRunning) {
    state.stopwatch.resume();
  }
  std::invoke(std::forward<WakeScheduler>(wakeScheduler));
}

template <typename Lifecycle, typename CommitAudio, typename WakeScheduler>
[[nodiscard]] audio::playback::BackendOperationResult
RestartAndCommitSeek(Lifecycle &lifecycle, std::string_view context,
                     SessionState &state, SeekState snapshot,
                     CursorPosition target, long long visualMicros,
                     CommitAudio &&commitAudio, WakeScheduler &&wakeScheduler) {
  auto result = RunAfterConfirmedStart(lifecycle, context, [&] {
    state.isPlaying.store(snapshot.wasPlaying, std::memory_order_release);
    {
      std::lock_guard<std::mutex> positionLock(state.positionMutex);
      state.stopwatch.seek(visualMicros);
      state.audioCursor = target.audio;
      state.bmpCursor = target.bmp;
      state.bmpLayerCursor = target.bmpLayer;
      std::invoke(std::forward<CommitAudio>(commitAudio));
    }
    if (snapshot.wasClockRunning) {
      state.stopwatch.resume();
    }
    std::invoke(std::forward<WakeScheduler>(wakeScheduler));
  });
  if (!result.success) {
    state.isPlaying.store(false, std::memory_order_release);
    state.stopwatch.pause();
    std::invoke(std::forward<WakeScheduler>(wakeScheduler));
  }
  return result;
}

} // namespace jukebox_lifecycle
